# Security Policy

## Supported versions

DAGE 尚未发布 1.0。安全修复只应用于当前开发分支；1.0 发布时将公布正式支持窗口。拟议的
1.x 支持/LTS 窗口和安全发行流程见
[Support, LTS, and release process](docs/support_and_release.md)；在首个 1.0 RC 冻结前，
该窗口不是支持承诺。

## Reporting

请使用代码托管平台的 Private Vulnerability Reporting，勿为未修复漏洞创建公开 Issue。
在启用独立项目域名之前，不将个人邮箱作为唯一长期安全入口。若私密报告入口暂不可用，请通过
发布元数据中列出的维护者私密渠道联系，并只提供最小复现信息。

报告应包含受影响版本、平台、攻击前提、影响、复现步骤及已知缓解措施。不要附带真实密钥、
个人数据或不必要的生产数据。

目标响应节奏：

- 3 个工作日内确认收到；
- 7 个工作日内完成初步分级；
- 未解决期间至少每 14 天更新一次状态。

这些是响应目标，不是固定修复期限。项目将协调修复、公告、CVE（适用时）和致谢；在修复可用
前，请对漏洞细节保密。

重点私密报告类别包括：签名绕过、路径穿越、Bundle digest 混淆、C ABI 内存安全问题、
Checkpoint/Replay 导致的重复副作用，以及跨租户数据泄露。

安全边界、宿主责任和残余风险见
[Threat model and security review](docs/threat_model.md)。DAGE 不沙箱宿主 Executor，也
不把 Bundle 签名当作 DAGE 运行时发行包签名。
