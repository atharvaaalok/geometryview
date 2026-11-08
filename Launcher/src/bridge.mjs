// The bridge between the launcher and the viewer's pages: requests from the AI app's tools to the
// most recently connected page, and the page's answers. Messages are JSON:
//   launcher to page:  { id, method, params }
//   page to launcher:  { id, result } or { id, error }

export class Bridge {
  #pages = [];
  #pending = new Map();
  #next = 1;
  #waiting = [];

  /// Takes a page's connection, from acceptWebSocket.
  attach(connection) {
    this.#pages.push(connection);
    connection.onmessage = (text) => {
      let message;
      try {
        message = JSON.parse(text);
      } catch {
        return;
      }
      const pending = this.#pending.get(message.id);
      if (!pending) return;
      this.#pending.delete(message.id);
      clearTimeout(pending.timer);
      if (message.error !== undefined) pending.reject(new Error(String(message.error)));
      else pending.resolve(message.result);
    };
    connection.onclose = () => {
      this.#pages = this.#pages.filter((page) => page !== connection);
      for (const [id, pending] of this.#pending) {
        if (pending.page === connection) {
          this.#pending.delete(id);
          clearTimeout(pending.timer);
          pending.reject(new Error("The viewer's page was closed."));
        }
      }
    };
    for (const resolve of this.#waiting.splice(0)) resolve(connection);
  }

  get connected() { return this.#pages.length > 0; }

  /// The newest page, waiting up to `timeoutMs` for one to connect.
  page(timeoutMs) {
    if (this.connected) return Promise.resolve(this.#pages.at(-1));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#waiting = this.#waiting.filter((w) => w !== done);
        reject(new Error("The viewer did not open in a browser in time."));
      }, timeoutMs);
      const done = (page) => {
        clearTimeout(timer);
        resolve(page);
      };
      this.#waiting.push(done);
    });
  }

  /// Asks the newest page to do `method`, and returns its result.
  async request(method, params = {}, { timeoutMs = 120_000, waitMs = 30_000 } = {}) {
    const page = await this.page(waitMs);
    const id = this.#next++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending.delete(id);
        reject(new Error(`The viewer did not answer ${method} in time.`));
      }, timeoutMs);
      this.#pending.set(id, { resolve, reject, timer, page });
      page.send(JSON.stringify({ id, method, params }));
    });
  }

  close() {
    for (const page of this.#pages) page.close();
  }
}
