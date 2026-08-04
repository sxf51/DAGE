# 稳定 C ABI

公共入口为 `include/dage/dage.h`，纯 C99 可包含。当前为 pre-1.0，状态码和结构仍可直接重构；
稳定性承诺从 1.0.0 开始。

Bundle 通过内存资源表进入 C ABI：

```c
dage_resource_entry_t entries[] = {
    {{"manifest.json", 13}, {manifest_bytes, manifest_size}},
    {{"workflows/main.json", 19}, {workflow_bytes, workflow_size}}
};
dage_bundle_handle bundle = NULL;
dage_workflow_handle workflow = NULL;
dage_bundle_load(engine, entries, 2, &bundle);
dage_bundle_load_workflow(engine, bundle, (dage_string_view_t){"main", 4}, &workflow);
```

宿主负责从目录、ZIP、数据库或网络取得字节；Core 仍只看到规范化资源。Bundle、Workflow、
Run 均为 transferred opaque handle，分别使用对应 `destroy`。

`dage_run_create_with_options` 接受 `dage_run_options_t`，支持 Normal/Replay/Shadow 与副作用
许可。C Executor callback 接收 borrowed `dage_execution_context_t`，其中包含 run/node/type、
attempt、idempotency key 和 run mode。`dage_run_snapshot` 使用标准双阶段缓冲区返回运行快照。
所有符号以 `dage_` 开头，所有 C++ 异常在边界被捕获。

运行选项还支持 output/state/event/in-flight 配额。Executor 可通过 borrowed
`context->is_cancelled(context->cancellation_userdata)` 协作取消；宿主使用
`dage_run_cancel_with_reason` 提交稳定取消原因。函数指针和 userdata 不得跨 Executor
callback 保存。

典型生命周期：

```c
dage_engine_handle engine = NULL;
dage_workflow_handle workflow = NULL;
dage_run_handle run = NULL;
dage_engine_create(NULL, &engine);
dage_engine_load(engine, json_view, &workflow);
dage_run_create(engine, workflow, &run);
/* execute */
dage_run_destroy(run);
dage_workflow_destroy(workflow);
dage_engine_destroy(engine);
```

`create/load` 的输出均为 transferred；对应 `destroy` 接受 NULL。字符串输入是 UTF-8 borrowed
view，只在调用期间有效。format/export/run 输出采用双阶段调用：先传 NULL 得到
`DAGE_STATUS_BUFFER_TOO_SMALL` 和含结尾 NUL 的长度，再由调用方分配缓冲区。

可恢复运行使用 `DAGE_STATUS_SUSPENDED`、`dage_run_checkpoint`、
`dage_run_restore` 和 `dage_run_resume`。Checkpoint 是 UTF-8 JSON，并包含当前节点、节点输出、
state、循环/边计数和已消耗 timeout；它不包含 Executor、密钥或宿主权限。
恢复时会校验 Workflow 指纹，防止把 checkpoint 应用到不同图。

C Executor callback 每个节点 attempt 只调用一次。callback 填充 `dage_owned_buffer_t`；
DAGE 在解析 JSON 后恰好调用一次可选 `release`。buffer 可以不以 NUL 结尾，所有权在
callback 成功返回时转移给 DAGE。userdata 的 destroy callback 在 Executor 被替换或
Engine 销毁时恰好调用一次。Engine 必须比由它创建的 Run 活得更久。

原生异步 Executor 使用 `dage_engine_register_async_executor`。启动 callback 每个 attempt
调用一次并取得一次性 completion handle；宿主随后调用 `dage_executor_complete` 或
`dage_executor_abandon` 消费该 handle。等待期间可调用
`dage_executor_completion_is_cancelled`，外部副作用完成后使用
`dage_executor_completion_commit_effect`。late completion 安全，但同一 handle 不得消费两次。
