# C++17 API

引入 `#include <dage/dage.hpp>`，链接 `dage`。

```cpp
dage::Engine engine;
engine.register_executor(
    "echo",
    [](const dage::ExecutionContext&, const dage::Value& input) {
        return dage::ExecutionResult::ok(input);
    });

std::unique_ptr<dage::Workflow> workflow = engine.load(json);
std::unique_ptr<dage::Run> run = engine.create_run(*workflow);
dage::ExecutionResult result = run->execute(dage::Value::parse(input_json));
```

节点普通失败通过 `ExecutionResult::fail` 返回，不使用异常控制流程。解析、验证、无效 API 参数
和不可恢复错误可抛出异常。`set_executor_policy` 是宿主权限门；`set_event_callback` 可观察
`node_started`、`node_succeeded` 和 `node_failed`。

Executor 可使用同步 `register_executor`，也可使用 completion 驱动的
`register_async_executor`。异步启动函数接收 one-shot `AsyncExecutorCompletion`，启动宿主
I/O 后立即返回。Run 保存当前节点并归还 Scheduler worker；completion 到达后重新调度 Run。
cancel token 由 `ExecutionContext::cancellation` 提供，宿主操作应协作取消。

```cpp
engine.register_async_executor(
    "model",
    [](const dage::ExecutionContext& context, const dage::Value& input,
       const std::shared_ptr<dage::AsyncExecutorCompletion>& done) {
        host_model_async(input, [done](dage::ExecutionResult result) {
            done->complete(result);
        });
    });

run->execute_async(input, [](const dage::ExecutionResult& result) {
    // Run 的最终结果；callback 在配置的 Scheduler 上执行。
});
```

human Executor 可返回 category 为 `suspended` 的失败结果。调用 `Run::checkpoint()` 持久化，
用 `Engine::restore_run` 重建，再以 `Run::resume(human_output)` 继续。命名子流程通过
`Engine::register_workflow` 注册。局部修改使用 `Engine::apply_patch`。

`RunOptions` 可选择 Normal、Replay 或 Shadow，并控制 external write / irreversible。
Executor 通过 `ExecutionContext` 接收 run mode 和稳定 idempotency key。`Run::snapshot()`
提供只读节点状态、事件序号及 Workflow/Bundle digest。

## Scheduler SPI

默认构造的 Engine 自带有界线程池。宿主也可以实现 `dage::Scheduler`：

```cpp
auto scheduler = std::make_shared<MyScheduler>();
dage::Engine engine(scheduler);
```

Scheduler 负责接受 `std::function<void()>` 并返回 `std::future<void>`。队列拒绝和停止应以异常
报告；Runtime 将在执行边界把它转换成 `Error`。不要在任务尚未结束时销毁宿主依赖。
`schedule_continuation` 用于 Run 恢复和 parallel child-run：调用方保证当前任务不会同步
等待该任务，因此线程池可以从自身 worker 安全地将它放回 continuation 队列。

## Result

新模块使用 `dage::Result<T, dage::Error>` 表达普通验证和运行失败。当前为 pre-1.0，
`ExecutionResult` 将被直接替换，不提供 deprecated 兼容层；异常只用于构造失败、内存失败和内部不变量。
