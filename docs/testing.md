# 构建与验证

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DDAGE_BUILD_SHARED=OFF
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Release and CI builds enable strict diagnostics for every DAGE-owned target:

```powershell
cmake -S . -B build-warnings -G "MinGW Makefiles" `
  -DDAGE_BUILD_SHARED=ON `
  -DDAGE_BUILD_TESTS=ON `
  -DDAGE_BUILD_TOOLS=ON `
  -DDAGE_BUILD_EXAMPLES=ON `
  -DDAGE_BUILD_BENCHMARKS=ON `
  -DDAGE_WARNINGS_AS_ERRORS=ON
cmake --build build-warnings -j 4
```

This applies `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic -Werror` on GCC, Clang,
AppleClang, and MinGW. The option affects only targets built in this project; it is not exported as
a usage requirement to applications linking DAGE.

The optional benchmark smoke gate is enabled with `-DDAGE_BUILD_BENCHMARKS=ON` and can be selected
independently:

```powershell
ctest --test-dir build -L benchmark --output-on-failure
```

See [Performance benchmarks](benchmarks.md) for Release measurement commands and scope.
See [Fuzzing, property models, and concurrency stress](fuzzing_and_stress.md) for libFuzzer,
ThreadSanitizer, generated-DAG model checks, and extended soak commands.
See [Failure and race matrix](failure_matrix.md) for durable-write, corruption, hostile-SPI,
cancellation, and timeout evidence.
See [Installing and packaging DAGE](packaging.md) for the external shared/static consumer gates,
Tier-1 architecture matrix, archive contents, and signing boundary.

Dependency discovery prefers standard CMake package targets for jsoncpp, OpenSSL, and zlib, then
falls back to pkg-config where needed. `vcpkg.json` supplies the MSVC manifest build; Unix and
MinGW distribution packages remain supported.

Sanitizer（GCC/Clang 平台）：

```powershell
cmake -S . -B build-san -G "MinGW Makefiles" -DDAGE_BUILD_SHARED=OFF -DDAGE_ENABLE_SANITIZERS=ON
cmake --build build-san -j 4
ctest --test-dir build-san --output-on-failure
```

Core 测试仍由一个 `dage_tests` 二进制承载，以避免重复编译大型 fixture，但 CTest 会为
21 个语义套件分别启动进程。每个 `dage_core_*` 测试有 `core` label、60 秒硬超时和独立
开始/完成诊断；一个套件挂起不会遮蔽其余结果。运行全部 Core 或单个套件：

```powershell
ctest --test-dir build -L core --output-on-failure -j 4
.\build\dage_tests.exe state_store_and_async
```

不带参数直接运行 `dage_tests` 仍会顺序执行全部套件；未知套件返回状态码 2 并列出合法名称。
这些套件覆盖 Value、Result、默认线程池、自定义/拒绝 Scheduler、所有 12 种节点格式、
Bundle/IR、SemVer Resolver、依赖循环/冲突、lock 漂移、offline cache miss、真实 Ed25519
签名、TrustPolicy、key revocation、内容篡改、非法 JSON、引用、分支、循环、表达式、
Executor、policy、retry、parallel/join 和导出转义。`dage_c_header_test` 由纯 C 编译，
覆盖 create/destroy、Executor 单次调用、owned output/release、userdata 生命周期、
deadline、effect commit 和公共 API 双阶段缓冲区。
`dage_recovery_stress` 连续启动 12 个工作进程，在持久化后用 `_Exit` 模拟崩溃，再由新
运行时恢复并验证外部写不重复。单元测试还覆盖 StateStore CAS 冲突和异步 Executor。

Windows MinGW 未提供 sanitizer runtime；已在 WSL Ubuntu、GCC 13.3 上使用
`ASAN_OPTIONS=detect_leaks=1` 完成 ASan/UBSan/LSan 构建。Windows shared-library 完整
CTest 当前为 29/29 通过；sanitizer 结果由 Linux CI 持续验证。

异步测试 Executor 不允许 `detach()` 宿主线程。测试必须显式拥有并 join 线程，确保 Run
完成不被误当成宿主回调栈已经退出。TimerCoordinator 由 Engine/Run 共享持有，在最后一个
所有者释放时正常停止；它不是 DLL 静态析构对象，因此不会在 Windows loader lock 中等待
工作线程。

绑定验证：

```powershell
python examples\python\run.py
dotnet run --project bindings\csharp\Dage.csproj -c Release
java "-Djava.library.path=build-java" -cp bindings\java\out io.dage.Smoke
cargo test --manifest-path bindings\rust\Cargo.toml
npm.cmd run build:native --prefix bindings\node
node bindings\node\native-smoke.js
```
