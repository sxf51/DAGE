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

## Examples

- `examples/python/run.py`: minimal synchronous Workflow and pure host Executor.
- `examples/python/langchain_host.py`: a LangChain `Runnable` hosted as an async DAGE Executor;
  deterministic and API-key-free, with the real-model replacement boundary called out in source.
- `examples/python/service_host.py`: full asyncio service-host contract with durable SQLite CAS,
  restore ownership, idempotent effects, traces, leases, deadlines and cancellation.

From the repository root, after installing a platform wheel:

```powershell
python -m pip install -r examples/python/requirements-langchain.txt
python examples/python/langchain_host.py "Explain durable workflows"
```

Inputs and outputs are JSON-compatible Python values. The Workflow document maps the public input
into each node; registered Executors receive the resolved node input and return a JSON-compatible
node output. An `end` node selects the public Run output. DAGE owns orchestration semantics, while
the host owns model/tool implementations, credentials, authorization, storage and network policy.
