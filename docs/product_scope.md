# 产品范围与宿主边界

## 定位

**DAGE is a small, embeddable, provider-agnostic DAG runtime for AI applications.**

DAGE 是库，不是带数据库的 Agent 平台，也不是服务端系统。C++17 是内部实现语言；所有跨语言
SDK 以 C ABI 为唯一稳定边界。核心保持小而可组合，类似优秀 Python 库：默认开箱即用，
高级能力通过窄 SPI 替换。

项目当前为 pre-1.0，不兼容旧原型协议、API 或能力；1.0 前优先保持设计简洁，不增加迁移包袱。
1.0 发布后才执行 C ABI 与同 major Checkpoint 的增量兼容承诺。

## Core 拥有的语义

1. 读取规范化 Bundle，解析、规范化并验证 Workflow。
2. 编译不可变 IR，生成稳定指纹。
3. 根据依赖和控制边调度节点。
4. 管理节点输入、输出、错误、状态和取消信号。
5. 提供重试、超时、并行、暂停/恢复、checkpoint、重新运行。
6. 采集结构化 Trace；是否记录 prompt/完整输入由宿主配置。
7. 应用显式 Workflow Patch；普通配置可 deep merge。
8. 比较版本执行表现，选择版本并回滚。
9. 暴露 C++17 API 与 1.0 后仅增量扩展的 C ABI。

## 宿主拥有的能力

- LLM、Agent、Tool、MCP 或任意节点业务实现；
- 数据库、对象存储、文件系统、网络和密钥管理；
- 身份认证、授权、租户、限流和审计策略；
- Bundle 的目录/ZIP 读取与生产发布流程；
- Trace 的持久化、脱敏、采样和导出；
- 服务端、UI、队列、分布式 Worker 和部署。

Core 只看到抽象 `ResourceProvider` 提供的规范化资源，不依赖路径或 ZIP 格式。官方工具包提供
Directory 和 ZIP Provider。生产部署要求 Ed25519 签名；密钥托管和信任根仍由宿主负责。

## 错误边界

- 普通业务和执行失败：`Result<T, Error>`。
- 构造失败、内存失败、内部不变量破坏：C++ 异常。
- C ABI：纯状态码；所有异常在边界捕获，绝不跨 ABI。

## 明确非目标

- MaaFramework API/协议兼容；
- 内置模型客户端、Prompt 管理平台或 Agent 框架；
- 内置数据库、认证系统、Web 服务或可视化控制台；
- 在 Core 中读取任意宿主文件或发起网络请求；
- 以隐式 deep merge 改变 Workflow 控制语义；
- 让 Full Trace 成为不可关闭的生产默认值。
