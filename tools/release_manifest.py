#!/usr/bin/env python3
"""Create and verify deterministic, Ed25519-signed DAGE release manifests."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile


def fail(message):
    raise ValueError(message)


def canonical_bytes(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def digest_file(path):
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            size += len(block)
            digest.update(block)
    return size, digest.hexdigest()


def validate_dependency_inventory(path):
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("format") != "dage-dependency-inventory" or document.get("format_version") != 1:
        fail("unsupported dependency inventory format")
    if set(document) != {"build", "dage_version", "dependencies", "format", "format_version"}:
        fail("dependency inventory fields do not match format version 1")
    if not isinstance(document["build"], dict) or not isinstance(document["dage_version"], str):
        fail("invalid dependency inventory build identity")
    dependencies = document["dependencies"]
    if not isinstance(dependencies, list) or not dependencies:
        fail("dependency inventory has no dependencies")
    names = set()
    for dependency in dependencies:
        if not isinstance(dependency, dict):
            fail("invalid dependency inventory entry")
        name = dependency.get("name")
        version = dependency.get("version")
        enabled = dependency.get("enabled", True)
        if not isinstance(name, str) or not name or name in names:
            fail("invalid or duplicate dependency inventory name")
        names.add(name)
        if not isinstance(enabled, bool):
            fail("invalid dependency enabled state: " + name)
        if enabled and (not isinstance(version, str) or version in ("", "unknown", "not-enabled")):
            fail("enabled dependency has no exact version: " + name)
        required_by = dependency.get("required_by")
        if not isinstance(required_by, list) or not all(isinstance(value, str) and value for value in required_by):
            fail("invalid dependency consumers: " + name)
    return document


def atomic_write(path, data):
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
        if os.name == "posix":
            directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def load_canonical_manifest(path):
    raw = path.read_bytes()
    document = json.loads(raw.decode("utf-8"))
    if raw != canonical_bytes(document):
        fail("release manifest is not canonical JSON")
    if document.get("format") != "dage-release-manifest" or document.get("format_version") != 1:
        fail("unsupported release manifest format")
    expected_fields = {
        "artifacts", "builder", "format", "format_version", "key_id",
        "source_revision", "target", "version",
    }
    if set(document) != expected_fields:
        fail("release manifest fields do not match format version 1")
    validate_identity("builder", document["builder"])
    validate_identity("key_id", document["key_id"])
    validate_identity("target", document["target"])
    validate_version(document["version"])
    validate_revision(document["source_revision"])
    artifacts = document.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        fail("release manifest has no artifacts")
    names = set()
    for artifact in artifacts:
        if set(artifact) != {"name", "role", "sha256", "size"}:
            fail("release artifact fields do not match format version 1")
        name = artifact.get("name")
        if not isinstance(name, str) or pathlib.PurePath(name).name != name or name in (".", ".."):
            fail("artifact name must be a single portable filename")
        if name in names:
            fail("duplicate artifact name: " + name)
        names.add(name)
        if artifact.get("role") not in ("runtime", "dependency-inventory"):
            fail("invalid release artifact role: " + name)
        if not isinstance(artifact.get("size"), int) or artifact["size"] < 0:
            fail("invalid artifact size: " + name)
        digest = artifact.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64 or digest != digest.lower():
            fail("invalid artifact digest: " + name)
        int(digest, 16)
    if sum(artifact["role"] == "dependency-inventory" for artifact in artifacts) != 1:
        fail("release manifest requires exactly one dependency inventory")
    return document


def validate_identity(name, value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._:/+@-]{0,127}", value):
        fail("invalid " + name)


def validate_version(value):
    if not isinstance(value, str) or not re.fullmatch(
        r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
        r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
        r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?",
        value,
    ):
        fail("version must be SemVer")


def validate_revision(value):
    if not isinstance(value, str) or not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", value):
        fail("source_revision must be a full lowercase SHA-1 or SHA-256 object ID")


def create(args):
    validate_version(args.version)
    validate_revision(args.source_revision)
    validate_identity("builder", args.builder)
    validate_identity("target", args.target)
    validate_identity("key_id", args.key_id)
    artifacts = []
    names = set()
    inputs = [(value, "runtime") for value in args.artifact]
    inputs.append((args.dependency_inventory, "dependency-inventory"))
    for value, role in inputs:
        path = pathlib.Path(value).resolve()
        if not path.is_file() or path.is_symlink():
            fail("artifact is not a regular file: " + str(path))
        if path.name in names:
            fail("duplicate artifact filename: " + path.name)
        names.add(path.name)
        if role == "dependency-inventory":
            inventory = validate_dependency_inventory(path)
            if inventory["dage_version"] != args.version:
                fail("dependency inventory DAGE version does not match release version")
        size, digest = digest_file(path)
        artifacts.append({"name": path.name, "role": role, "sha256": digest, "size": size})
    artifacts.sort(key=lambda item: item["name"].encode("utf-8"))
    if sum(item["role"] == "dependency-inventory" for item in artifacts) != 1:
        fail("release manifest requires exactly one dependency inventory")
    document = {
        "artifacts": artifacts,
        "builder": args.builder,
        "format": "dage-release-manifest",
        "format_version": 1,
        "key_id": args.key_id,
        "source_revision": args.source_revision,
        "target": args.target,
        "version": args.version,
    }
    atomic_write(pathlib.Path(args.output), canonical_bytes(document))


def openssl(args, *command):
    completed = subprocess.run(
        [args.openssl, "pkeyutl", *command],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        fail("OpenSSL Ed25519 operation failed: " + completed.stderr.decode("utf-8", "replace").strip())


def sign(args):
    manifest = pathlib.Path(args.manifest).resolve()
    load_canonical_manifest(manifest)
    output = pathlib.Path(args.signature).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="." + output.name + ".", dir=output.parent)
    os.close(descriptor)
    try:
        openssl(args, "-sign", "-rawin", "-inkey", args.private_key, "-in", str(manifest), "-out", temporary_name)
        signature = pathlib.Path(temporary_name).read_bytes()
        if len(signature) != 64:
            fail("Ed25519 signature must be exactly 64 bytes")
        atomic_write(output, signature)
    finally:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass


def verify(args):
    manifest_path = pathlib.Path(args.manifest).resolve()
    document = load_canonical_manifest(manifest_path)
    if document["key_id"] != args.expected_key_id:
        fail("release manifest key_id does not match the pinned expected key")
    signature_path = pathlib.Path(args.signature).resolve()
    if signature_path.stat().st_size != 64:
        fail("Ed25519 signature must be exactly 64 bytes")
    openssl(
        args,
        "-verify",
        "-rawin",
        "-pubin",
        "-inkey",
        args.public_key,
        "-sigfile",
        str(signature_path),
        "-in",
        str(manifest_path),
    )
    if args.artifact_dir:
        root = pathlib.Path(args.artifact_dir).resolve()
        for artifact in document["artifacts"]:
            path = root / artifact["name"]
            if not path.is_file() or path.is_symlink():
                fail("release artifact is missing: " + artifact["name"])
            size, digest = digest_file(path)
            if size != artifact["size"] or digest != artifact["sha256"]:
                fail("release artifact digest mismatch: " + artifact["name"])
            if artifact["role"] == "dependency-inventory":
                inventory = validate_dependency_inventory(path)
                if inventory["dage_version"] != document["version"]:
                    fail("dependency inventory DAGE version does not match release manifest")


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    subcommands = result.add_subparsers(dest="command", required=True)
    create_parser = subcommands.add_parser("create")
    create_parser.add_argument("--version", required=True)
    create_parser.add_argument("--source-revision", required=True)
    create_parser.add_argument("--builder", required=True)
    create_parser.add_argument("--target", required=True)
    create_parser.add_argument("--key-id", required=True)
    create_parser.add_argument("--artifact", action="append", required=True)
    create_parser.add_argument("--dependency-inventory", required=True)
    create_parser.add_argument("--output", required=True)
    create_parser.set_defaults(handler=create)
    for name, handler in (("sign", sign), ("verify", verify)):
        operation = subcommands.add_parser(name)
        operation.add_argument("--manifest", required=True)
        operation.add_argument("--signature", required=True)
        operation.add_argument("--openssl", default="openssl")
        operation.set_defaults(handler=handler)
    subcommands.choices["sign"].add_argument("--private-key", required=True)
    subcommands.choices["verify"].add_argument("--public-key", required=True)
    subcommands.choices["verify"].add_argument("--expected-key-id", required=True)
    subcommands.choices["verify"].add_argument("--artifact-dir")
    return result


def main():
    try:
        arguments = parser().parse_args()
        arguments.handler(arguments)
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("release manifest error: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
