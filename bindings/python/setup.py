"""Binary-wheel staging hooks; project metadata remains in pyproject.toml."""

import os
import shutil
import sys
from pathlib import Path

from setuptools import Distribution, setup
from setuptools.command.build_py import build_py


class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True


class BuildPy(build_py):
    def run(self):
        super().run()
        libraries = os.environ.get("DAGE_WHEEL_LIBRARIES", "")
        paths = [Path(value) for value in libraries.split(os.pathsep) if value]
        # Preserve the canonical loader filename even when it is a build-tree symlink.
        paths = list(dict.fromkeys(Path(os.path.abspath(path)) for path in paths))
        if not paths:
            raise RuntimeError(
                "DAGE_WHEEL_LIBRARIES must list the native runtime and its private dependencies"
            )
        runtime_names = (
            {"libdage.dll", "dage.dll"} if sys.platform == "win32"
            else ({"libdage.dylib"} if sys.platform == "darwin" else {"libdage.so"})
        )
        runtimes = [path for path in paths if path.name in runtime_names]
        if len(runtimes) != 1:
            raise RuntimeError(
                f"DAGE_WHEEL_LIBRARIES must contain exactly one canonical runtime; found {runtimes}"
            )
        destination = Path(self.build_lib) / "dage" / "_native"
        destination.mkdir(parents=True, exist_ok=True)
        for source in paths:
            if not source.is_file():
                raise RuntimeError(f"wheel library does not exist: {source}")
            shutil.copy2(source, destination / source.name)


setup(distclass=BinaryDistribution, cmdclass={"build_py": BuildPy})
