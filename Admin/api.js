export class ApiError extends Error {
  constructor(status, message) { super(message); this.status = status; }
}

// Owns session-related UI state. The actual session cookie is HttpOnly and is
// never read by JavaScript, written to localStorage or sent in a URL.
export class AccountApi {
  csrfToken = "";

  async request(path, { method = "GET", body, timeoutMs = 10000 } = {}) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    const headers = { Accept: "application/json" };
    if (method !== "GET") {
      headers["Content-Type"] = "application/json";
      headers["X-Accounts-Request"] = "1";
      if (this.csrfToken) headers["X-CSRF-Token"] = this.csrfToken;
    }
    try {
      const response = await fetch(path, {
        method, headers, credentials: "same-origin", cache: "no-store",
        body: body === undefined ? undefined : JSON.stringify(body), signal: controller.signal
      });
      const data = await response.json();
      if (!response.ok) throw new ApiError(response.status, data.error || "Request failed");
      if (data.csrfToken) this.csrfToken = data.csrfToken;
      return data;
    } finally { clearTimeout(timeout); }
  }
}
