"""Use a LangChain Runnable as a host-owned DAGE async Executor.

This demo is deliberately local and deterministic: it needs no model account or API key. Replace
``chain`` with a prompt/model/parser RunnableSequence to call a real model. DAGE still owns Workflow
execution semantics; LangChain and the application own prompts, models, credentials, and tools.
"""

import argparse
import asyncio
import json

from dage import Engine, RunOptions
from langchain_core.runnables import RunnableLambda


WORKFLOW = {
    "format": "dage-workflow",
    "format_version": "0.2.0",
    "entry": "answer",
    "nodes": {
        "answer": {
            "type": "llm",
            "executor": "langchain_answer",
            "effects": {"kind": "external_read", "replay": "safe"},
            "input": {"question": "${workflow.input.question}"},
            "next": "done",
        },
        "done": {"type": "end", "input": "${nodes.answer.output}"},
    },
}


async def normalize(value):
    question = value.get("question") if isinstance(value, dict) else None
    if not isinstance(question, str) or not question.strip():
        raise ValueError("input.question must be a non-empty string")
    return {"question": question.strip()}


async def local_answer(value):
    """Deterministic stand-in for ChatOpenAI/ChatAnthropic/etc."""
    await asyncio.sleep(0)
    return {
        "answer": f"LangChain received: {value['question']}",
        "provider": "local-runnable",
    }


chain = (
    RunnableLambda(normalize, name="normalize_question")
    | RunnableLambda(local_answer, name="local_answer")
)


async def execute(question):
    traces = []
    with Engine() as engine:
        engine.set_trace_sink(traces.append)

        async def langchain_executor(context, value):
            return await chain.ainvoke(
                value,
                config={
                    "tags": ["dage"],
                    "metadata": {
                        "dage_run_id": context.run_id,
                        "dage_node_id": context.node_id,
                    },
                },
            )

        engine.register_async_executor("langchain_answer", langchain_executor)
        with engine.load(WORKFLOW) as workflow:
            with workflow.create_run(RunOptions(deadline_ms=10_000)) as run:
                output = await run.execute_async({"question": question})

    return {"output": output, "trace_events": len(traces)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("question", nargs="?", default="What is DAGE?")
    arguments = parser.parse_args()
    print(json.dumps(asyncio.run(execute(arguments.question)), indent=2))


if __name__ == "__main__":
    main()
