// The server side of WebSocket (RFC 6455), for the bridge between the launcher and the viewer's
// page: the handshake, text frames in both directions, ping and close. No dependencies; frames from
// the page are masked, as the protocol requires, and frames to it are not.

import { createHash } from "node:crypto";

const kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const kMaxMessage = 64 << 20; // a rendered picture, base64, with room to spare

/// Completes an upgrade request and returns the connection: { send(text), close(), onmessage,
/// onclose }. Answers 400 and returns null if the request is not a WebSocket handshake.
export function acceptWebSocket(request, socket) {
  const key = request.headers["sec-websocket-key"];
  if (request.headers.upgrade?.toLowerCase() !== "websocket" || !key) {
    socket.end("HTTP/1.1 400 Bad Request\r\n\r\n");
    return null;
  }
  const accept = createHash("sha1").update(key + kGuid).digest("base64");
  socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
               `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  socket.setNoDelay(true);

  const connection = { onmessage: null, onclose: null, open: true };
  let buffered = Buffer.alloc(0);
  let fragments = [];

  const frame = (opcode, payload) => {
    const length = payload.length;
    const header = length < 126 ? Buffer.from([0x80 | opcode, length])
                 : length < 65536 ? Buffer.from([0x80 | opcode, 126, length >> 8, length & 255])
                 : (() => {
                     const h = Buffer.alloc(10);
                     h[0] = 0x80 | opcode;
                     h[1] = 127;
                     h.writeBigUInt64BE(BigInt(length), 2);
                     return h;
                   })();
    socket.write(Buffer.concat([header, payload]));
  };

  const finish = () => {
    if (!connection.open) return;
    connection.open = false;
    connection.onclose?.();
  };

  connection.send = (text) => {
    if (connection.open) frame(0x1, Buffer.from(text, "utf8"));
  };
  connection.close = () => {
    if (connection.open) frame(0x8, Buffer.alloc(0));
    socket.end();
    finish();
  };

  socket.on("data", (chunk) => {
    buffered = Buffer.concat([buffered, chunk]);
    for (;;) {
      if (buffered.length < 2) return;
      const fin = (buffered[0] & 0x80) !== 0;
      const opcode = buffered[0] & 0x0f;
      const masked = (buffered[1] & 0x80) !== 0;
      let length = buffered[1] & 0x7f;
      let at = 2;
      if (length === 126) {
        if (buffered.length < 4) return;
        length = buffered.readUInt16BE(2);
        at = 4;
      } else if (length === 127) {
        if (buffered.length < 10) return;
        length = Number(buffered.readBigUInt64BE(2));
        at = 10;
      }
      if (!masked || length > kMaxMessage) {
        connection.close(); // a client must mask; and nothing this large is expected
        return;
      }
      if (buffered.length < at + 4 + length) return;
      const mask = buffered.subarray(at, at + 4);
      const payload = Buffer.from(buffered.subarray(at + 4, at + 4 + length));
      for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i & 3];
      buffered = buffered.subarray(at + 4 + length);
      if (opcode === 0x8) {
        connection.close();
        return;
      }
      if (opcode === 0x9) {
        frame(0xA, payload);
        continue;
      }
      if (opcode === 0x1 || opcode === 0x0) {
        fragments.push(payload);
        if (fin) {
          const text = Buffer.concat(fragments).toString("utf8");
          fragments = [];
          connection.onmessage?.(text);
        }
      }
    }
  });
  socket.on("close", finish);
  socket.on("error", finish);
  return connection;
}
