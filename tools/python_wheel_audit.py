"""Fail-closed structural audit for a DAGE platform wheel."""

import argparse
import csv
import hashlib
import io
import re
import sys
import zipfile
from pathlib import PurePosixPath


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel")
    arguments = parser.parse_args()
    wheel = PurePosixPath(arguments.wheel.replace("\\", "/")).name
    if wheel.endswith("-none-any.whl"):
        raise SystemExit("DAGE native runtime must not be published as a universal wheel")
    with zipfile.ZipFile(arguments.wheel) as archive:
        names = archive.namelist()
        if any(PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts
               for name in names):
            raise SystemExit("wheel contains an unsafe path")
        native = [name for name in names if re.search(
            r"^dage/_native/(?:lib)?dage\.(?:dll|so|dylib)$", name)]
        if len(native) != 1:
            raise SystemExit(f"expected exactly one DAGE runtime, found {native}")
        metadata_name = next(
            (name for name in names if name.endswith(".dist-info/METADATA")), None)
        record_name = next(
            (name for name in names if name.endswith(".dist-info/RECORD")), None)
        if not metadata_name or not record_name:
            raise SystemExit("wheel metadata or RECORD is missing")
        metadata = archive.read(metadata_name).decode("utf-8")
        for required in ("Name: dage-runtime", "Version: 0.2.0", "Requires-Python: >=3.11"):
            if required not in metadata:
                raise SystemExit(f"wheel metadata is missing {required!r}")
        records = {row[0]: row for row in csv.reader(
            io.StringIO(archive.read(record_name).decode("utf-8")))}
        for name in names:
            if name == record_name or name.endswith("/"):
                continue
            row = records.get(name)
            if row is None or not row[1].startswith("sha256="):
                raise SystemExit(f"wheel RECORD has no digest for {name}")
            digest = hashlib.sha256(archive.read(name)).digest()
            import base64
            encoded = base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")
            if row[1] != f"sha256={encoded}" or int(row[2]) != len(archive.read(name)):
                raise SystemExit(f"wheel RECORD mismatch for {name}")
    print(f"audited {wheel}: {len(names)} entries, native runtime {native[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
