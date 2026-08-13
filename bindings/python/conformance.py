"""Shared DAGE language-binding conformance test."""
import asyncio
import os
import threading
import time
from pathlib import Path

from dage import DageError, Engine, EngineOptions, RunOptions, TraceCapture


class MemoryStateStore:
    def __init__(self):
        self.lock = threading.Lock()
        self.records = {}

    def put(self, run_id, checkpoint):
        with self.lock:
            version = self.records.get(run_id, {}).get("version", 0) + 1
            self.records[run_id] = {"version": version, "epoch": 0,
                                    "owner": "", "checkpoint": checkpoint}

    def get(self, run_id):
        with self.lock:
            record = self.records.get(run_id)
            return None if record is None else record["checkpoint"]

    def erase(self, run_id):
        with self.lock:
            self.records.pop(run_id, None)

    def load(self, run_id):
        with self.lock:
            record = self.records.get(run_id)
            return None if record is None else dict(record)

    def compare_exchange(self, run_id, expected_version, checkpoint):
        with self.lock:
            record = self.records.get(run_id)
            actual = 0 if record is None else record["version"]
            if actual != expected_version:
                return None
            version = expected_version + 1
            self.records[run_id] = {"version": version, "epoch": 0,
                                    "owner": "", "checkpoint": checkpoint}
            return version

    def claim(self, run_id, expected_version, owner):
        with self.lock:
            record = self.records.get(run_id)
            if record is None or record["version"] != expected_version:
                return None
            record = dict(record)
            record.update(version=expected_version + 1, epoch=record["epoch"] + 1,
                          owner=owner)
            self.records[run_id] = record
            return dict(record)

    def list(self, prefix):
        with self.lock:
            return sorted(key for key in self.records if key.startswith(prefix))


class TestLease:
    def __init__(self, provider):
        self.provider = provider
        self.lease_id = "python-conformance-lease"
        self.fencing_token = 7
        self.expires_at_unix_ms = int(time.time() * 1000) + 10_000

    def renew(self, ttl_ms):
        self.expires_at_unix_ms = int(time.time() * 1000) + ttl_ms
        return self.expires_at_unix_ms

    def release(self):
        self.provider.releases += 1


class TestLeaseProvider:
    def __init__(self):
        self.acquires = 0
        self.releases = 0

    def acquire(self, request, cancelled):
        assert "resources" in request and not cancelled()
        self.acquires += 1
        return TestLease(self)


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

    with Engine(options=EngineOptions(max_nodes=1024)) as engine:
        assert engine.has_capability("c.runtime_spi.v1")
        registry = engine.runtime_registry()
        assert "statuses" in registry and "callbacks" in registry
        scheduled = []
        traces = []

        def schedule(task, continuation):
            scheduled.append(continuation)
            task.run()

        engine.set_scheduler(schedule)
        engine.set_trace_sink(traces.append)
        state_store = MemoryStateStore()
        leases = TestLeaseProvider()
        engine.set_state_store(state_store)
        engine.set_resource_lease_provider(leases)
        with engine.load(valid) as workflow:
            graph = workflow.mermaid()
            assert "flowchart TD" in graph and "done-完成" in graph
            unchanged = workflow.diff(workflow)
            patch = {
                "base_digest": unchanged["before_digest"],
                "changes": [{
                    "action": "set_field", "node_id": "done", "field": "input",
                    "value": {"conformance": "patched"},
                }],
            }
            assert workflow.dry_run_patch(patch)["valid"] is True
            assert "affected_nodes" in workflow.analyze_patch(patch)
            with workflow.apply_patch(patch) as changed:
                changed_diff = changed.diff(workflow)
                assert changed_diff["before_digest"] != changed_diff["after_digest"]
            with workflow.create_run(RunOptions(
                trace_capture=TraceCapture.FULL, deadline_ms=10_000,
                retry_budget=2, max_output_bytes=1024 * 1024,
            )) as run:
                result = run.execute({})
                assert result["conformance"] is True
                checkpoint = run.checkpoint()
                assert isinstance(run.snapshot(), dict)
            with engine.restore(workflow, checkpoint) as restored:
                assert isinstance(restored.snapshot(), dict)
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

        assert scheduled and traces and state_store.records
        assert leases.acquires > 0 and leases.acquires == leases.releases

        with engine.load(async_tool.replace("__EXECUTOR__", "async_fail")) as workflow:
            with workflow.create_run() as run:
                try:
                    await run.execute_async({})
                except DageError:
                    pass
                else:
                    raise AssertionError("async Executor exception was not translated")
                errors = engine.callback_errors()
                assert errors and errors[0][0] == "async_executor:async_fail"

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

        cancellation_started.clear()
        cancellation_seen.clear()
        with engine.load(async_tool.replace("__EXECUTOR__", "async_wait")) as workflow:
            run = workflow.create_run()
            task = asyncio.create_task(run.execute_async({"close": True}))
            await asyncio.wait_for(cancellation_started.wait(), timeout=1)
            try:
                run.execute({})
            except RuntimeError:
                pass
            else:
                raise AssertionError("Run accepted concurrent operations")
            await asyncio.to_thread(run.close)
            try:
                await task
            except DageError:
                pass
            else:
                raise AssertionError("Run close did not fence the active native call")

    with Engine() as diagnostic_engine:
        def broken_trace_sink(_event):
            raise RuntimeError("contained trace failure")

        diagnostic_engine.set_trace_sink(broken_trace_sink)
        with diagnostic_engine.load(valid) as workflow, workflow.create_run() as run:
            assert run.execute({})["conformance"] is True
        errors = diagnostic_engine.callback_errors()
        assert errors and errors[0][0] == "trace_sink"

    lifetime_engine = Engine()
    lifetime_workflow = lifetime_engine.load(valid)
    lifetime_run = lifetime_workflow.create_run()
    try:
        lifetime_workflow.close()
    except RuntimeError:
        pass
    else:
        raise AssertionError("Workflow closed while a Run was still live")
    lifetime_run.close()
    try:
        lifetime_engine.close()
    except RuntimeError:
        pass
    else:
        raise AssertionError("Engine closed while a Workflow was still live")
    lifetime_workflow.close()
    lifetime_engine.close()
    print("Python binding conformance passed")


if __name__ == "__main__":
    asyncio.run(main())
