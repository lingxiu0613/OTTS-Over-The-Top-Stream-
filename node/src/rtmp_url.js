import crypto from "crypto";

export function streamToken() {
  return process.env.OTTS_STREAM_TOKEN || "";
}

export function streamAuthSecret() {
  return process.env.OTTS_AUTH_SECRET || "";
}

export function isStreamTokenRequired(token = streamToken(), secret = streamAuthSecret()) {
  return Boolean(token || secret);
}

export function streamSignature(action, streamKey, expires, secret = streamAuthSecret()) {
  if (!secret || !action || !streamKey || !expires) {
    return "";
  }
  return crypto
    .createHmac("sha256", secret)
    .update(`${action}\n${streamKey}\n${expires}`)
    .digest("hex");
}

export function appendStreamAuth(url, action, streamKey, options = {}) {
  const token = options.token ?? streamToken();
  const secret = options.secret ?? streamAuthSecret();
  const ttlSeconds = Number(options.ttlSeconds || process.env.OTTS_AUTH_TTL_SECONDS || 3600);
  if (token) {
    const separator = url.includes("?") ? "&" : "?";
    return `${url}${separator}token=${encodeURIComponent(token)}`;
  }
  if (!secret) {
    return url;
  }
  const expires = String(Math.floor(Date.now() / 1000) + Math.max(1, ttlSeconds));
  const sign = streamSignature(action, streamKey, expires, secret);
  const separator = url.includes("?") ? "&" : "?";
  return `${url}${separator}expires=${encodeURIComponent(expires)}&sign=${encodeURIComponent(sign)}`;
}

export function appendRtmpToken(url, token = streamToken()) {
  if (!token) {
    return url;
  }
  const separator = url.includes("?") ? "&" : "?";
  return `${url}${separator}token=${encodeURIComponent(token)}`;
}

export function buildRtmpUrl(rtmpBase, streamKey, action = "publish") {
  return appendStreamAuth(`${rtmpBase}/${streamKey}`, action, streamKey);
}

export function queryParamFromUri(uri, key) {
  const raw = String(uri || "");
  const queryIndex = raw.indexOf("?");
  if (queryIndex < 0) {
    return "";
  }
  const query = raw.slice(queryIndex + 1);
  for (const part of query.split("&")) {
    if (!part) {
      continue;
    }
    const [name, ...rest] = part.split("=");
    if (decodeURIComponent(name || "") === key) {
      return decodeURIComponent(rest.join("=") || "");
    }
  }
  return "";
}

export function isStreamTokenAuthorizedFromUri(uri, action = "", streamKey = "", token = streamToken(), secret = streamAuthSecret()) {
  if (!isStreamTokenRequired(token, secret)) {
    return true;
  }
  if (token && queryParamFromUri(uri, "token") === token) {
    return true;
  }
  if (!secret || !action || !streamKey) {
    return false;
  }
  const expires = queryParamFromUri(uri, "expires");
  const sign = queryParamFromUri(uri, "sign").toLowerCase();
  if (!expires || !sign || Number(expires) < Math.floor(Date.now() / 1000)) {
    return false;
  }
  const expected = streamSignature(action, streamKey, expires, secret);
  return expected.length === sign.length && crypto.timingSafeEqual(Buffer.from(expected), Buffer.from(sign));
}

export function isValidSrtPassphrase(token = streamToken()) {
  return token.length >= 10 && token.length <= 79;
}

export function appendSrtPassphrase(url, token = streamToken()) {
  if (!isValidSrtPassphrase(token)) {
    return url;
  }
  const separator = url.includes("?") ? "&" : "?";
  return `${url}${separator}passphrase=${encodeURIComponent(token)}&pbkeylen=16`;
}

export function buildHttpFlvUrl(httpBase, streamKey, action = "play") {
  const cleanBase = String(httpBase || "http://127.0.0.1:8080").replace(/\/$/, "");
  return appendStreamAuth(`${cleanBase}/${streamKey}.flv`, action, streamKey);
}
