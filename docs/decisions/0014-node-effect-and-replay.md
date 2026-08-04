# ADR-0014：节点 Effect 与 Replay 语义

- 状态：Accepted / 1.0 Freeze
- 日期：2026-07-27

## 决策

副作用性质与重放安全性是两个独立维度：

- Effect kind：`pure`、`local_state`、`external_read`、`external_write`、`irreversible`；
- Replay policy：`safe`、`idempotent`、`at_most_once`、`manual`、`forbidden`。

1.0 前所有 `tool`、`custom` 节点必须显式声明 `effects`。`llm` 默认为
`external_read/safe`，远程状态变化必须覆盖。`transform`、`condition`、`noop` 默认为
`pure/safe`。宿主不得仅根据 node type 推断自定义 Executor 的副作用。

`idempotent` 必须提供稳定 idempotency key，或显式声明 Executor 保证幂等。Shadow Run 和
历史回放默认禁止 external write。未确认完成的 at-most-once 节点不得自动重跑；
irreversible/manual 必须由宿主确认；forbidden 在恢复、评估和 replay 中均不执行。

## 后果

Retry、Checkpoint、Replay、Shadow Evaluation 与自我优化共享同一安全语义。工作流作者需要
多写少量元数据，但 DAGE 不会通过猜测重复发送邮件、创建订单或修改生产资源。
