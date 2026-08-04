"use strict";
const { execFile } = require("node:child_process");
const { promisify } = require("node:util");
const execFileAsync = promisify(execFile);
const native = require("./build/Release/dage_node.node");

class Engine {
  #native;
  #closed = false;

  constructor() {
    this.#native = new native.Engine();
  }

  registerAsyncExecutor(name, callback) {
    if (this.#closed) throw new Error("Engine is closed");
    if (typeof callback !== "function") throw new TypeError("callback must be a function");
    this.#native.registerAsyncExecutor(name, callback);
  }

  async run(workflow, input = {}, options = {}) {
    if (this.#closed) throw new Error("Engine is closed");
    const signal = options.signal;
    const operation = this.#native.startRun(workflow, JSON.stringify(input));
    const cancel = () => operation.cancel(String(signal?.reason || "AbortSignal cancelled"));
    if (signal?.aborted) cancel();
    else signal?.addEventListener("abort", cancel, { once: true });
    try {
      return await operation.promise;
    } finally {
      signal?.removeEventListener("abort", cancel);
    }
  }

  close() {
    if (!this.#closed) {
      this.#native.close();
      this.#closed = true;
    }
  }
}
exports.Engine = Engine;

/**
 * Non-blocking Node.js adapter. It delegates protocol semantics to the DAGE CLI.
 * Set DAGE_CLI to the built executable.
 */
async function command(name, workflow, input) {
  const executable = process.env.DAGE_CLI || "dage";
  const args = [name, workflow];
  if (input !== undefined) args.push(JSON.stringify(input));
  const { stdout } = await execFileAsync(executable, args, {
    encoding: "utf8",
    windowsHide: true,
  });
  return stdout;
}
exports.validate = (workflow) => command("validate", workflow);
exports.format = (workflow) => command("format", workflow);
exports.mermaid = (workflow) => command("mermaid", workflow);
exports.dot = (workflow) => command("dot", workflow);
exports.run = async (workflow, input = {}) => JSON.parse(await command("run", workflow, input));
