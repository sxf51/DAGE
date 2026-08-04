"""Shared DAGE language-binding conformance test."""
import asyncio
import os
from pathlib import Path

from dage import DageError, Engine


async def main():
    vectors = Path(
        os.environ.get(
            "DAGE_CONFORMANCE_DIR",
            Path(__file__).resolve().parents[2] / "tests" / "conformance" / "bindings",
        )
    )
    valid = (vectors / "valid.json").read_text(encoding="utf-8")
    invalid = (vectors / "invalid.json").read_text(encoding="utf-8")
    async_tool = (vectors / "async_tool.json").read_text(encoding="utf-8")

    with Engine() as engine:
        with engine.load(valid) as workflow:
            graph = workflow.mermaid()
            assert "flowchart TD" in graph and "done-完成" in graph
        try:
            engine.load(invalid)
        except DageError:
            pass
        else:
            raise AssertionError("invalid Workflow was accepted")

        async def echo(_context, value):
            await asyncio.sleep(0)
            return value

        cancellation_started = asyncio.Event()
        cancellation_seen = asyncio.Event()

        async def wait_for_cancel(context, value):
            cancellation_started.set()
            while not context.is_cancelled():
                await asyncio.sleep(0.001)
            cancellation_seen.set()
            return value

        async def fail(_context, _value):
            raise RuntimeError("must not cross the ctypes callback boundary")

        engine.register_async_executor("async_echo", echo)
        engine.register_async_executor("async_wait", wait_for_cancel)
        engine.register_async_executor("async_fail", fail)
        with engine.load(async_tool.replace("__EXECUTOR__", "async_echo")) as workflow:
            with workflow.create_run() as run:
                result = await run.execute_async({"message": "异步完成"})
                assert result["message"] == "异步完成"

        with engine.load(async_tool.replace("__EXECUTOR__", "async_fail")) as workflow:
            with workflow.create_run() as run:
                try:
                    await run.execute_async({})
                except DageError:
                    pass
                else:
                    raise AssertionError("async Executor exception was not translated")

        with engine.load(async_tool.replace("__EXECUTOR__", "async_wait")) as workflow:
            with workflow.create_run() as run:
                task = asyncio.create_task(run.execute_async({"cancel": True}))
                await asyncio.wait_for(cancellation_started.wait(), timeout=1)
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass
                else:
                    raise AssertionError("async Run ignored task cancellation")
                await asyncio.wait_for(cancellation_seen.wait(), timeout=1)
    print("Python binding conformance passed")


if __name__ == "__main__":
    asyncio.run(main())
