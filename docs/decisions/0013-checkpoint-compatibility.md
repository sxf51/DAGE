# ADR-0013：Checkpoint 兼容性

- 状态：Accepted / 1.0 Freeze
- 日期：2026-07-27

## 决策

DAGE 当前处于 pre-1.0。现有原型 Checkpoint、协议、方法和能力均不构成兼容承诺，可以直接
替换，不实现旧格式 reader、迁移器或 deprecated 入口。本 ADR 的兼容规则仅从 1.0.0 起生效。

DAGE 1.x Reader 必须读取并迁移较早 1.x 写出的 Checkpoint；Writer 只写当前格式。兼容能力
分为 `decode`、`migrate`、`inspect` 和 `resume`，解析成功不代表允许恢复。

- 当前版本：完整恢复；
- 较旧同 major：必须读取和迁移；
- 较新同 major：可 inspect 已知部分，默认不得自动 resume；
- 不同 major：默认拒绝，允许离线迁移工具；
- Executor 状态不兼容：可 inspect，不得 resume；
- Workflow 或 Bundle lock digest 不一致：默认不得 resume。

头部包含 format、format_version、runtime_version、workflow_digest、bundle_lock_digest、
created_at 和 required_capabilities。恢复必须检查必需能力、状态 schema、Executor 类型、
Effect/Replay 状态和 in-flight task 的可恢复性。

## 规范承诺

以下承诺的适用范围仅为 1.0.0 及之后：

> DAGE 1.x MUST read and migrate checkpoints written by an earlier 1.x release. It MAY inspect,
> but MUST NOT automatically resume, checkpoints written by a later release unless all required
> capabilities are supported.
