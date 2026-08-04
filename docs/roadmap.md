# 从 DAG Runtime 到完整 AI Infrastructure 核心的路线图

本路线图中的“完整”指嵌入式 DAG 运行时能力完整，不扩大为 Agent 平台。每阶段都必须同步
协议、API、示例、测试和迁移说明；未通过阶段出口条件，不进入下一阶段的兼容承诺。

当前为 pre-1.0：不兼容旧原型协议、方法、Checkpoint 或能力，不建立 deprecated 兼容层。
可以为得到更小、更一致的 1.0 API 直接重构。兼容承诺从 1.0.0 发布后开始。

## 已确定且不再阻塞的决策

| 主题 | 决策 | 主要权衡 |
| --- | --- | --- |
| 产品 | 小型嵌入式库 | 易嵌入、边界清楚；平台能力交给宿主 |
| 实现/FFI | C++17 + C ABI | Core 可用现代类型；绑定不承受 C++ ABI |
| 并发 | 默认有界线程池 + Scheduler SPI | 开箱即用，同时可接宿主事件循环 |
| Bundle | Core 只读规范化内容和 ResourceProvider | 核心可测试；目录/ZIP 移到官方工具 |
| 合并 | 配置 deep merge，Workflow 必须显式 Patch | 便利性与语义审计兼得 |
| 签名 | Ed25519，生产部署必须签名 | 签名小且快；信任根管理由宿主负责 |
| Trace | Capture Level 可切换到 Full | 调试完整；生产隐私/成本由部署方决定 |
| 错误 | Result + 有限异常；C ABI 纯状态码 | 业务分支显式，灾难错误不伪装成业务错误 |
| 许可 | MIT | 易采用；专利与商标策略需另行决定 |

## 阶段 1：Core 0.2 边界重构

目标：将原型从单体执行器变成清晰的 Protocol → Validator → IR → Runtime 分层。

- 切换 C++17，建立 `Result<T, Error>`；
- 用自有有界线程池替换 `std::async`，提供 Scheduler SPI；
- 定义不可变 Workflow IR、稳定 ID 和确定性序列化；
- 把 JSON DOM 限制在 Loader/Normalizer 边界；
- 建立 `ResourceProvider`，但不把目录/ZIP 放入 Core；
- 保持当前 12 类节点的协议回归矩阵。

出口：所有节点正向、失败、超时、取消、重试、嵌套和并行组合均可重复通过；TSAN/ASAN/UBSAN
及 Windows/Linux 编译通过。

取舍：早期重构会延缓新增节点，但避免 JSON DOM 和执行细节固化进 1.0 ABI。

## 阶段 2：Bundle、依赖与供应链

目标：工作流成为可解析、可锁定、可验证、可复现的发布单元。

- Bundle manifest、内容寻址、规范化字节序列；
- Bundle 可依赖其他 Bundle，生成 lockfile 与依赖 DAG；
- 版本约束、冲突诊断、循环依赖检测；
- Directory/ZIP 官方 Provider；
- Ed25519 签名、key id、信任策略 SPI；
- 开发模式可放宽，production policy 拒绝未签名或内容不匹配 Bundle。

出口：相同 Bundle 在不同平台产生同一摘要和 IR；篡改、错签、未知 key、依赖替换全部失败。

状态：核心能力已于 0.2 原型完成，包括 SemVer Resolver、依赖 DAG、确定性 lockfile、
Development/Frozen/Verified/Offline、Ed25519、KeyProvider、Keyring、TrustPolicy、多签名结构
和阈值。后续仍需增加 Directory/ZIP Repository、HSM adapter 与正式跨平台测试向量。

取舍：锁文件牺牲“自动拿最新版本”，换取可复现部署。Core 验签但不管理私钥。

## 阶段 3：可靠执行与恢复

目标：使运行状态成为明确协议，而不是进程内偶然状态。

- Run/Node 状态机与事件序列号；
- checkpoint schema、版本迁移和幂等恢复；
- deadline 传播、协作取消、重试预算、退避与 jitter；
- fail-fast / all / any / min-success 等并行策略的确定语义；
- rerun-from-node、selective replay 和 side-effect fence；
- StateStore SPI；默认内存实现，并提供本地持久化参考实现。

出口：崩溃点注入、重复恢复、重复事件、取消/超时竞态均有模型测试。

状态：0.2 已完成 RunMode、Effect/Replay 执行栅栏、稳定幂等键、节点状态机、事件序号、
运行快照、Checkpoint capability/node status、Workflow/Bundle digest 恢复检查、deadline、
重试预算与 jitter、commit fence、selective rerun、崩溃点注入、版本化 StateStore/CAS、
本地 FileStateStore、并行父状态原子发布、异步 Executor，以及跨进程崩溃恢复压力测试。

取舍：exactly-once 对外部副作用通常不可实现；DAGE 提供幂等键和提交栅栏，宿主兑现语义。

## 阶段 4：Trace、可观测性与隐私

目标：同一份运行轨迹既支持故障定位，也支持 AI workflow 评测。

- `Off / Metadata / Inputs / Full` capture levels；
- 稳定事件 envelope、trace/span/causation ID；
- TraceSink SPI、背压与采样；
- 字段级 redaction hook、大小限制和二进制引用；
- OpenTelemetry 官方桥接放在扩展包，不污染 Core。

出口：禁用数据采集时不泄露 payload；Full 模式可重建控制决策；慢 Sink 不阻塞调度线程。

状态：已完成结构化 EventEnvelope、Off/Metadata/Inputs/Full 配置、TraceSink SPI、
MemoryTraceSink、带原因的异步取消、运行级资源配额、parallel child-run 恢复关联、
Linux/macOS 多进程与 sanitizer CI、异步有界背压队列、确定性采样、递归 JSON redaction、
trace/span/parent/causation 关联、ResourceLease SPI，以及无 SDK 依赖的 OpenTelemetry
扩展桥接。后续仍可增加 JSON Pointer redaction、动态采样规则和正式 SDK adapter 包。

取舍：默认建议 Metadata；Full 提升可复现性但显著增加隐私、存储与延迟成本。

## 阶段 5：Patch、版本比较与回滚

目标：让 AI 工作流演进可解释、可审计、可逆。

- 业务配置 deep merge 的精确定义；
- Workflow Patch 使用稳定操作集、前置条件和 base fingerprint；
- Patch dry-run、影响分析、逆向 Patch；
- Trace replay 与 A/B 结果比较接口；
- 指标由宿主定义，DAGE 只聚合并比较；
- Bundle 版本选择和回滚 API。

出口：任何控制流变化都有显式 Patch；错误 base、节点重命名、删除引用均能诊断；回滚可复现。

取舍：不支持“随意 JSON merge workflow”，牺牲短期方便来换长期可审计性。

## 阶段 6：稳定 C ABI 与语言 SDK

目标：把 1.0 兼容承诺建立在经过验证的 Core 上。

- opaque handles、结构 `struct_size`、能力查询、allocator/callback 规则；
- 状态码和字段编号登记表，发布后永不重排或复用；
- C ABI 全入口异常屏障、线程与生命周期说明；
- Python、Java、C#、Rust、Node.js 的惯用封装；
- ABI dump/diff、旧头文件对新库、旧二进制对新库的 CI。

出口：五语言相同 conformance suite；跨版本二进制兼容测试通过。

取舍：C ABI 更繁琐，但它是多语言长期稳定的最小公分母。C++ API 不承诺跨编译器 ABI。

## 阶段 7：性能、可移植性与 1.0

目标：以数据证明“高效、简洁、可靠”。

- 解析、编译、调度、恢复的微基准和真实 AI DAG 基准；
- arena/小对象优化只在 profile 证明后引入；
- Windows/Linux/macOS，x64/arm64；
- 模糊测试、属性测试、故障注入、安全审计；
- API freeze、LTS 与漏洞响应策略；

出口：性能预算、内存上限、兼容矩阵、威胁模型、运维手册和迁移报告齐全。

## 1.0 冻结决策

此前 7 个开放项已于 2026-07-27 全部关闭，详见
[ADR-0010 至 ADR-0016](decisions/README.md)。其摘要为：

1. Manifest 使用 SemVer，production lockfile 固定 version + digest。
2. Core 提供 KeyProvider/keyring，宿主 TrustPolicy 最终裁决。
3. 默认 bounded queue + Reject；仅受严格约束的 Worker 嵌套允许 caller-runs。
4. 较旧同 major Checkpoint 必须可迁移读取；Writer 只写当前格式。
5. 1.0 前潜在副作用节点必须声明独立的 Effect 与 Replay policy。
6. C++17；Tier 1 为 Linux/Windows/macOS 64 位及 ADR 指定工具链。
7. MIT + DCO 1.1；暂不采用 CLA，并补齐项目治理与安全文件。
