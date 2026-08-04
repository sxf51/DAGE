# Language binding conformance vectors

Every supported language binding must consume these files directly. A copied or language-specific
Workflow is not conformance.

The suite currently requires each binding to:

1. load `valid.json`;
2. export Mermaid containing `flowchart TD` and the UTF-8 node display name `done-完成`;
3. reject `invalid.json` through the language's normal error mechanism;
4. replace only `__EXECUTOR__` in `async_tool.json` and verify asynchronous success, failure, and
   start-fenced cancellation;
5. replace only `__EXECUTOR__` in `async_effect.json` and verify idempotent effect commit;
6. release all native handles on every path, including late completion and cancellation races.

The successful result also contains `基础设施-🚀`, covering both BMP and supplementary-plane
Unicode. Java executes the Workflow and verifies the exact string to guard against JNI modified
UTF-8 regressions.

Set `DAGE_CONFORMANCE_DIR` to this directory when invoking a binding test from another working
directory. Python, Java, C#, Rust, and Node.js consume the same synchronous and asynchronous
Workflow documents. Future vectors are added here first and adopted by every binding in the same
change.
