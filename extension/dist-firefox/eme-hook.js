// FDM DRM probe — runs in the page's MAIN world (so it can see the page's own
// navigator). Any use of EME (Encrypted Media Extensions) means the media is
// DRM-protected (Widevine / PlayReady / FairPlay) and cannot be downloaded; we
// flag it via postMessage so the in-page panel refuses cleanly instead of
// handing an undownloadable URL to yt-dlp.
(() => {
  const nav = navigator;
  if (!nav.requestMediaKeySystemAccess) return;
  const orig = nav.requestMediaKeySystemAccess.bind(nav);
  nav.requestMediaKeySystemAccess = function (keySystem, configs) {
    try {
      window.postMessage({ __fdmDRM: true, keySystem: String(keySystem) }, "*");
    } catch (e) {
      /* ignore */
    }
    return orig(keySystem, configs);
  };
})();
