"use strict";
const fs = require("node:fs");
const path = require("node:path");
const { Engine } = require("./index.js");

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
async function withTimeout(promise, message) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(message)), 5000);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function main() {
  const vectors = process.env.DAGE_CONFORMANCE_DIR ||
    path.join(__dirname, "..", "..", "tests", "conformance", "bindings");
  const valid = fs.readFileSync(path.join(vectors, "valid.json"), "utf8");
  const invalid = fs.readFileSync(path.join(vectors, "invalid.json"), "utf8");
  const asyncTool = fs.readFileSync(path.join(vectors, "async_tool.json"), "utf8");
  const asyncEffect = fs.readFileSync(path.join(vectors, "async_effect.json"), "utf8");
  const engine = new Engine();
  try {
    const output = await engine.run(valid, {});
    if (output.utf8 !== "基础设施-🚀") throw new Error("Run output lost UTF-8");
    await engine.run(invalid, {}).then(
      () => { throw new Error("invalid Workflow was accepted"); },
      () => undefined,
    );

    engine.registerAsyncExecutor("node_echo", async (_context, input) => input);
    engine.registerAsyncExecutor("node_fail", async () => {
      throw new Error("expected Node.js Executor failure");
    });
    engine.registerAsyncExecutor("node_effect", async (context, input) => {
      if (!context.idempotencyKey) throw new Error("missing idempotency key");
      context.commitEffect();
      return input;
    });
    const template = (executor) => asyncTool.replace("__EXECUTOR__", executor);
    const echoed = await engine.run(template("node_echo"), { message: "Node-🚀" });
    if (echoed.message !== "Node-🚀") throw new Error("async output lost UTF-8");
    await engine.run(template("node_fail"), {}).then(
      () => { throw new Error("Executor failure was not translated"); },
      () => undefined,
    );
    const effect = await engine.run(
      asyncEffect.replace("__EXECUTOR__", "node_effect"), { effect: true });
    if (effect.effect !== true) throw new Error("effect completion failed");

    let startedResolve;
    let seenResolve;
    const started = new Promise((resolve) => { startedResolve = resolve; });
    const seen = new Promise((resolve) => { seenResolve = resolve; });
    engine.registerAsyncExecutor("node_wait", async (context, input) => {
      startedResolve();
      while (!context.isCancelled()) await delay(1);
      seenResolve();
      return input;
    });
    const controller = new AbortController();
    const execution = engine.run(
      template("node_wait"), { cancel: true }, { signal: controller.signal });
    await withTimeout(started, "async Executor did not start");
    try {
      engine.close();
      throw new Error("Engine closed with active Run");
    } catch (error) {
      if (!String(error.message).includes("active Runs")) throw error;
    }
    controller.abort("Node conformance cancellation");
    await execution.then(
      () => { throw new Error("AbortSignal cancellation was ignored"); },
      () => undefined,
    );
    await withTimeout(seen, "Executor did not observe cancellation");
  } finally {
    engine.close();
  }
  console.log("Node.js binding conformance passed");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
