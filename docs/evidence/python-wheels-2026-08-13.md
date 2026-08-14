# Python wheel matrix evidence — 2026-08-13

The first complete hosted `python-wheels` matrix passed for commit
`319ba670477e3aa0b8d6247c6764241a7bf3be59` in GitHub Actions run
[31680164259](https://github.com/sxf51/DAGE/actions/runs/31680164259).

Each job built CPython 3.11, 3.12 and 3.13 platform wheels, repaired their native dependency
closure, ran `tools/python_wheel_audit.py`, installed each wheel in cibuildwheel's clean test
environment, and ran both `bindings/python/conformance.py` and
`examples/python/service_host.py`. The tests do not set `DAGE_LIBRARY`, so success proves that the
installed wheel locates and loads its bundled runtime.

| Tier-1 target | Job | Retained artifact |
| --- | --- | --- |
| manylinux x86_64 | [ubuntu-22.04](https://github.com/sxf51/DAGE/actions/runs/31680164259/job/94383628817) | `python-wheels-ubuntu-22.04-unsigned` |
| manylinux arm64 | [ubuntu-24.04-arm](https://github.com/sxf51/DAGE/actions/runs/31680164259/job/94383628759) | `python-wheels-ubuntu-24.04-arm-unsigned` |
| macOS x86_64 | [macos-15-intel](https://github.com/sxf51/DAGE/actions/runs/31680164259/job/94383628686) | `python-wheels-macos-15-intel-unsigned` |
| macOS arm64 | [macos-14](https://github.com/sxf51/DAGE/actions/runs/31680164259/job/94383628720) | `python-wheels-macos-14-unsigned` |
| Windows x64 | [windows-2022](https://github.com/sxf51/DAGE/actions/runs/31680164259/job/94383628658) | `python-wheels-windows-x64-unsigned` |

The five artifact sets were present and unexpired when this evidence was recorded. They are CI
validation artifacts, not signed production releases. Publishing them to TestPyPI or PyPI remains
an explicit maintainer release action.

The companion full repository CI run
[31680164275](https://github.com/sxf51/DAGE/actions/runs/31680164275) also passed at the same commit,
including ASan/UBSan, TSAN, GCC/Clang/AppleClang/MinGW/MSVC, static-package consumers and all five
language-binding conformance suites.
