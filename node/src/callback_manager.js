function nowIso() {
  return new Date().toISOString();
}

function streamParts(streamKey) {
  const parts = String(streamKey || "").split("/").filter(Boolean);
  return {
    app: parts[0] || "live",
    stream: parts.slice(1).join("/") || parts[0] || "stream"
  };
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function postJson(url, payload, timeoutMs) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      signal: controller.signal
    });
    const text = await response.text();
    return {
      ok: response.ok,
      status: response.status,
      body: text.slice(0, 2048)
    };
  } finally {
    clearTimeout(timer);
  }
}

export class CallbackManager {
  constructor(options = {}) {
    this.configManager = options.configManager;
    this.maxEvents = options.maxEvents || 200;
    this.events = [];
  }

  getConfig() {
    return this.configManager?.getConfig()?.callbacks || { enabled: false, events: {} };
  }

  urlsFor(eventName) {
    const cfg = this.getConfig();
    const urls = cfg.events?.[eventName] || [];
    return Array.isArray(urls) ? urls.filter(Boolean) : [];
  }

  buildPayload(eventName, payload = {}) {
    const streamKey = payload.stream_key || payload.streamKey || "";
    const parts = streamParts(streamKey);
    const resolved = this.configManager?.resolveStream(streamKey);
    return {
      event: eventName,
      timestamp: nowIso(),
      stream_key: streamKey,
      vhost: resolved?.vhost || "__default__",
      app: payload.app || parts.app,
      stream: payload.stream || parts.stream,
      source_protocol: payload.source_protocol || payload.protocol || "unknown",
      client_ip: payload.client_ip || null,
      session_id: payload.session_id || payload.sessionKey || null,
      data: payload.data || {}
    };
  }

  remember(entry) {
    this.events.unshift(entry);
    if (this.events.length > this.maxEvents) {
      this.events.length = this.maxEvents;
    }
  }

  async emit(eventName, payload = {}) {
    const cfg = this.getConfig();
    const body = this.buildPayload(eventName, payload);
    const urls = this.urlsFor(eventName);
    const entry = {
      id: `${Date.now()}-${Math.random().toString(16).slice(2)}`,
      event: eventName,
      timestamp: body.timestamp,
      payload: body,
      enabled: Boolean(cfg.enabled),
      urls,
      deliveries: []
    };
    this.remember(entry);

    if (!cfg.enabled || !urls.length) {
      return entry;
    }

    const retries = Math.max(0, Number(cfg.retries || 0));
    const timeoutMs = Math.max(100, Number(cfg.timeoutMs || 3000));

    for (const url of urls) {
      const delivery = {
        url,
        attempts: []
      };
      entry.deliveries.push(delivery);

      for (let attempt = 0; attempt <= retries; attempt += 1) {
        try {
          const result = await postJson(url, body, timeoutMs);
          delivery.attempts.push({ at: nowIso(), attempt: attempt + 1, ...result });
          if (result.ok) {
            break;
          }
        } catch (error) {
          delivery.attempts.push({
            at: nowIso(),
            attempt: attempt + 1,
            ok: false,
            error: error instanceof Error ? error.message : "callback failed"
          });
        }
        if (attempt < retries) {
          await sleep(200);
        }
      }
    }

    return entry;
  }

  listEvents(limit = 100) {
    return this.events.slice(0, Math.max(1, Math.min(Number(limit) || 100, this.maxEvents)));
  }
}
