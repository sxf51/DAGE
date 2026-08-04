# ADR-0012：Scheduler 饱和策略

- 状态：Accepted / 1.0 Freeze
- 日期：2026-07-27

## 决策

默认线程池：

- worker 数为 `max(1, hardware_concurrency())`；
- queue capacity 为 `worker_count * 64`；
- 默认队列满策略为 `Reject`，返回 `resource_exhausted`；
- `Block` 和一般 `CallerRuns` 只能显式启用；
- 外部调用线程默认禁止 caller-runs。

同池 Worker 的嵌套 caller-runs 只有在同一 Run 依赖解除路径、节点非 blocking/external、
未取消、deadline 未到、Executor 声明 reentrant 且 inline depth 小于限制时允许。
默认 `max_inline_depth=1`。无法证明任一条件时必须拒绝，不得猜测。

## 后果

避免 Python GIL、JNI、Node 主线程、回调重入、栈增长和优先级反转。Scheduler SPI 后续需要
携带 run、effect、deadline、reentrant 等提交提示；0.2 当前实现的嵌套 inline 是过渡实现，
在 1.0 前必须收紧到本 ADR。
