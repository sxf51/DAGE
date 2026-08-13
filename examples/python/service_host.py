"""Production-host contract example for embedding DAGE in an asyncio service.

The SQLite adapter is deliberately host-owned. Workflow catalog selection, tenant
authorization, model routing, credentials, and database choice are application policy,
not DAGE Core behavior.
"""

import argparse
import asyncio
import json
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from pathlib import Path

from dage import DageError, Engine, RunOptions, TraceCapture


WORKFLOW = {
    "format": "dage-workflow",
    "format_version": "0.2.0",
    "entry": "llm",
    "nodes": {
        "llm": {
            "type": "llm", "executor": "host_llm",
            "effects": {"kind": "external_read", "replay": "safe"},
            "input": "${workflow.input}", "next": "tool",
        },
        "tool": {
            "type": "tool", "executor": "host_tool",
            "effects": {
                "kind": "external_write", "replay": "idempotent",
                "idempotency_key": "${run.id}:${node.id}",
            },
            "input": "${nodes.llm.output}", "next": "done",
        },
        "done": {"type": "end", "input": "${nodes.tool.output}"},
    },
}


class SQLiteStateStore:
    """Small durable CAS adapter; production hosts may provide another implementation."""

    def __init__(self, path):
        self.path = str(path)
        with closing(self._connect()) as database:
            database.execute(
                "CREATE TABLE IF NOT EXISTS runs ("
                "run_id TEXT PRIMARY KEY, version INTEGER NOT NULL, epoch INTEGER NOT NULL, "
                "owner TEXT NOT NULL, checkpoint TEXT NOT NULL)"
            )

    def _connect(self):
        database = sqlite3.connect(self.path, timeout=10, isolation_level=None)
        database.execute("PRAGMA busy_timeout=10000")
        database.execute("PRAGMA journal_mode=WAL")
        return database

    def put(self, run_id, checkpoint):
        with closing(self._connect()) as database:
            database.execute("BEGIN IMMEDIATE")
            row = database.execute(
                "SELECT version FROM runs WHERE run_id=?", (run_id,)).fetchone()
            version = (row[0] if row else 0) + 1
            database.execute(
                "INSERT INTO runs VALUES (?, ?, 0, '', ?) "
                "ON CONFLICT(run_id) DO UPDATE SET version=excluded.version, "
                "checkpoint=excluded.checkpoint",
                (run_id, version, checkpoint),
            )
            database.commit()

    def get(self, run_id):
        with closing(self._connect()) as database:
            row = database.execute(
                "SELECT checkpoint FROM runs WHERE run_id=?", (run_id,)).fetchone()
            return None if row is None else row[0]

    def erase(self, run_id):
        with closing(self._connect()) as database:
            database.execute("DELETE FROM runs WHERE run_id=?", (run_id,))

    def load(self, run_id):
        with closing(self._connect()) as database:
            row = database.execute(
                "SELECT version, epoch, owner, checkpoint FROM runs WHERE run_id=?",
                (run_id,),
            ).fetchone()
            if row is None:
                return None
            return dict(zip(("version", "epoch", "owner", "checkpoint"), row))

    def compare_exchange(self, run_id, expected_version, checkpoint):
        with closing(self._connect()) as database:
            database.execute("BEGIN IMMEDIATE")
            row = database.execute(
                "SELECT version FROM runs WHERE run_id=?", (run_id,)).fetchone()
            actual = row[0] if row else 0
            if actual != expected_version:
                database.rollback()
                return None
            version = expected_version + 1
            database.execute(
                "INSERT INTO runs VALUES (?, ?, 0, '', ?) "
                "ON CONFLICT(run_id) DO UPDATE SET version=excluded.version, "
                "checkpoint=excluded.checkpoint",
                (run_id, version, checkpoint),
            )
            database.commit()
            return version

    def claim(self, run_id, expected_version, owner):
        with closing(self._connect()) as database:
            database.execute("BEGIN IMMEDIATE")
            row = database.execute(
                "SELECT version, epoch, checkpoint FROM runs WHERE run_id=?", (run_id,)
            ).fetchone()
            if row is None or row[0] != expected_version:
                database.rollback()
                return None
            version, epoch = expected_version + 1, row[1] + 1
            updated = database.execute(
                "UPDATE runs SET version=?, epoch=?, owner=? "
                "WHERE run_id=? AND version=?",
                (version, epoch, owner, run_id, expected_version),
            )
            if updated.rowcount != 1:
                database.rollback()
                return None
            database.commit()
            return {"version": version, "epoch": epoch, "owner": owner,
                    "checkpoint": row[2]}

    def list(self, prefix):
        with closing(self._connect()) as database:
            return [row[0] for row in database.execute(
                "SELECT run_id FROM runs WHERE run_id LIKE ? ORDER BY run_id",
                (prefix.replace("%", "\\%") + "%",),
            )]


class HostLease:
    def __init__(self, provider):
        self.provider = provider
        self.lease_id = f"host-{provider.next_fence}"
        self.fencing_token = provider.next_fence
        provider.next_fence += 1
        self.expires_at_unix_ms = int(time.time() * 1000) + 30_000

    def renew(self, ttl_ms):
        self.expires_at_unix_ms = int(time.time() * 1000) + ttl_ms
        return self.expires_at_unix_ms

    def release(self):
        self.provider.releases += 1


class HostLeaseProvider:
    def __init__(self):
        self.next_fence = 1
        self.releases = 0

    def acquire(self, request, cancelled):
        if cancelled():
            return None
        if "resources" not in request:
            raise ValueError("invalid DAGE resource request")
        return HostLease(self)


def configure_engine(database, traces, effects, scheduler, cancellation_started=None):
    engine = Engine()
    engine.set_state_store(SQLiteStateStore(database))
    engine.set_trace_sink(traces.append)
    engine.set_resource_lease_provider(HostLeaseProvider())
    engine.set_scheduler(lambda task, _continuation: scheduler.submit(task.run))

    async def llm(context, value):
        await asyncio.sleep(0)
        return {"answer": value["prompt"].upper(), "trace_run_id": context.run_id}

    async def tool(context, value):
        await asyncio.sleep(0)
        effects.setdefault(context.idempotency_key, value)
        return {"stored": value["answer"], "idempotency_key": context.idempotency_key}, True

    async def wait_for_cancel(context, value):
        cancellation_started.set()
        while not context.is_cancelled():
            await asyncio.sleep(0.001)
        return value

    engine.register_async_executor("host_llm", llm)
    engine.register_async_executor("host_tool", tool)
    if cancellation_started is not None:
        engine.register_async_executor("host_cancel", wait_for_cancel)
    return engine


async def restore_worker(database, checkpoint):
    traces, effects = [], {}
    with ThreadPoolExecutor(max_workers=4, thread_name_prefix="dage-scheduler") as scheduler:
        with configure_engine(database, traces, effects, scheduler) as engine:
            with engine.load(WORKFLOW) as workflow:
                with engine.restore(workflow, checkpoint) as run:
                    result = await run.execute_async({"prompt": "ignored-after-restore"})
                    return result


async def self_test():
    with tempfile.TemporaryDirectory(prefix="dage-service-host-") as directory:
        database = Path(directory) / "state.sqlite3"
        checkpoint_file = Path(directory) / "checkpoint.json"
        traces, effects = [], {}
        with ThreadPoolExecutor(max_workers=4, thread_name_prefix="dage-scheduler") as scheduler:
            with configure_engine(database, traces, effects, scheduler) as engine:
                with engine.load(WORKFLOW) as workflow:
                    options = RunOptions(allow_external_writes=True, deadline_ms=10_000,
                                         trace_capture=TraceCapture.FULL)
                    with workflow.create_run(options) as run:
                        result = await run.execute_async({"prompt": "hello"})
                        checkpoint = run.checkpoint()
                        checkpoint_file.write_text(checkpoint, encoding="utf-8")
        assert result["stored"] == "HELLO" and len(effects) == 1
        assert traces and len({event["trace_id"] for event in traces}) == 1

        command = [sys.executable, str(Path(__file__).resolve()), "--restore",
                   str(database), str(checkpoint_file)]
        first = subprocess.run(command, check=False, capture_output=True, text=True)
        if first.returncode:
            raise RuntimeError(first.stderr or first.stdout)
        second = subprocess.run(command, check=False, capture_output=True, text=True)
        assert second.returncode == 2, "a second process unexpectedly acquired the same Run"

        cancellation_started = threading.Event()
        cancel_workflow = dict(WORKFLOW)
        cancel_workflow["nodes"] = {
            "wait": {"type": "tool", "executor": "host_cancel",
                     "effects": {"kind": "pure", "replay": "safe"},
                     "input": "${workflow.input}"}
        }
        cancel_workflow["entry"] = "wait"
        with ThreadPoolExecutor(max_workers=4, thread_name_prefix="dage-scheduler") as scheduler:
            with configure_engine(database, [], {}, scheduler, cancellation_started) as engine:
                with engine.load(cancel_workflow) as workflow, workflow.create_run(
                        RunOptions(deadline_ms=30_000)) as run:
                    task = asyncio.create_task(run.execute_async({"cancel": True}))
                    await asyncio.to_thread(cancellation_started.wait, 2)
                    task.cancel()
                    try:
                        await task
                    except asyncio.CancelledError:
                        pass
                    else:
                        raise AssertionError("host cancellation did not propagate")
        print("Python service-host conformance passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--restore", nargs=2, metavar=("DATABASE", "CHECKPOINT"))
    arguments = parser.parse_args()
    if arguments.restore:
        database, checkpoint_path = arguments.restore
        try:
            asyncio.run(restore_worker(
                database, Path(checkpoint_path).read_text(encoding="utf-8")))
        except DageError:
            return 2
        return 0
    asyncio.run(self_test())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
