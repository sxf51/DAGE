# DAGE

> DAGE is a small, embeddable, provider-agnostic DAG runtime for AI applications.

> **Development status:** DAGE is pre-1.0 and under active development. APIs, Workflow formats,
> Checkpoint formats, and binary layouts may change without compatibility guarantees. Current
> builds are evaluation artifacts and are not production releases; production use must wait for
> the documented 1.0 security, compatibility, and release gates.

DAGE（Directed Agent Graph Engine，大哥）是一个 MIT 许可的底层运行时库。它读取 JSON
工作流，验证并规范化 DAG，调度宿主提供的节点执行器，并产出可恢复、可比较的执行结果。
核心使用 C++17；Python、Java、C#、Rust 和 Node.js 等生态只依赖稳定 C ABI。

## 边界

DAGE Core 负责：

- 解析、规范化和验证工作流；
- 编译高效 IR（演进中）；
- 调度节点，管理数据流与控制流；
- 重试、超时、取消、暂停、恢复和重新运行；
- 采集可配置的运行轨迹；
- 显式应用 Workflow Patch；
- 比较版本表现并支持回滚；
- 提供 C++ API 和稳定 C ABI。

DAGE 不实现 LLM、Agent、工具、数据库、网络、文件系统、身份认证、服务端或控制台。这些能力由
宿主通过 Executor、ResourceProvider、Scheduler 和 Trace Sink 等 SPI 提供。

## 当前状态

0.2 原型已经实现 JSON 协议、12 类节点、不可变 IR、Bundle/lock/signature、并行与子流程、
可靠恢复、Trace 管道、资源租约、Patch、CLI、C ABI 和五种语言绑定的基础能力。当前正在按
[1.0 TODO](docs/TODO.md) 消除异步执行、持久性、版本比较、ABI、性能和生产验证差距。
1.0 前允许清理 API；1.0 后 C ABI 只增量扩展，既有状态码和结构字段不重新编号、不改变语义。

## 构建

需要 CMake、C++17 编译器和 jsoncpp：

```bash
cmake -S . -B build -DDAGE_BUILD_SHARED=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

运行完整节点示例：

```bash
./build/dage validate examples/workflows/all_nodes.json
./build/dage run examples/workflows/all_nodes.json '{"prompt":"hello"}'
```

Windows 多配置生成器的可执行文件通常位于 `build/Debug` 或 `build/Release`。

## 文档

- [DAGE 1.0 持续维护 TODO](docs/TODO.md)
- [使用与示例](docs/examples.md)
- [Bundle、ResourceProvider 与不可变 IR](docs/bundle.md)
- [Bundle Resolver、dage.lock 与 Ed25519](docs/bundle_resolver.md)
- [Key Scope, Rotation, Revocation, and Signature Policy](docs/key_trust.md)
- [Workflow Diff 与 Bundle Rollback](docs/workflow_diff_and_rollback.md)
- [Trace Replay 与 Shadow Evaluation](docs/replay_and_evaluation.md)
- [Directory/ZIP Bundle Tools](docs/bundle_tools.md)
- [Atomic Lockfile, Bundle Cache, and Verified Production Profile](docs/bundle_store.md)
- [Canonical Bundle Golden Vectors](docs/canonical_bundle.md)
- [可靠执行、Effect 与恢复](docs/reliable_execution.md)
- [可靠性运行时 SPI、deadline、commit fence 与 selective rerun](docs/reliability_runtime.md)
- [可观测且可扩展的执行内核](docs/observable_kernel.md)
- [Trace 管道、资源租约与 OpenTelemetry 桥接](docs/trace_pipeline_and_leases.md)
- [产品范围与宿主边界](docs/product_scope.md)
- [完整路线图与决策点](docs/roadmap.md)
- [JSON 工作流协议](docs/pipeline_protocol.md)
- [C++ API](docs/cpp_api.md)
- [C ABI](docs/c_api.md)
- [C Runtime SPI](docs/c_runtime_spi.md)
- [C Bundle Resolver and Trust SPI](docs/c_bundle_resolver.md)
- [C Analysis, Replay, and Evaluation API](docs/c_analysis_replay.md)
- [C ABI Capabilities, Allocator, and Threading Registry](docs/c_abi_registry.md)
- [C ABI compatibility gate](docs/c_abi_compatibility.md)
- [平台与语言兼容矩阵](docs/compatibility_matrix.md)
- [Workflow 解析与编译资源限制](docs/workflow_resource_limits.md)
- [多语言绑定](docs/bindings.md)
- [测试与验收](docs/testing.md)
- [性能基准](docs/benchmarks.md)
- [Fuzzing、属性模型与并发压力测试](docs/fuzzing_and_stress.md)
- [Failure 与竞态矩阵](docs/failure_matrix.md)
- [安装、外部消费与发行打包](docs/packaging.md)
- [威胁模型与安全审查边界](docs/threat_model.md)
- [生产运行指南](docs/operations.md)
- [支持、LTS 与发行流程](docs/support_and_release.md)
- [Ed25519 运行时发行 Manifest](docs/release_manifest.md)
- [架构决策记录](docs/decisions/README.md)
- [贡献指南与 DCO](CONTRIBUTING.md)
- [安全策略](SECURITY.md)
- [项目治理](GOVERNANCE.md)
- [商标策略](TRADEMARKS.md)

## 许可证

[MIT](LICENSE)。
