// FDM Integration background worker.
//
// Auto-captures http(s) downloads: cancels the browser's own download, gathers
// the URL + auth context (cookies, referer, user-agent), and hands it to the
// native host, which forwards it to FDM. A toolbar toggle turns capture on/off,
// and a context-menu item works regardless of the toggle.

const HOST = "com.fdm.native_host";
const NOTIF_ICON = "icons/icon48.png";

// URLs we re-injected as a browser fallback (host unreachable). We skip the
// onCreated they trigger so we don't loop. Keyed by URL -> expiry timestamp.
const recentFallback = new Map();

function suppressed(url) {
  const exp = recentFallback.get(url);
  if (exp === undefined) return false;
  recentFallback.delete(url);
  return exp > Date.now();
}

// Cache the toggle in memory so onCreated can decide synchronously and cancel
// the browser's download before it makes progress. Kept in sync with storage.
let enabled = true;
chrome.storage.local.get({ enabled: true }).then((v) => {
  enabled = v.enabled;
});
chrome.storage.onChanged.addListener((changes, area) => {
  if (area === "local" && changes.enabled) enabled = changes.enabled.newValue;
});

function isHttp(url) {
  return typeof url === "string" && /^https?:/i.test(url);
}

async function cookieHeader(url) {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    if (!cookies || !cookies.length) return "";
    return cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  } catch (e) {
    return "";
  }
}

function leafName(item) {
  // item.filename is often empty at onCreated time; FDM resolves the real name
  // from Content-Disposition anyway, so a best-effort leaf is fine.
  const name = item && item.filename ? item.filename : "";
  if (!name) return "";
  const parts = name.split(/[\\/]/);
  return parts[parts.length - 1];
}

function sendToFdm(req) {
  return new Promise((resolve) => {
    chrome.runtime.sendNativeMessage(HOST, req, (resp) => {
      if (chrome.runtime.lastError) {
        resolve({ ok: false, error: chrome.runtime.lastError.message });
      } else {
        resolve(resp || { ok: false, error: "no response" });
      }
    });
  });
}

function notify(message) {
  try {
    chrome.notifications.create({
      type: "basic",
      iconUrl: NOTIF_ICON,
      title: "FDM Integration",
      message,
    });
  } catch (e) {
    /* notifications are best-effort */
  }
}

async function captureUrl(url, opts = {}) {
  const headers = { "User-Agent": navigator.userAgent };
  const cookie = await cookieHeader(url);
  if (cookie) headers["Cookie"] = cookie;
  if (opts.referrer) headers["Referer"] = opts.referrer;

  const resp = await sendToFdm({ url, filename: opts.filename || "", headers });
  if (!resp.ok) {
    notify("Couldn't reach FDM — downloading in the browser instead.");
    recentFallback.set(url, Date.now() + 15000);
    chrome.downloads.download({ url });
  }
  return resp;
}

chrome.downloads.onCreated.addListener((item) => {
  const url = (item && (item.finalUrl || item.url)) || "";
  if (!enabled) return;
  if (suppressed(url)) return;
  if (!isHttp(url)) return;
  takeOver(item, url);
});

async function takeOver(item, url) {
  // Cancel + remove the browser's own download; FDM takes over.
  try {
    await chrome.downloads.cancel(item.id);
  } catch (e) {
    /* may already have completed/failed */
  }
  try {
    await chrome.downloads.erase({ id: item.id });
  } catch (e) {
    /* ignore */
  }
  await captureUrl(url, { filename: leafName(item), referrer: item.referrer });
}

// Context menu — explicit "Download with FDM", independent of the toggle.
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "fdm-download",
    title: "Download with FDM",
    contexts: ["link", "image", "video", "audio"],
  });
});

chrome.contextMenus.onClicked.addListener((info) => {
  const url = info.linkUrl || info.srcUrl;
  if (!isHttp(url)) return;
  captureUrl(url, { referrer: info.pageUrl });
});
