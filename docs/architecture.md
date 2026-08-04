# DAGE 架构

DAGE 0.2.0 使用固定流水线：
`ResourceProvider → Bundle Loader → JSON Parser → Normalizer → Validator → Immutable Workflow IR → Runtime`。
公共头文件不暴露 JsonCpp；`Workflow` 编译后不可复制，`Run` 持有运行所需 IR 快照和
Executor 注册表快照，执行期间不读取原始 JSON 文本或 Workflow JSON DOM。

## 模块边界

- `Value`：C++17 Pimpl JSON 值，支持复制和移动。
- `Engine`：注册 Executor、宿主策略和事件回调，验证并加载工作流。
- `Workflow`：不可变的规范化图，可导出 Mermaid 和 DOT。
- `Bundle`：规范化资源集合、canonical manifest、命名 Workflow 和 SHA-256 内容身份。
- `ResourceProvider`：Core 唯一资源入口；目录、ZIP、网络和数据库由宿主/官方工具实现。
- `WorkflowIR`：类型化只读节点、边、Effect/Replay 和调度约束；Run 的唯一静态图输入。
- `BundleResolver`：解析 SemVer 依赖 DAG，生成/执行 `dage.lock`，实施加载模式。
- `KeyProvider` / `TrustPolicy`：把 Ed25519 密码学验证与宿主信任裁决分离。
- `Run`：状态、节点输出、错误、重试、边命中次数、循环计数和原子取消标志。
- C ABI：参数检查、opaque handle、异常转状态码和双阶段缓冲区；不实现图语义。
- Python：`ctypes` 生命周期和字符串适配；不重新实现图语义。

`parallel` 使用 Scheduler SPI 并受 `limits.max_parallel` 分批限制；默认实现为有界线程池。分支结果写入各自
`nodes.<branch>.output` 后进入 join；支持 all、any 与 min_success 成功策略。
`subflow` 通过 `Engine::register_workflow` 注册命名工作流，并在隔离的子 Run 中执行；Core
不自行读取任意文件。父流程只能读取 subflow 的最终公开输出。
`human` Executor 可挂起运行；checkpoint 序列化控制状态，进程重启后通过 restore/resume
注入审批结果并继续。0.2 Checkpoint 使用 `dage-checkpoint` 头部并绑定 Workflow IR 的
SHA-256 digest；不读取旧整数版本格式。Checkpoint 不保存 Executor、权限或密钥。

## 所有权

- `Engine`、`Workflow`、`Run` 的 C++ 工厂结果为 transferred `unique_ptr`。
- ExecutionContext 和回调输入在回调期间 borrowed。
- C handle 必须由同名 `destroy` 释放；Engine 必须晚于其 Run 销毁。
- C Executor userdata 由 Engine 持有，替换注册或销毁 Engine 时调用 destroy callback。
- C ABI 输出由调用方分配；第一次调用取得包含 NUL 的 `required_size`。

## 安全边界

协议不执行脚本。表达式只读取运行上下文，支持比较、布尔运算以及白名单函数。Executor 是否
可用由宿主注册表和 policy 决定。JSON 大小限制为 16 MiB，节点上限为 10000，运行受
`max_steps`、工作流 timeout、节点 timeout、loop 和 `max_hits` 限制。
