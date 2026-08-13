# dage-runtime

Python 3.11+ bindings for the embeddable DAGE DAG runtime. Official platform wheels include the
matching native runtime; source-tree development may set `DAGE_LIBRARY` explicitly.

```python
from dage import Engine

with Engine() as engine, engine.load(workflow_json) as workflow:
    with workflow.create_run() as run:
        result = run.execute({"prompt": "hello"})
```

The project is pre-1.0 and currently published as an Alpha API. See the repository documentation
for asyncio Executors, durability adapters, cancellation, runtime limits and release verification.
