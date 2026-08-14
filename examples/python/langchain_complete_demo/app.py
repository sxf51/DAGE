"""DAGE + LangChain + OpenAI 兼容模型的完整宿主演示。"""

from __future__ import annotations

import argparse
import ast
import asyncio
import getpass
import json
import math
import operator
import os
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Literal

from dage import DageError, Engine, RunOptions, TraceCapture
from filelock import FileLock
from langchain_core.messages import AIMessage, HumanMessage, SystemMessage, ToolMessage
from langchain_core.tools import StructuredTool
from langchain_openai import ChatOpenAI
from pydantic import BaseModel, Field


DEMO_DIR = Path(__file__).resolve().parent
WORKFLOW_TEMPLATE = DEMO_DIR / "workflow.template.json"
DEFAULT_WORKFLOW = DEMO_DIR / "state" / "workflow.json"
MAX_TOOL_ROUNDS = 8


class PatchChange(BaseModel):
    """允许模型生成的 DAGE Patch 单项。"""

    action: Literal[
        "add_node", "remove_node", "replace_node", "rename_node", "set_field",
        "remove_field", "set_next", "insert_next", "remove_next", "set_on_error",
        "enable_node", "disable_node", "set_entry",
    ] = Field(description="DAGE 支持的 patch action")
    node_id: str | None = Field(default=None, description="目标节点 ID")
    node: dict[str, Any] | None = Field(default=None, description="新增或替换后的完整节点")
    field: str | None = Field(default=None, description="set_field/remove_field 的字段")
    value: Any = Field(default=None, description="字段值或节点定义")
    new_node_id: str | None = Field(default=None, description="rename_node 的新 ID")
    index: int | None = Field(default=None, ge=0, description="next 数组中的位置")


class UpdateWorkflowInput(BaseModel):
    expected_version: int = Field(ge=0, description="读取时得到的 x_revision；必须原样传回")
    changes: list[PatchChange] = Field(min_length=1, max_length=20)


class CalculatorInput(BaseModel):
    expression: str = Field(description="只含数字、括号和基本算术运算的表达式")


@dataclass(frozen=True)
class WorkflowSnapshot:
    document: dict[str, Any]
    version: int
    digest: str


class VersionConflict(RuntimeError):
    """调用者基于旧版本写入；应重新读取后再决定是否重试。"""


def _load_document(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("workflow.json 顶层必须是 JSON object")
    return document


def _ensure_workflow(path: Path) -> None:
    """默认运行副本与 Git 中的模板隔离，避免 demo 意外修改仓库样例。"""
    if path.exists():
        return
    if path != DEFAULT_WORKFLOW:
        raise FileNotFoundError(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write_json(path, _load_document(WORKFLOW_TEMPLATE))


def _snapshot(engine: Engine, path: Path) -> WorkflowSnapshot:
    document = _load_document(path)
    version = document.get("x_revision", 0)
    if not isinstance(version, int) or version < 0:
        raise ValueError("x_revision 必须是非负整数")
    with engine.load(document) as workflow:
        # Python API 没有暴露单独的 digest 属性；self-diff 是稳定的只读查询。
        digest = workflow.diff(workflow)["before_digest"]
    return WorkflowSnapshot(document, version, digest)


def _atomic_write_json(path: Path, document: dict[str, Any]) -> None:
    """同目录临时文件 + fsync + os.replace，避免崩溃留下半个 JSON。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(document, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def update_workflow_file(
    engine: Engine, path: Path, expected_version: int, changes: list[dict[str, Any]]
) -> dict[str, Any]:
    """在跨进程锁内完成 compare-and-swap、DAGE 校验和原子发布。"""
    with FileLock(str(path) + ".lock", timeout=10):
        current = _snapshot(engine, path)
        if current.version != expected_version:
            raise VersionConflict(
                f"版本冲突：期望 {expected_version}，当前 {current.version}；请重新读取"
            )

        patch = {
            "base_digest": current.digest,
            "base_revision": current.version,
            "changes": changes,
        }
        with engine.load(current.document) as workflow:
            dry_run = workflow.dry_run_patch(patch)
            if not dry_run.get("valid"):
                return {"updated": False, "diagnostics": dry_run.get("diagnostics", [])}
            analysis = workflow.analyze_patch(patch)
            with workflow.apply_patch(patch) as candidate:
                # format() 是经过 normalize/validate/compile 后的规范 JSON。
                candidate_document = json.loads(candidate.format())

        candidate_version = candidate_document.get("x_revision")
        if candidate_version != expected_version + 1:
            raise RuntimeError("DAGE 返回了非预期 revision，拒绝持久化")
        _atomic_write_json(path, candidate_document)
        return {
            "updated": True,
            "old_version": expected_version,
            "new_version": candidate_version,
            "candidate_digest": dry_run.get("candidate_digest"),
            "affected_nodes": analysis.get("affected_nodes", []),
            "notice": "当前 Run 使用不可变旧快照；新定义从下一次 Run 生效",
        }


_BINARY_OPERATORS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
    ast.Pow: operator.pow,
}
_UNARY_OPERATORS = {ast.UAdd: operator.pos, ast.USub: operator.neg}


def safe_calculate(expression: str) -> float | int:
    """不使用 eval 的受限计算器；限制深度、长度、幂和结果大小。"""
    if len(expression) > 100:
        raise ValueError("表达式过长")

    def evaluate(node: ast.AST, depth: int = 0) -> float | int:
        if depth > 12:
            raise ValueError("表达式嵌套过深")
        if isinstance(node, ast.Constant) and type(node.value) in (int, float):
            return node.value
        if isinstance(node, ast.UnaryOp) and type(node.op) in _UNARY_OPERATORS:
            return _UNARY_OPERATORS[type(node.op)](evaluate(node.operand, depth + 1))
        if isinstance(node, ast.BinOp) and type(node.op) in _BINARY_OPERATORS:
            left, right = evaluate(node.left, depth + 1), evaluate(node.right, depth + 1)
            if isinstance(node.op, ast.Pow) and abs(right) > 10:
                raise ValueError("指数绝对值不能超过 10")
            result = _BINARY_OPERATORS[type(node.op)](left, right)
            if not math.isfinite(result) or abs(result) > 1e100:
                raise ValueError("结果超出允许范围")
            return result
        raise ValueError("只允许数字、括号以及 + - * / // % **")

    return evaluate(ast.parse(expression, mode="eval").body)


def build_tools(engine: Engine, workflow_path: Path) -> list[StructuredTool]:
    """工具闭包只拥有指定 workflow 文件的权限，不接受任意路径。"""

    def read_workflow() -> dict[str, Any]:
        snapshot = _snapshot(engine, workflow_path)
        return {
            "version": snapshot.version,
            "digest": snapshot.digest,
            "workflow": snapshot.document,
        }

    def update_workflow(expected_version: int, changes: list[PatchChange]) -> dict[str, Any]:
        try:
            return update_workflow_file(
                engine,
                workflow_path,
                expected_version,
                [change.model_dump(exclude_none=True) for change in changes],
            )
        except VersionConflict as error:
            return {"updated": False, "conflict": True, "message": str(error)}

    def calculator(expression: str) -> dict[str, float | int]:
        return {"result": safe_calculate(expression)}

    def utc_now() -> dict[str, str]:
        return {"utc": datetime.now(timezone.utc).isoformat()}

    return [
        StructuredTool.from_function(
            read_workflow,
            name="read_workflow",
            description="读取当前 DAGE JSON、语义 digest 和乐观锁版本号。修改前必须调用。",
        ),
        StructuredTool.from_function(
            update_workflow,
            name="update_workflow",
            description=(
                "以 DAGE Patch 修改节点定义。必须使用刚读取的 expected_version；"
                "冲突时重新读取，但不要在语义不确定时自动覆盖。"
            ),
            args_schema=UpdateWorkflowInput,
        ),
        StructuredTool.from_function(
            calculator,
            name="calculator",
            description="安全执行基本算术，不执行 Python 代码。",
            args_schema=CalculatorInput,
        ),
        StructuredTool.from_function(
            utc_now,
            name="utc_now",
            description="返回当前 UTC 时间。",
        ),
    ]


async def run_tool_call(tool_map: dict[str, StructuredTool], call: dict[str, Any]) -> ToolMessage:
    tool = tool_map.get(call["name"])
    if tool is None:
        content: Any = {"error": f"未知或未授权工具：{call['name']}"}
    else:
        try:
            content = await tool.ainvoke(call.get("args", {}))
        except Exception as error:  # 工具错误作为观察返回模型，不泄露堆栈或密钥。
            content = {"error": type(error).__name__, "message": str(error)}
    return ToolMessage(
        content=json.dumps(content, ensure_ascii=False, default=str),
        tool_call_id=call["id"],
    )


async def invoke_agent(
    model: ChatOpenAI, tools: list[StructuredTool], value: dict[str, Any]
) -> dict[str, Any]:
    tool_map = {tool.name: tool for tool in tools}
    runnable = model.bind_tools(tools)
    request = (
        f"用户问题：{value['question']}\n"
        f"工作流修改要求：{value.get('change_request') or '无'}\n"
        f"用户看到的版本号：{value.get('expected_version')}"
    )
    messages = [
        SystemMessage(
            content=(
                "你是 DAGE 工作流助手。可以回答问题并使用工具。若用户要求修改工作流，"
                "必须先 read_workflow，再用返回的 version 调 update_workflow；只做用户明确"
                "要求的最小修改。遇到版本冲突时说明冲突，不猜测、不覆盖。不要索取或输出 API Key。"
            )
        ),
        HumanMessage(content=request),
    ]
    called_tools: list[str] = []
    for _ in range(MAX_TOOL_ROUNDS):
        response: AIMessage = await runnable.ainvoke(messages)
        messages.append(response)
        if not response.tool_calls:
            return {"answer": str(response.content), "tools_called": called_tools}
        # 同一轮的只读工具可以并发；写工具依靠 CAS 决定唯一胜者。
        tool_messages = await asyncio.gather(
            *(run_tool_call(tool_map, call) for call in response.tool_calls)
        )
        called_tools.extend(call["name"] for call in response.tool_calls)
        messages.extend(tool_messages)
    raise RuntimeError(f"模型工具调用超过 {MAX_TOOL_ROUNDS} 轮")


async def invoke_offline_agent(
    tools: list[StructuredTool], value: dict[str, Any]
) -> dict[str, Any]:
    """不访问网络的 smoke test；真实运行仍走上面的 LangChain tool-calling 循环。"""
    tool_map = {tool.name: tool for tool in tools}
    workflow = await tool_map["read_workflow"].ainvoke({})
    calculation = await tool_map["calculator"].ainvoke({"expression": "17 * 23 + 5"})
    await asyncio.sleep(0.30)  # 让并发重叠在输出耗时中清晰可见。
    return {
        "answer": f"离线验证完成：{value['question']}",
        "tools_called": ["read_workflow", "calculator"],
        "observations": {
            "workflow_version": workflow["version"],
            "calculation": calculation,
        },
    }


async def execute(arguments: argparse.Namespace) -> dict[str, Any]:
    workflow_path = arguments.workflow.resolve()
    _ensure_workflow(workflow_path)
    model: ChatOpenAI | None = None
    if not arguments.offline:
        api_key = arguments.api_key or os.getenv("OPENAI_API_KEY")
        if not api_key:
            api_key = getpass.getpass("请输入 OpenAI 兼容 API Key（不会保存）：").strip()
        if not api_key:
            raise ValueError("API Key 不能为空")
        base_url = arguments.base_url or os.getenv(
            "OPENAI_BASE_URL", "https://api.openai.com/v1"
        )
        model_name = arguments.model or os.getenv("OPENAI_MODEL", "gpt-4.1-mini")
        model = ChatOpenAI(
            api_key=api_key,
            base_url=base_url,
            model=model_name,
            temperature=0,
            timeout=arguments.model_timeout,
            max_retries=2,
        )

    traces: list[dict[str, Any]] = []
    timings: dict[str, dict[str, float]] = {}
    with Engine() as engine:
        engine.set_trace_sink(traces.append)
        tools = build_tools(engine, workflow_path)

        async def langchain_agent(context, value):
            started = time.perf_counter()
            result = (
                await invoke_offline_agent(tools, value)
                if model is None
                else await invoke_agent(model, tools, value)
            )
            timings[context.node_id] = {"elapsed_ms": (time.perf_counter() - started) * 1000}
            # 该 executor 允许写文件；True 关闭 DAGE 的 external_write commit fence。
            return {
                **result,
                "dage": {"run_id": context.run_id, "node_id": context.node_id},
            }, True

        async def inspect_workflow(context, _value):
            started = time.perf_counter()
            snapshot = await asyncio.to_thread(_snapshot, engine, workflow_path)
            await asyncio.sleep(arguments.inspect_delay)
            timings[context.node_id] = {"elapsed_ms": (time.perf_counter() - started) * 1000}
            return {
                "version_at_run_start": snapshot.version,
                "node_count": len(snapshot.document["nodes"]),
                "digest": snapshot.digest,
            }

        async def merge_results(_context, value):
            return value

        engine.register_async_executor("langchain_agent", langchain_agent)
        engine.register_async_executor("inspect_workflow", inspect_workflow)
        engine.register_async_executor("merge_results", merge_results)

        initial = _snapshot(engine, workflow_path)
        with engine.load(initial.document) as workflow:
            options = RunOptions(
                allow_external_writes=True,
                deadline_ms=arguments.deadline_ms,
                trace_capture=TraceCapture.FULL,
                max_output_bytes=2 * 1024 * 1024,
                max_state_bytes=2 * 1024 * 1024,
                max_events=1_000,
                max_in_flight_tasks=2,
            )
            with workflow.create_run(options) as run:
                run_started = time.perf_counter()
                output = await run.execute_async(
                    {
                        "question": arguments.question,
                        "change_request": arguments.change_request,
                        "expected_version": initial.version,
                        "requested_at": datetime.now(timezone.utc).isoformat(),
                    }
                )
                run_elapsed_ms = (time.perf_counter() - run_started) * 1000

    branch_sum_ms = sum(item["elapsed_ms"] for item in timings.values())
    return {
        "output": output,
        "concurrency": {
            "parallel_nodes": ["agent", "inspect"],
            "node_timings": timings,
            "run_elapsed_ms": run_elapsed_ms,
            "branch_elapsed_sum_ms": branch_sum_ms,
            "overlap_observed": run_elapsed_ms < branch_sum_ms,
            "explanation": "两个分支由 DAGE fan-out，并通过异步 executor 重叠执行",
        },
        "trace_events": len(traces),
    }


def validate(workflow_path: Path) -> dict[str, Any]:
    """无需 API Key 的安装/JSON/DAGE 编译检查。"""
    workflow_path = workflow_path.resolve()
    _ensure_workflow(workflow_path)
    with Engine() as engine:
        snapshot = _snapshot(engine, workflow_path)
        with engine.load(snapshot.document) as workflow:
            return {
                "valid": True,
                "version": snapshot.version,
                "digest": snapshot.digest,
                "mermaid": workflow.mermaid(),
            }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("question", nargs="?", default="请说明这次运行做了什么")
    parser.add_argument("--change-request", default="", help="让模型修改 workflow.json 的要求")
    parser.add_argument("--workflow", type=Path, default=DEFAULT_WORKFLOW)
    parser.add_argument("--model", help="覆盖 OPENAI_MODEL")
    parser.add_argument("--base-url", help="覆盖 OPENAI_BASE_URL，例如本地兼容服务 /v1 地址")
    parser.add_argument("--api-key", help="不推荐：命令行可能进入 shell 历史；优先环境变量或交互输入")
    parser.add_argument("--deadline-ms", type=int, default=120_000)
    parser.add_argument("--model-timeout", type=float, default=60.0)
    parser.add_argument("--inspect-delay", type=float, default=0.25)
    parser.add_argument("--offline", action="store_true", help="不用 Key/网络，运行完整并发 smoke test")
    parser.add_argument("--validate", action="store_true", help="不调用 LLM，只校验工作流")
    return parser.parse_args()


def main() -> None:
    arguments = parse_args()
    try:
        result = validate(arguments.workflow) if arguments.validate else asyncio.run(execute(arguments))
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except (DageError, OSError, ValueError, RuntimeError) as error:
        raise SystemExit(f"错误：{error}") from error


if __name__ == "__main__":
    main()
