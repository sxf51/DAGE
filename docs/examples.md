# JSON 工作流示例

完整节点格式见 `examples/workflows/all_nodes.json`。最小可运行流程：

```json
{
  "format": "dage-workflow",
  "format_version": "0.2.0",
  "entry": "echo",
  "nodes": {
    "echo": {
      "type": "tool",
      "executor": "echo",
      "effects": {"kind": "pure", "replay": "safe"},
      "input": {"message": "${workflow.input.message}"},
      "next": "finish"
    },
    "finish": {
      "type": "end",
      "input": {"result": "${nodes.echo.output.message}"}
    }
  }
}
```

CLI：

```powershell
build\dage.exe validate examples\workflows\all_nodes.json
build\dage.exe format examples\workflows\all_nodes.json
build\dage.exe mermaid examples\workflows\all_nodes.json
build\dage.exe dot examples\workflows\all_nodes.json
build\dage.exe patch workflow.json change.json
build\dage.exe resume workflow.json checkpoint.json "{\"approved\":true}"
build\dage.exe bundle-validate examples\bundles\basic main
build\dage.exe bundle-run examples\bundles\basic main "{\"message\":\"hello\"}"
```

`llm/tool/transform/join/subflow/human/custom` 都是宿主 Executor 节点；差异用于策略、诊断和
可视化，Core 不绑定厂商。`start/condition/noop/end` 是内建控制节点。`parallel` 按声明顺序
运行分支并在全部自然结束后进入 join。
