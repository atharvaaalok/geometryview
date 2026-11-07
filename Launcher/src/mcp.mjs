// The Model Context Protocol over standard input and output: newline-delimited JSON-RPC 2.0.
// Only what a tool server needs: initialize, ping, tools/list and tools/call. Standard output
// carries the protocol alone, so anything else goes to standard error.

import { createInterface } from "node:readline";

const SERVER = { name: "geometry-viewer", version: "0.1.0" };
const LATEST_PROTOCOL = "2025-06-18";

/// Serves `tools` until standard input closes. Each tool is { name, description, inputSchema,
/// call(args) -> Promise<content[]> }; a thrown error becomes a tool error the model can read.
export function serve(tools, instructions) {
  const byName = new Map(tools.map((tool) => [tool.name, tool]));
  const reply = (id, result) => send({ jsonrpc: "2.0", id, result });
  const fail = (id, code, message) => send({ jsonrpc: "2.0", id, error: { code, message } });

  const handlers = {
    initialize: (params) => ({
      // Answer in the client's version: every version so far shares what this server uses.
      protocolVersion: params?.protocolVersion ?? LATEST_PROTOCOL,
      capabilities: { tools: {} },
      serverInfo: SERVER,
      instructions,
    }),
    ping: () => ({}),
    "tools/list": () => ({
      tools: tools.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })),
    }),
    "tools/call": async (params) => {
      const tool = byName.get(params?.name);
      if (!tool) {
        return { content: [{ type: "text", text: `There is no tool named ${params?.name}.` }],
                 isError: true };
      }
      try {
        return { content: await tool.call(params.arguments ?? {}) };
      } catch (error) {
        return { content: [{ type: "text", text: error.message ?? String(error) }], isError: true };
      }
    },
  };

  const lines = createInterface({ input: process.stdin, crlfDelay: Infinity });
  lines.on("line", async (line) => {
    if (!line.trim()) {
      return;
    }
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      fail(null, -32700, "Parse error");
      return;
    }
    const handler = handlers[message.method];
    if (message.id === undefined) {
      return; // a notification, such as notifications/initialized
    }
    if (!handler) {
      fail(message.id, -32601, `Method not found: ${message.method}`);
      return;
    }
    try {
      reply(message.id, await handler(message.params));
    } catch (error) {
      fail(message.id, -32603, error.message ?? String(error));
    }
  });
}

function send(message) {
  process.stdout.write(JSON.stringify(message) + "\n");
}
