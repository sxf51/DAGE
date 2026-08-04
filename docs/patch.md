# Revision、Patch 与回滚

`Engine::apply_patch` 接收协议第 32 节的局部 Patch，基于不可变 Workflow 生成一个新 Workflow，
并在返回前重新执行 Normalize、Validate、Compile。旧 Workflow 不被修改，因此回滚只需继续
使用旧对象或其持久化 JSON，不需要逆向执行 Patch。

每个 Patch 必须携带当前 `Workflow::ir().digest()`：

```json
{
  "base_digest": "sha256:...",
  "base_revision": 3,
  "changes": [
    {"action": "set_field", "node_id": "write", "field": "timeout_ms", "value": 5000}
  ]
}
```

`base_digest` 是强制的乐观并发前置条件；不匹配返回
`PATCH_BASE_DIGEST_MISMATCH`。`base_revision` 可选，但提供时也必须精确匹配。不要只依赖
revision，因为不同 Workflow 可能拥有相同 revision。

`Engine::dry_run_patch` 在私有副本上执行完整的 Normalize、Validate 和 Compile，返回候选
digest、revision 与诊断，但不发布候选 Workflow。`apply_patch` 使用相同事务路径：任何
change 或最终验证失败都会丢弃整个私有副本，原 Workflow 和 revision 保持不变。

dry-run 诊断按 change 输入顺序返回，且 `code` 与 `path` 是稳定的机器接口。例如：

```json
[
  {
    "code": "PATCH_NODE_ID_REQUIRED",
    "path": "/changes/0/node_id",
    "message": "node_id is required."
  },
  {
    "code": "PATCH_UNKNOWN_ACTION",
    "path": "/changes/1/action",
    "message": "Unknown Patch action: rewrite_everything"
  }
]
```

文档级前置条件定位到 `/base_digest`、`/base_revision` 或 `/changes`；change schema 和
状态错误定位到 `/changes/{index}/{field}`。change 全部应用后产生的 Workflow 验证错误保留
原验证 code，并以 `/candidate` 表示候选对象作用域。调用方必须按 `code` 和 `path` 处理，
不应解析面向人的 `message`。

`Engine::analyze_patch` 进一步返回：

- 直接改变的节点；
- 被控制流、错误流、parallel 或 `${nodes.*}` 数据依赖传递影响的节点；
- effect policy 是否变化；
- 现有 checkpoint 是否能按语义 digest 继续恢复；
- 以 candidate digest/revision 为前置条件的 inverse Patch。

inverse 使用显式 `add_node`、`remove_node`、`replace_node` 和 `set_entry` 操作。应用 inverse
会继续增加 revision，但恢复原 Workflow 的编译语义 digest。`x_revision` 是发布元数据，
不属于 IR 语义 digest。

支持 `add_node`、`remove_node`、`replace_node`、`rename_node`、`set_field`、
`remove_field`、`set_next`、`insert_next`、`remove_next`、`set_on_error`、
`enable_node`、`disable_node` 和 `set_entry`。字段操作格式：

```json
{
  "base_digest": "sha256:...",
  "changes": [
    {"action": "set_field", "node_id": "write", "field": "timeout_ms", "value": 5000},
    {"action": "rename_node", "node_id": "write", "new_node_id": "draft"}
  ]
}
```

成功后 `x_revision` 加一。rename 会同步 entry、next、on_error、branches 和 join 引用。
任何未知 action、无效引用或最终验证错误都会拒绝整个操作。
