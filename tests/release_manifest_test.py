import pathlib
import subprocess
import sys
import tempfile


def run(*arguments, expect=0):
    completed = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if completed.returncode != expect:
        raise RuntimeError(
            "unexpected exit {} for {}\n{}".format(
                completed.returncode, arguments, completed.stderr.decode("utf-8", "replace")
            )
        )


def main():
    script = pathlib.Path(sys.argv[1]).resolve()
    openssl = sys.argv[2]
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        artifact = root / "DAGE-0.2.0-test.zip"
        artifact.write_bytes(b"deterministic release artifact")
        inventory = root / "dage-dependencies.json"
        inventory.write_text(
            '{"build":{"compiler":"test"},"dage_version":"0.2.0",'
            '"dependencies":[{"name":"openssl","required_by":["dage"],"version":"3.6.1"}],'
            '"format":"dage-dependency-inventory","format_version":1}\n',
            encoding="utf-8",
        )
        private_key = root / "release-private.pem"
        public_key = root / "release-public.pem"
        manifest = root / "release-manifest.json"
        signature = root / "release-manifest.ed25519"
        run(openssl, "genpkey", "-algorithm", "Ed25519", "-out", str(private_key))
        run(openssl, "pkey", "-in", str(private_key), "-pubout", "-out", str(public_key))
        common = (sys.executable, str(script))
        run(
            *common,
            "create",
            "--version", "0.2.0",
            "--source-revision", "0123456789abcdef0123456789abcdef01234567",
            "--builder", "local-release-test",
            "--target", "windows-x86_64",
            "--key-id", "test-only",
            "--artifact", str(artifact),
            "--dependency-inventory", str(inventory),
            "--output", str(manifest),
        )
        inventory.write_text(
            inventory.read_text(encoding="utf-8").replace(
                '"dage_version":"0.2.0"', '"dage_version":"9.9.9"'
            ),
            encoding="utf-8",
        )
        run(
            *common,
            "create",
            "--version", "0.2.0",
            "--source-revision", "0123456789abcdef0123456789abcdef01234567",
            "--builder", "local-release-test",
            "--target", "windows-x86_64",
            "--key-id", "test-only",
            "--artifact", str(artifact),
            "--dependency-inventory", str(inventory),
            "--output", str(manifest),
            expect=1,
        )
        inventory.write_text(
            inventory.read_text(encoding="utf-8").replace(
                '"dage_version":"9.9.9"', '"dage_version":"0.2.0"'
            ),
            encoding="utf-8",
        )
        first = manifest.read_bytes()
        run(
            *common,
            "create",
            "--version", "0.2.0",
            "--source-revision", "0123456789abcdef0123456789abcdef01234567",
            "--builder", "local-release-test",
            "--target", "windows-x86_64",
            "--key-id", "test-only",
            "--artifact", str(artifact),
            "--dependency-inventory", str(inventory),
            "--output", str(manifest),
        )
        if manifest.read_bytes() != first:
            raise RuntimeError("release manifest is not deterministic")
        run(*common, "sign", "--openssl", openssl, "--manifest", str(manifest),
            "--private-key", str(private_key), "--signature", str(signature))
        run(*common, "verify", "--openssl", openssl, "--manifest", str(manifest),
            "--public-key", str(public_key), "--signature", str(signature),
            "--expected-key-id", "test-only",
            "--artifact-dir", str(root))
        run(*common, "verify", "--openssl", openssl, "--manifest", str(manifest),
            "--public-key", str(public_key), "--signature", str(signature),
            "--expected-key-id", "unexpected-key", expect=1)
        original_signature = signature.read_bytes()
        signature.write_bytes(bytes([original_signature[0] ^ 1]) + original_signature[1:])
        run(*common, "verify", "--openssl", openssl, "--manifest", str(manifest),
            "--public-key", str(public_key), "--signature", str(signature),
            "--expected-key-id", "test-only", expect=1)
        signature.write_bytes(original_signature)
        artifact.write_bytes(b"tampered")
        run(*common, "verify", "--openssl", openssl, "--manifest", str(manifest),
            "--public-key", str(public_key), "--signature", str(signature),
            "--expected-key-id", "test-only",
            "--artifact-dir", str(root), expect=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
