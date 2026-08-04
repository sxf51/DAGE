import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "bindings", "python"))
from dage import Engine

document = {
    "format": "dage-workflow",
    "format_version": "0.2.0",
    "entry": "echo",
    "nodes": {
        "echo": {
            "type": "tool",
            "executor": "echo",
            "effects": {"kind": "pure", "replay": "safe"},
            "input": {"message": "${workflow.input.message}"},
            "next": "done",
        },
        "done": {"type": "end", "input": {"result": "${nodes.echo.output.message}"}},
    },
}

with Engine() as engine:
    engine.register_executor("echo", lambda _node, value: value)
    with engine.load(document) as workflow:
        with workflow.create_run() as run:
            assert run.execute({"message": "hello from Python"}) == {
                "result": "hello from Python"
            }
            print("Python run smoke test passed")
