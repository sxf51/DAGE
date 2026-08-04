# Bundle 与 ResourceProvider

## Bundle 格式

DAGE Core 不读取目录、ZIP、网络或数据库，只通过 `ResourceProvider` 接收规范化资源。
Bundle 根资源必须包含 `manifest.json`：

```json
{
  "format": "dage-bundle",
  "format_version": "0.2.0",
  "id": "org.example.research",
  "version": "1.2.0",
  "workflows": {
    "main": "workflows/main.json"
  },
  "dependencies": {},
  "config": {},
  "signatures": []
}
```

`config` 是可选的普通业务配置 object，并参与 Bundle digest 与签名。依赖 Bundle 的配置可
通过 `ResolvedBundleGraph::merged_config()` 按确定性拓扑顺序合并，完整规则见
[Business Config Deep Merge](config_merge.md)。Workflow 不允许嵌入 `config` 走隐式合并。

资源路径必须是 UTF-8 相对路径，使用 `/`，不得包含空段、`.`、`..`、反斜杠、NUL 或绝对路径。
Provider 返回的列表必须已经规范化且不得重复。Core 按路径和文件内容计算 `sha256:` Bundle
digest；压缩顺序和 ZIP 元数据不参与内容身份。

当前 `MemoryResourceProvider` 用于嵌入和测试。Directory/ZIP Provider 属于官方工具层，不能让
Core 依赖文件系统或压缩库。

CLI 已提供 Directory Provider：

```powershell
build-next\dage.exe bundle-validate examples\bundles\basic main
build-next\dage.exe bundle-run examples\bundles\basic main "{\"message\":\"hello\"}"
```

ZIP Provider 将作为独立官方工具组件实现，不链接进 Core。

## C++ 使用

```cpp
dage::MemoryResourceProvider resources;
resources.add_text("manifest.json", manifest);
resources.add_text("workflows/main.json", workflow);

dage::Engine engine;
auto bundle = engine.load_bundle(resources);
auto main = engine.load_workflow(*bundle, "main");
auto run = engine.create_run(*main);
```

`manifest.json` 的 SemVer dependency、`dage.lock` 精确 digest、Ed25519、KeyProvider 和
TrustPolicy 已由 [Bundle Resolver](bundle_resolver.md) 实现。Loader 保持不依赖 I/O，只建立
内容边界、canonical signing payload 和确定性 digest。

## 不可变 IR

Workflow 通过 Normalizer/Validator 后立即编译为 `WorkflowIR`：

- 节点类型、边、并行分支、重试和超时均为类型化字段；
- Effect/Replay 在编译期固化；
- 节点按稳定 ID 建立只读索引；
- IR 具有规范化内容的 SHA-256 digest；
- `Run` 只读取 IR，不读取 Workflow JSON DOM；
- JSON DOM 仅存在于 Loader/Normalizer/Patch 和运行期动态 `Value` 边界。

`Workflow::ir()` 返回 const 引用。IR 没有 setter，Run 共享其不可变快照。
