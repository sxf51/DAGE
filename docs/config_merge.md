# Business Config Deep Merge

DAGE 只对普通业务配置提供 deep merge。Workflow 必须通过显式 Patch 修改；任意层级出现
`"format": "dage-workflow"` 都会返回 `WORKFLOW_CONFIG_MERGE_FORBIDDEN`。

```cpp
dage::Result<dage::Value> merged =
    dage::deep_merge_config(base, overlay);
```

默认规则：

- 根必须是 JSON object；
- object 按 key 递归合并；
- scalar 或类型不同时由 overlay 整体替换；
- array 整体替换，不按索引合并；
- object 成员的 `null` 表示删除；
- 输入不可变；
- 最大深度 64，最大输出 16 MiB。

需要数组追加或保留 null 时必须显式配置：

```cpp
dage::ConfigMergeOptions options;
options.arrays = dage::ConfigArrayMerge::Append;
options.nulls = dage::ConfigNullMerge::Preserve;
```

不提供隐式数组去重、按 `id` 合并或类型强制转换。这些策略容易产生不可审计的宿主差异，
应由宿主在调用 DAGE 前显式完成。

## Bundle precedence

Bundle manifest 可包含 object 类型的 `config`。`ResolvedBundleGraph::merged_config()` 按解析后
的拓扑顺序合并：依赖先于依赖者，root Bundle 最后，因此依赖者覆盖其依赖。兄弟依赖顺序由
规范化依赖 ID 的确定性遍历产生；若业务不能接受这种覆盖，应使用不同 key namespace，不能
依赖仓库返回顺序。

配置是已签名 Bundle 内容的一部分，因此任何配置变化都会改变 Bundle digest。Workflow 文件
不会进入此合并路径。
