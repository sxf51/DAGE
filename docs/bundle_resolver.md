# Bundle Resolver、Lockfile 与签名

## 加载模式

| 模式 | 依赖选择 | Lockfile | Digest | 签名 | 资源来源 |
| --- | --- | --- | --- | --- | --- |
| `Development` | 解析 SemVer，选择最高兼容版本 | 输出新 lock | 校验内容身份 | 可选 | Repository |
| `Frozen` | 只用 lock 中的精确版本 | 必须 | 必须匹配 | 可选 | Repository |
| `Verified` | 只用 lock | 必须 | 必须匹配 | 必须满足阈值 | Repository |
| `Offline` | 只用 lock | 必须 | 必须匹配 | 默认必须 | 仅 Cache Repository |

Core 不发起网络请求。`BundleRepository` 的 `offline` 参数是强制策略信号：Offline 模式下，
实现必须只查询本地内容寻址缓存，cache miss 返回错误。
Offline 默认与 Verified 一样要求签名，确保生产离线部署不绕过供应链策略。测试或受控开发环境
可显式选择 `SignaturePolicyKind::Disabled`；`verified_production_profile` 会拒绝该策略。

## Manifest dependency

```json
{
  "dependencies": {
    "com.example.search": "^1.4.0",
    "com.example.writer": ">=2.1.0 <3.0.0"
  }
}
```

当前 pre-1.0 Resolver 支持精确 `MAJOR.MINOR.PATCH`、`^` 以及空格连接的
`>=`、`<=`、`>`、`<`、`=`。预发布版本语义尚未启用，不能静默接受。

Resolver 构建依赖 DAG，拒绝：

- 循环依赖；
- 同一 Bundle 的不兼容约束；
- lock 缺项；
- lock 版本不满足 manifest；
- lock digest 与实际内容不一致；
- 不存在兼容版本；
- Offline cache miss。

`ResolvedBundleGraph::topological_order()` 返回 dependency-first 顺序。

## dage.lock

```json
{
  "format": "dage-lock",
  "lock_version": 1,
  "bundles": {
    "com.example.search": {
      "version": "1.6.2",
      "digest": "sha256:...",
      "source": "registry.example.com",
      "signatures": [
        {"algorithm": "ed25519", "key_id": "release-2026-a"}
      ]
    }
  }
}
```

Development 每次输出完整、确定性排序的 lock。Frozen/Verified/Offline 不解析“最新版本”，只按
lock 精确加载。Lockfile 是部署输入，不放入被它锁定的 Bundle 内容中，避免身份循环。

## Ed25519

签名消息是：

1. 移除 `signatures` 后的 canonical manifest digest；
2. 所有规范化资源路径，按字节序排序；
3. 每个资源的 SHA-256 digest。

ZIP 压缩字节、mtime 和条目物理顺序不参与签名。签名字段：

```json
{
  "signatures": [{
    "algorithm": "ed25519",
    "key_id": "org.example.release-2026",
    "signature": "base64...",
    "signed_at": "2026-07-27T00:00:00Z"
  }]
}
```

Core 使用 OpenSSL EVP Ed25519 验证，不暴露 OpenSSL 类型。`KeyProvider` 返回 32 字节 raw
public key；内置 `Keyring` 支持添加和撤销。`TrustPolicy` 在密码学验证之外裁决 namespace、
环境、Bundle 类型和组织策略。Verified 模式默认要求至少一份有效且受信任的签名，可配置更高
阈值；数据模型原生支持多签名。

## C++ 示例

```cpp
dage::BundleResolverOptions options;
options.mode = dage::BundleLoadMode::Verified;
options.repository = &repository;
options.keys = &keyring;
options.trust = &trust_policy;
options.lock_json = lock_json;
options.signature_policy.kind = dage::SignaturePolicyKind::Threshold;
options.signature_policy.threshold = 2;

dage::BundleResolver resolver(options);
auto resolved = resolver.resolve(root_provider);
if (!resolved) {
    // resolved.error().code / message
}
auto graph = std::move(resolved.value());
auto workflow = engine.load_workflow(graph->root(), "main");
```

依赖冲突、lock、cache、签名和信任失败都通过 `Result<T, Error>` 返回。仅内存失败和内部
不变量异常可越过 C++ Resolver 边界。
