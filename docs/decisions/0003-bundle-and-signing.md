# ADR-0003：Bundle、Patch 与签名

- 状态：Accepted
- 日期：2026-07-27

## 决策

Core 只理解规范化 Bundle 内容和抽象 ResourceProvider。目录与 ZIP 由官方工具包支持。
Bundle 可以依赖其他 Bundle。普通配置允许 deep merge；任何 Workflow 语义变化必须使用显式
Patch。生产部署的 Bundle 必须通过 Ed25519 签名验证。

## 后果

Core 不依赖文件格式和平台 I/O，测试更确定；发布工具承担打包与规范化责任。显式 Patch 增加
编辑操作数量，但提供审计、冲突检测、影响分析和可靠回滚基础。
