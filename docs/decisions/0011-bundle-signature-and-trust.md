# ADR-0011：Bundle 签名与信任模型

- 状态：Accepted / 1.0 Freeze
- 日期：2026-07-27

## 决策

签名算法为 Ed25519，验证语义遵循 RFC 8032。Manifest 从第一版起使用 `signatures` 数组，
不得固化成单签名字段。

Core 提供 `KeyProvider`、内置 Keyring 和 `TrustPolicy`。宿主可用 callback/HSM adapter
提供公钥，并最终裁决 key 撤销、namespace scope、Bundle 类型、环境、发布时间证明和阈值。
未知 key 在 verified 模式默认拒绝。

第一版策略允许 `at_least_one`；数据结构预留 `all_required` 和 `threshold M-of-N`。私钥、
撤销列表分发和 HSM 生命周期不属于 Core。

## 后果

支持 key rotation、多签名和企业信任系统，同时避免 Core 成为 PKI 产品。规范化签名消息必须
有独立测试向量，key id 仅用于查找，不能替代公钥校验。
