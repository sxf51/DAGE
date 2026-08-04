# ADR-0010：Bundle 解析与锁定

- 状态：Accepted / 1.0 Freeze
- 日期：2026-07-27

## 决策

Bundle 使用双层约束：

- `manifest.json` 使用 SemVer 范围表达作者兼容意图；
- `dage.lock` 固定精确版本、SHA-256 内容 digest、来源和签名元数据；
- production 默认必须有 lockfile；
- frozen 只接受 lockfile；
- verified 要求 lockfile、digest 与有效签名；
- offline 只接受本地缓存中 digest 完全匹配的 Bundle；
- 已发布版本的内容不可替换。

签名消息覆盖 canonical manifest、规范化文件路径以及每个文件的 digest，不直接签 ZIP 字节。
依赖解析结果自身也必须可确定性序列化和摘要。

## 后果

开发模式可以更新依赖；生产、离线与安全环境可复现。压缩方式和文件顺序变化不会破坏内容签名，
但规范化算法成为需要版本化的协议。
