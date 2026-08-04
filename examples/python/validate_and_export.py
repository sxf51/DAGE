import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "bindings", "python"))
from dage import Engine

workflow_path = os.path.join(os.path.dirname(__file__), "..", "workflows", "all_nodes.json")
with open(workflow_path, encoding="utf-8") as stream:
    document = json.load(stream)

with Engine() as engine:
    with engine.load(document) as workflow:
        print(workflow.mermaid())
