# 0.2.0 原型验收清单

- [x] C++17 工程、RAII、Value 复制/移动与 Pimpl。
- [x] 规范化 ResourceProvider、Bundle manifest、SHA-256 内容身份。
- [x] Workflow 编译为不可变类型化 IR；Run 不读取 Workflow JSON DOM。
- [x] SemVer Resolver、依赖 DAG、确定性 dage.lock 与四种加载模式。
- [x] Ed25519、多签名结构、KeyProvider、Keyring、TrustPolicy 和撤销测试。
- [x] UTF-8 JSON 解析、规范化、分层诊断和不可变 Workflow。
- [x] 12 种节点类型均可验证；所有内建/Executor 节点均有实际执行测试。
- [x] ordered `next` / `on_error`、otherwise、表达式与模板引用。
- [x] retry、协作 timeout、cancel、loop、max_hits、state set 基础结构。
- [x] parallel 真正并发分支调度、max_parallel、all/any/min_success 与 join。
- [x] Executor 注册、未注册错误、宿主 policy 拒绝。
- [x] Mermaid/DOT、format、validate/run/export CLI。
- [x] 稳定 C ABI、纯 C 头、opaque handle、异常边界、双阶段缓冲和 userdata 清理。
- [x] Python ctypes 示例绑定及共享库 smoke test。
- [x] Java JNI AutoCloseable 与 smoke test。
- [x] C# P/Invoke SafeHandle 与 smoke test。
- [x] Rust Drop/Result 绑定，实际 FFI 集成测试 1/1 通过。
- [x] Node-API AsyncWorker 原生 addon 与异步 smoke test；另有零编译 CLI adapter。
- [x] 不可变 revision/patch、全 action 分派、重新验证与保留旧 Workflow 回滚。
- [x] CTest 正常、错误、边界和资源释放测试。
- [x] Linux GCC ASan/UBSan/LSan：WSL Debug 构建与 CTest 2/2 通过。

本清单仅表示 0.2 原型验收完成，不表示 1.0 路线图或生产承诺已经完成。剩余工作以
[`TODO.md`](TODO.md) 为准。Windows MinGW 自身没有 sanitizer runtime；Linux/macOS 与
sanitizer CI 已配置，但只有实际 CI 运行结果才能作为对应平台的发布证据。
