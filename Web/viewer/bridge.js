// The page's side of the launcher's bridge: when the launcher opened this page, it passes a token,
// and the page takes requests from the AI app's tools over a WebSocket to the launcher, and
// answers them. See Launcher/src/bridge.mjs.

export function connectBridge(token, handlers) {
  const connect = () => {
    const socket = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}` +
                                 `/bridge?token=${encodeURIComponent(token)}`);
    socket.onmessage = async (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      const { id, method, params } = message;
      try {
        const handler = handlers[method];
        if (!handler) throw new Error(`The viewer does not know ${method}.`);
        socket.send(JSON.stringify({ id, result: await handler(params ?? {}) }));
      } catch (error) {
        socket.send(JSON.stringify({ id, error: String(error?.message ?? error) }));
      }
    };
    // The launcher may restart: keep trying, quietly.
    socket.onclose = () => setTimeout(connect, 2000);
  };
  connect();
}
