# DAGE + LangChain 完整 Python Demo

这个示例展示一条可用于真实宿主程序的集成路径：DAGE 运行不可变工作流和并发分支，LangChain 调用 OpenAI 兼容模型及结构化工具，宿主用版本号、语义 digest、文件锁和原子替换安全地修改 JSON 节点定义。

## 功能

- OpenAI 官方 API，或任何支持 OpenAI Chat Completions/tool calling 格式的兼容服务；
- API Key 可来自环境变量，也可在终端中隐藏输入，不写日志和文件；
- LangChain 模型工具：读取工作流、应用 DAGE Patch、安全计算器、UTC 时间；
- DAGE `parallel`/`join` 并发分支、async executor、deadline、运行配额和完整 trace；
- `external_write` effect、幂等键和 commit fence；
- JSON 节点修改前执行 DAGE dry-run、影响分析、校验和编译；
- `x_revision` + DAGE digest 双重乐观锁，跨进程文件锁，临时文件 + `fsync` + 原子替换；
- 工作流模板与运行副本分离。模型只可访问指定文件，不能传入任意路径。

## 安装

建议创建独立虚拟环境。已经从 TestPyPI 安装 DAGE 时，只安装其余依赖即可：

```powershell
cd D:\DAGE\examples\python\langchain_complete_demo
python -m pip install "langchain-core>=1.5,<1.6" "langchain-openai>=1.1,<1.2" "filelock>=3.20,<4"
```

若尚未安装 DAGE 0.2.0：

```powershell
python -m pip install --extra-index-url https://test.pypi.org/simple/ dage-runtime==0.2.0
python -m pip install -r requirements.txt
```

TestPyPI 的依赖集合可能不完整，所以使用 `--extra-index-url`，让普通依赖仍从 PyPI 解析。生产环境发布后应改用正式 PyPI，并用锁文件固定已审计的传递依赖。

## 先做离线校验

该命令不需要 API Key，也不访问模型：

```powershell
python app.py --validate
```

首次运行会把受 Git 管理的 `workflow.template.json` 复制为被忽略的 `state/workflow.json`。后续修改只落到运行副本。删除 `state` 目录即可恢复模板状态。

还可以不用 Key 跑完 DAGE 的 parallel/join、异步 executor 和两个 LangChain 工具：

```powershell
python app.py --offline "验证并发与工具"
```

`--offline` 是 smoke test，不假装生成真实 LLM 回答，也不会修改工作流。

## 配置模型

推荐只在当前 PowerShell 进程设置环境变量：

```powershell
$env:OPENAI_API_KEY = Read-Host "API Key" -AsSecureString |
  ConvertFrom-SecureString -AsPlainText
$env:OPENAI_BASE_URL = "https://api.openai.com/v1"
$env:OPENAI_MODEL = "gpt-4.1-mini"
```

兼容服务只需替换 `OPENAI_BASE_URL` 和模型名。若未设置 Key，程序会用 `getpass` 提示隐藏输入。`--api-key` 仅用于临时调试，不推荐，因为命令行可能进入 shell 历史或进程列表。

## 运行问答与工具调用

```powershell
python app.py "现在的 UTC 时间是多少？顺便计算 (17 * 23) + 5"
```

模型可以调用 `utc_now` 和 `calculator`。最终 JSON 包含回答、工具调用记录、两个 DAGE 并发节点的耗时以及 trace 事件数。

## 让 LLM 修改 JSON 节点

下面要求模型先读版本，再用 DAGE Patch 修改 `inspect` 节点的超时：

```powershell
python app.py "检查工作流并说明修改结果" `
  --change-request "把 inspect 节点的 timeout_ms 设置为 3000，只做这一项修改"
```

成功后，`state/workflow.json` 的 `x_revision` 从 0 增至 1。修改工具不直接编辑任意 JSON：它依次执行：

1. 获取跨进程文件锁并重新读取文件；
2. 比较模型传入的 `expected_version` 与当前 `x_revision`；
3. 用当前 DAGE 语义 digest 构造 Patch；
4. 执行 `dry_run_patch` 与 `analyze_patch`；
5. 执行 `apply_patch`，取得规范化、已重新编译的候选工作流；
6. 校验 revision 恰好加一，再原子替换目标文件。

当前 Run 始终执行启动时加载的不可变快照，因此修改只影响下一次 Run。这避免“运行到一半图结构改变”。两个写入者基于同一版本修改时，只有先获得锁且版本匹配者成功；后一个收到 conflict，必须重新读取并由模型/用户重新判断，不能自动覆盖。

## 并发边界

`fan_out` 同时调度 `agent` 和 `inspect`，`merge` 等两者完成。`max_parallel=2` 是工作流上限，`RunOptions.max_in_flight_tasks=2` 是本次运行上限。两个 executor 都是异步函数，所以模型网络等待和本地检查可以重叠。

同一个 DAGE `Run` 仍然是 single-flight：不要并发调用同一个 Run 的 `execute/resume/checkpoint`。若服务要同时处理多个用户请求，应共享一个长生命周期 `Engine` 和不可变 `Workflow`，但为每个请求创建独立 `Run`。Python CPU 密集工作应放进进程池或原生代码；asyncio 并不会绕过 GIL。

## 安全与生产化边界

- Demo 授权模型修改一个明确指定的工作流副本，不提供通用文件读写或 shell 工具。
- 修改仍应在真实产品中增加 RBAC、审批、审计日志、备份/回滚和允许字段策略。
- `filelock` 适合本地文件。多副本服务应把 `(workflow_id, version, document)` 放进支持事务 CAS 的数据库/对象存储，而不是共享网络文件。
- OpenAI 兼容并不保证所有服务都完整实现 tool calling；模型必须支持结构化工具调用。
- DAGE 管理执行语义，不管理模型账号、费用、提示词安全、内容审核或密钥轮换，这些属于宿主应用责任。
- 示例返回 `True` 关闭 external-write commit fence；生产写工具应使用 `context.idempotency_key` 做持久化去重，并在写入真正持久化后才 commit。
