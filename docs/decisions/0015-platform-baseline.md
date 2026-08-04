# ADR-0015：DAGE 1.0 平台基线

- 状态：Accepted / 1.0 Freeze
- 日期：2026-07-27

## 决策

DAGE 的语言基线是 **C++17**；C ABI 头保持 C99-compatible。优先支持 64 位 little-endian。

Tier 1（CI 必测并发布二进制）：

| 平台 | 架构 | 最低系统 |
| --- | --- | --- |
| Linux | x86_64 / arm64 | glibc 2.28+ |
| Windows | x86_64 | Windows 10 1809+ |
| macOS | x86_64 / arm64 | macOS 11+ |

最低工具链：GCC 9.4、Clang 10、AppleClang/Xcode 12、MSVC 2019 16.8、CMake 3.16。

Tier 2 为 musl Linux、FreeBSD、Windows arm64、Android NDK、iOS 和 MinGW。1.0 暂不承诺
32 位、big-endian、无线程环境及更旧工具链。

## 后果

支持边界以可维护的 sanitizer、原子操作和标准库为准，而不是以 C++17 语法的理论最老编译器
为准。Tier 2 可接受社区修复，但不阻塞每次发布。
