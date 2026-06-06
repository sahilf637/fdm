// FDM in-page video panel. Stays dormant until the background worker reports
// that downloadable media was seen on this tab; then it shows a floating
// "Download video" badge. Clicking it probes the page (yt-dlp via the host) and
// lists qualities; picking one hands it back to the background worker.
(() => {
  if (window.__fdmPanelLoaded) return;
  window.__fdmPanelLoaded = true;

  let badge = null;
  let panel = null;
  let manifestUrl = "";
  let videoTitle = "";
  let drm = false;

  function refreshBadge() {
    if (!badge) return;
    badge.textContent = drm ? "🔒  Protected (DRM)" : "⬇  Download video";
    badge.classList.toggle("fdm-badge-drm", drm);
  }

  function ensureBadge() {
    if (badge) {
      refreshBadge();
      return;
    }
    badge = document.createElement("div");
    badge.className = "fdm-badge";
    badge.addEventListener("click", openPanel);
    refreshBadge();
    document.documentElement.appendChild(badge);
  }

  function closePanel() {
    if (panel) {
      panel.remove();
      panel = null;
    }
  }

  function humanSize(n) {
    if (!n || n < 0) return "";
    const u = ["B", "KB", "MB", "GB"];
    let i = 0;
    while (n >= 1024 && i < u.length - 1) {
      n /= 1024;
      i++;
    }
    return ` · ${n.toFixed(n < 10 ? 1 : 0)} ${u[i]}`;
  }

  async function openPanel() {
    closePanel();
    panel = document.createElement("div");
    panel.className = "fdm-panel";
    panel.innerHTML = `<div class="fdm-row">
        <span class="fdm-title">Reading video…</span>
        <span class="fdm-close">×</span></div>`;
    panel.querySelector(".fdm-close").addEventListener("click", closePanel);
    document.documentElement.appendChild(panel);

    if (drm) {
      renderError("This video is DRM-protected and can't be downloaded.");
      return;
    }

    let resp;
    try {
      resp = await chrome.runtime.sendMessage({ fdm: "probe", manifest: manifestUrl });
    } catch (e) {
      resp = { ok: false, error: String(e) };
    }
    if (!panel) return;  // closed while waiting
    if (!resp || !resp.ok) {
      renderError((resp && resp.error) || "Couldn't read this video");
      return;
    }
    if (resp.isLive) {
      renderError("Live stream — can't be saved as a file");
      return;
    }
    renderFormats(resp);
  }

  function renderError(msg) {
    panel.innerHTML = `<div class="fdm-row">
        <span class="fdm-title">Can't download</span>
        <span class="fdm-close">×</span></div>
      <div class="fdm-err"></div>`;
    panel.querySelector(".fdm-err").textContent = msg;
    panel.querySelector(".fdm-close").addEventListener("click", closePanel);
  }

  function renderFormats(resp) {
    videoTitle = resp.title || "";
    const fmts = (resp.formats || []).slice();
    // Video first (highest resolution at top), then audio-only.
    fmts.sort((a, b) => (b.height || 0) - (a.height || 0));

    panel.innerHTML = `<div class="fdm-row">
        <span class="fdm-title"></span>
        <span class="fdm-close">×</span></div>
      <div class="fdm-list"></div>`;
    panel.querySelector(".fdm-title").textContent = resp.title || "Video";
    panel.querySelector(".fdm-close").addEventListener("click", closePanel);

    const list = panel.querySelector(".fdm-list");
    if (!fmts.length) {
      renderError("No downloadable formats found");
      return;
    }
    fmts.forEach((f) => {
      const item = document.createElement("button");
      item.className = "fdm-fmt";
      const left = document.createElement("span");
      left.textContent = f.label || f.formatId;
      const right = document.createElement("span");
      right.className = "fdm-fmt-size";
      right.textContent = humanSize(f.filesize) + (f.needsAudioMerge ? "  +audio" : "");
      item.append(left, right);
      item.addEventListener("click", () => pick(f, item));
      list.appendChild(item);
    });
  }

  async function pick(f, item) {
    item.classList.add("fdm-busy");
    const label = item.firstChild.textContent;
    item.firstChild.textContent = "Sending to FDM…";
    let r;
    try {
      r = await chrome.runtime.sendMessage({
        fdm: "download",
        manifest: manifestUrl,
        formatId: f.formatId,
        selector: f.selector,
        title: videoTitle,
      });
    } catch (e) {
      r = { ok: false, error: String(e) };
    }
    item.firstChild.textContent =
      r && r.ok ? "Sent to FDM ✓" : `Error: ${(r && r.error) || "failed"}`;
  }

  chrome.runtime.onMessage.addListener((msg) => {
    if (msg && msg.fdm === "media-available") {
      if (msg.manifest) manifestUrl = msg.manifest;
      ensureBadge();
    }
  });

  // The MAIN-world EME hook (eme-hook.js) tells us when the page uses DRM.
  window.addEventListener("message", (e) => {
    if (e.source === window && e.data && e.data.__fdmDRM) {
      drm = true;
      refreshBadge();
    }
  });

  // The worker may have detected media before this script attached, so ask once.
  chrome.runtime
    .sendMessage({ fdm: "get-media" })
    .then((r) => {
      if (r && r.available) {
        if (r.manifest) manifestUrl = r.manifest;
        ensureBadge();
      }
    })
    .catch(() => {});
})();
