"""Hermetic source checks for Python distribution metadata and release invariants."""

import re
import sys
from pathlib import Path


def main(root):
    root = Path(root)
    python_root = root / "bindings" / "python"
    pyproject = (python_root / "pyproject.toml").read_text(encoding="utf-8")
    version_match = re.search(r'^version\s*=\s*"([^"]+)"\s*$', pyproject, re.MULTILINE)
    if version_match is None:
        raise RuntimeError("Python package version is missing or not a string literal")
    version = version_match.group(1)
    if (python_root / "LICENSE").read_bytes() != (root / "LICENSE").read_bytes():
        raise RuntimeError("Python wheel license drifted from the repository license")
    module = (python_root / "src" / "dage" / "__init__.py").read_text(encoding="utf-8")
    header = (root / "include" / "dage" / "dage.h").read_text(encoding="utf-8")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    expected = [
        f'__version__ = "{version}"',
        f'#define DAGE_RUNTIME_VERSION_STRING "{version}"',
        f"project(DAGE VERSION {version}",
    ]
    for marker, source in zip(expected, (module, header, cmake)):
        if marker not in source:
            raise RuntimeError(f"release version drift: missing {marker}")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise RuntimeError("Python package version is not canonical SemVer")
    setup = (python_root / "setup.py").read_text(encoding="utf-8")
    if "DAGE_WHEEL_LIBRARIES" not in setup or "glob(" in setup:
        raise RuntimeError("native wheel staging must use an explicit file closure")
    print(f"Python packaging invariants passed for {version}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
