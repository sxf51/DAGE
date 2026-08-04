# 线程与取消

- 不同 Engine 和不同 Run 可由不同线程执行。
- 同一 Run 的 `execute` 不允许并发调用；`cancel()` 是唯一线程安全的 Run 操作。
- Workflow 加载后不可变，可安全用于创建多个 Run。
- Executor 注册表在 `create_run` 时复制；之后修改 Engine 不影响已有 Run。
- 同步 Executor 通过 borrowed 原子 cancel token 协作取消。
- parallel 使用受 `limits.max_parallel` 限制的 Scheduler 任务；并发分支引用的 Executor 必须线程安全。
- parallel 分支使用隔离 child-run record 持久化 effect fence，不修改父 checkpoint；
  父 Run 汇总后只发布一次，避免观察者看到半合并状态。
- `register_async_executor` 接收线程安全的 one-shot completion。启动函数必须立即返回；
  等待宿主 I/O 时 Run 不占用 Scheduler worker。completion 可由任意宿主线程调用，随后
  DAGE 将 Run 重新投递到 Scheduler。Scheduler SPI 不接管宿主 I/O event loop。
- `Scheduler::schedule_continuation` 明确表示当前任务不会同步等待新任务。默认线程池允许
  worker 将这种任务放回有界队列，但普通 `schedule` 仍执行嵌套提交保护。自定义
  Scheduler 可覆盖该方法以映射到 event-loop wakeup 或 continuation queue。
- parallel 以不超过 `max_parallel` 的批次启动隔离 child-run。批次完成后由最后一个 child
  completion 重新投递父 Run，父 Run 汇总结果后才启动下一批或原子发布 join 状态。
- 异步 subflow 使用共享 child execution state 和弱 parent wakeup，不形成 parent/child
  `Run` 所有权环；父 Run 取消会传播到活动 child。
- 内置线程池的 worker 自持内部状态。即使宿主在 Run completion callback 中释放最后一个
  Engine、Run 或 Scheduler 引用，也不会发生 worker 自己 `join()` 自己或使用已释放队列。
- 所有异步节点 deadline 共用一个计时协调线程；计时线程只赢得 timeout/completion
  generation fence 并投递 continuation，不执行节点或用户 completion callback。
- 默认 `ThreadPoolScheduler` 使用有界队列，worker 数为
  `max(1, hardware_concurrency())`，队列容量默认为 `worker_count * 64`。
- `QueueFullPolicy` 支持 `Reject`、`Block`、`CallerRuns`；默认 `Reject`。`Block` 和普通
  caller-runs 必须由宿主显式启用。
- 可通过 `Engine(std::shared_ptr<Scheduler>)` 或 `set_scheduler` 注入宿主调度器。
- 默认策略拒绝从同池 worker 发起的嵌套调度，避免排队后发生线程饥饿死锁。显式
  `CallerRuns` 最多 inline `max_inline_depth` 层；1.0 前 Scheduler task metadata 还必须执行
  ADR-0012 的 effect、deadline、同 Run 和 reentrant 限制。
- 自定义 Scheduler 必须保证：任务至多执行一次、异常进入返回的 future、销毁前处理生命周期。
