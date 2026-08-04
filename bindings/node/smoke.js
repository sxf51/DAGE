"use strict";
const path = require("node:path");
const dage = require("./index");
(async () => {
  const workflow = path.join(__dirname, "..", "..", "examples", "workflows", "all_nodes.json");
  const graph = await dage.mermaid(workflow);
  if (!graph.includes("flowchart TD")) throw new Error("missing Mermaid graph");
  console.log("Node.js non-blocking smoke test passed");
})().catch((error) => { console.error(error); process.exitCode = 1; });
