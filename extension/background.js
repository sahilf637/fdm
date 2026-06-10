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

// Record a fallback URL so its self-triggered onCreated is skipped. Prunes
// expired entries first: suppressed() only deletes on a hit, so a URL that
// falls back once and never recurs would otherwise linger for the worker's
// lifetime.
function rememberFallback(url) {
  const now = Date.now();
  for (const [u, exp] of recentFallback) {
    if (exp <= now) recentFallback.delete(u);
  }
  recentFallback.set(url, now + 15000);
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
    rememberFallback(url);
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

async function videoDownload(target, referrer) {
  if (!isHttp(target)) return;
  const resp = await sendToFdm({
    type: "download-video",
    url: target,
    selector: "",
    title: "",
    headers: videoHeaders(referrer),
  });
  if (!resp || !resp.ok) notify("Couldn't start the FDM video download.");
}

chrome.contextMenus.onClicked.addListener((info, tab) => {
  // Right-clicking a <video>/<audio> element: its src is usually a blob/MSE URL
  // that can't be fetched directly, so route it through the video path (yt-dlp)
  // against the media URL if it's a real one, else the page itself.
  if (info.mediaType === "video" || info.mediaType === "audio") {
    const target = isHttp(info.srcUrl)
      ? info.srcUrl
      : info.pageUrl || (tab && tab.url);
    if (isHttp(target)) {
      videoDownload(target, info.pageUrl);
      return;
    }
  }
  const url = info.linkUrl || info.srcUrl;
  if (!isHttp(url)) return;
  captureUrl(url, { referrer: info.pageUrl });
});

// --- video detection -------------------------------------------------------
// Watch network traffic for streaming media (HLS/DASH manifests, progressive
// audio/video) and keep a per-tab registry. The content script shows a
// "Download video" badge when a tab has any; picking a quality routes back here
// to probe (yt-dlp via the host) and, later, to download.
//
// The service worker is ephemeral, so the registry is mirrored into
// chrome.storage.session and rehydrated on startup.
let tabMedia = {};  // tabId -> { manifest: <url>, count: <int> }
chrome.storage.session.get({ tabMedia: {} }).then((v) => {
  tabMedia = v.tabMedia || {};
});
function persistMedia() {
  chrome.storage.session.set({ tabMedia });
}

// Classify a request as a streaming manifest, plain media, or neither.
function mediaKind(url, contentType) {
  const path = url.split(/[?#]/)[0].toLowerCase();
  const ct = (contentType || "").toLowerCase();
  if (path.endsWith(".m3u8") || path.endsWith(".mpd") ||
      ct.includes("mpegurl") || ct.includes("dash+xml")) {
    return "manifest";
  }
  if (/\.(mp4|webm|mkv|mov|m4a|m4s|ts)$/.test(path) ||
      ct.startsWith("video/") || ct.startsWith("audio/")) {
    return "media";
  }
  return null;
}

function recordMedia(tabId, url, kind) {
  const m = tabMedia[tabId] || (tabMedia[tabId] = { manifest: "", count: 0 });
  if (kind === "manifest") m.manifest = url;  // best probe input when present
  m.count++;
  persistMedia();
  // Nudge the content script to show/refresh its badge (ignored if absent).
  chrome.tabs
    .sendMessage(tabId, { fdm: "media-available", manifest: m.manifest })
    .catch(() => {});
}

chrome.webRequest.onResponseStarted.addListener(
  (d) => {
    if (d.tabId < 0) return;
    const ctHeader = (d.responseHeaders || []).find(
      (h) => h.name.toLowerCase() === "content-type",
    );
    const kind = mediaKind(d.url, ctHeader && ctHeader.value);
    if (kind) recordMedia(d.tabId, d.url, kind);
  },
  { urls: ["<all_urls>"] },
  ["responseHeaders"],
);

// A top-level navigation replaces whatever media the tab had.
chrome.webRequest.onBeforeRequest.addListener(
  (d) => {
    if (d.type === "main_frame") {
      delete tabMedia[d.tabId];
      persistMedia();
    }
  },
  { urls: ["<all_urls>"], types: ["main_frame"] },
);

chrome.tabs.onRemoved.addListener((tabId) => {
  delete tabMedia[tabId];
  persistMedia();
});

// Pick the best URL to hand yt-dlp: a captured manifest if we saw one
// (raw HLS/DASH players), otherwise the page URL (YouTube and friends).
function probeTarget(tab, manifest) {
  return manifest || (tab && tab.url) || "";
}

// Video extraction (yt-dlp) deliberately omits cookies: forwarding a logged-in
// YouTube session forces the web client, which now requires a PO token and
// returns no downloadable formats ("Requested format is not available"). Public
// videos work fine without them. (Authenticated video is a future opt-in.)
function videoHeaders(referrer) {
  const headers = { "User-Agent": navigator.userAgent };
  if (referrer) headers["Referer"] = referrer;
  return headers;
}

// Bridge messages from the in-page panel (content script) to the native host.
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  const tabId = sender.tab && sender.tab.id;
  if (!msg || !msg.fdm) return;

  if (msg.fdm === "get-media") {
    const m = tabId != null ? tabMedia[tabId] : null;
    sendResponse({ available: !!(m && m.count), manifest: m ? m.manifest : "" });
    return;  // synchronous
  }

  if (msg.fdm === "probe") {
    (async () => {
      const pageUrl = (sender.tab && sender.tab.url) || "";
      const url = probeTarget(sender.tab, msg.manifest);
      if (!isHttp(url)) return sendResponse({ ok: false, error: "no probeable URL" });
      // Replay the page as Referer: many CDNs gate manifest/segment URLs on it.
      sendResponse(await sendToFdm({ type: "probe-video", url, headers: videoHeaders(pageUrl) }));
    })();
    return true;  // async
  }

  if (msg.fdm === "download") {
    (async () => {
      const pageUrl = (sender.tab && sender.tab.url) || "";
      const target = probeTarget(sender.tab, msg.manifest);
      sendResponse(await sendToFdm({
        type: "download-video",
        url: target,
        pageUrl,
        manifest: msg.manifest || "",
        formatId: msg.formatId || "",
        selector: msg.selector || "",
        title: msg.title || "",
        headers: videoHeaders(pageUrl),
      }));
    })();
    return true;  // async
  }
});
