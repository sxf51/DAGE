# Contributing to DAGE

DAGE 使用 MIT 许可和 Developer Certificate of Origin 1.1，不要求 CLA。

## 提交要求

1. 变更应保持 DAGE 为小型、可嵌入、provider-agnostic 的 DAG runtime。
2. 协议、公共 API 或运行语义变化必须同时更新 `docs/`、示例和测试。
3. Workflow 语义变化使用显式 Patch，不以普通 deep merge 隐式改变。
4. 新增执行路径必须说明 Effect/Replay、取消、deadline 和错误语义。
5. 提交前运行文档中列出的完整测试。

每个 commit 必须包含 DCO sign-off：

```text
Signed-off-by: Name <email>
```

使用 `git commit -s` 可自动添加。Sign-off 表示贡献者同意
[Developer Certificate of Origin 1.1](https://developercertificate.org/)。

项目当前为 pre-1.0，不承诺兼容旧原型协议或 API。变更仍应说明设计原因，避免无意的行为漂移。
