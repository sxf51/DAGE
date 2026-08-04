# 可靠执行、Effect 与恢复

DAGE 只负责 DAG 执行语义。外部事务、数据库和业务补偿由宿主实现。

## Run 模式

- `Normal`：常规生产执行；
- `Replay`：从历史输入重新执行，仅允许 replay policy 明确安全的节点；
- `Shadow`：候选 Workflow 评估，默认禁止 external write 和 irreversible。

```cpp
dage::RunOptions options;
options.mode = dage::RunMode::Shadow;
options.allow_external_writes = false;
options.allow_irreversible = false;
auto run = engine.create_run(workflow, options);
```

执行栅栏：

| 场景 | 行为 |
| --- | --- |
| Shadow + external_write/irreversible | `EFFECT_REPLAY_BLOCKED` |
| Replay + at_most_once/manual/forbidden | `EFFECT_REPLAY_BLOCKED` |
| Normal + irreversible，未显式允许 | `EFFECT_REPLAY_BLOCKED` |
| retry + at_most_once/manual/forbidden | Workflow 验证失败 |
| idempotent | Executor 收到稳定 `idempotency_key` |

`ExecutionContext` 包含 run_id、node_id、node_type、attempt、run_mode、cancel token 和
idempotency_key。`${run.id}` 与 `${node.id}` 在调用 Executor 前解析，同一节点的重试保持相同
key。

## 节点状态机

```text
pending → running → succeeded
                  → failed
                  → suspended
                  → blocked
                  → skipped
```

`Run::snapshot()` 返回 Workflow/Bundle digest、当前节点、step、单调递增 event sequence、
运行模式和所有节点状态。快照用于观测，不是恢复格式。

## Checkpoint

0.2 Checkpoint 包含格式头、Workflow IR digest、Bundle digest、required capabilities、
RunMode、副作用许可、node status、event sequence、outputs、state 和暂停位置。

恢复时必须匹配 Workflow 与 Bundle digest。In-flight 节点只有 `safe` 或 `idempotent` 能降级为
pending 后恢复；`at_most_once`、`manual` 和 `forbidden` 拒绝自动恢复。

Checkpoint 不保存 Executor、密钥、权限或数据库连接。宿主必须重新注册相同语义的 Executor，
并兑现幂等键和外部事务策略。
