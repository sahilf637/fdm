# AMO submission notes (Firefox Add-ons)

Copy-paste source for the addons.mozilla.org listing + reviewer notes. Not
shipped in the extension.

---

## Listing

**Name:** FDM Integration

**Summary (≤250 chars):**
> Send your browser downloads to FDM, the Fresh Download Manager. Files and
> videos download faster over multiple connections, with your login cookies
> carried across so protected downloads keep working.

**Categories:** Download Management

**Full description:**
> FDM Integration hands your downloads to the **FDM desktop app** instead of the
> browser's built-in downloader. When you start a download, FDM opens its New
> Download dialog pre-filled with the URL, filename, and authentication context
> (cookies, referer, user-agent), then fetches the file over several parallel
> connections with pause/resume support. You can also right-click a link or
> video and choose **Download with FDM**, or pick a video quality from the
> in-page panel.
>
> **Requires the free FDM desktop application** (Linux), which provides the
> native-messaging host the extension talks to. Install it from:
> https://github.com/sahilf637/fdm
>
> Without the desktop app installed, the extension cannot do anything — it has
> no remote service of its own. It sends your download details only to FDM on
> your own computer and transmits nothing to any external server.

**Privacy policy URL:** link to the hosted copy of `PRIVACY.md`
(e.g. https://github.com/sahilf637/fdm/blob/main/PRIVACY.md)

**Support email:** sahilfartyal3@gmail.com
**Homepage:** https://github.com/sahilf637/fdm

---

## Data collection disclosure (AMO form)

- Does the add-on collect or transmit user data to the developer or third
  parties? **No.** Download details are passed only to a local application via
  native messaging; nothing is sent off-device. No analytics or telemetry.

---

## Permission justifications (for reviewers)

| Permission | Why it is needed |
|------------|------------------|
| `nativeMessaging` | Core function: forwards the download request to the local FDM app (`com.fdm.native_host`) over stdio. |
| `downloads` | Cancels the browser's own download so FDM can take it over instead. |
| `cookies` | Reads cookies for the download's domain so login-protected files download correctly in FDM. |
| `webRequest` (non-blocking) | Captures the request's headers (Referer, User-Agent, etc.) to reproduce the authenticated request in FDM. |
| `<all_urls>` (host permission + content scripts) | Downloads and videos can originate on any site, so capture and the in-page video panel must be able to run anywhere. |
| `contextMenus` | Adds the right-click "Download with FDM" entry. |
| `notifications` | Tells the user when FDM can't be reached and the download fell back to the browser. |
| `storage` | Persists local UI settings (e.g. the auto-capture toggle). On-device only. |

**Content script running in `world: "MAIN"` (`eme-hook.js`):** runs in the
page's own context to wrap `navigator.requestMediaKeySystemAccess`. This is the
only reliable way to detect that media is **DRM-protected** (EME) so the
extension refuses to "download" something undownloadable. It only reads the EME
key-system string and posts a local `window.postMessage`; it injects no other
page code and contacts no network.

---

## Reviewer notes

- All JavaScript is hand-written and unminified; no build step or bundler.
- The companion native-messaging host (`fdm-native-host`) is open source in the
  same repository (`host/`), as is the entire app.
- Minimum Firefox version is **140** (`data_collection_permissions` needs Fx140;
  the `world: "MAIN"` DRM-detection content script needs Fx128 — 140 covers both
  and matches the current ESR).
- **Mark the add-on as Firefox desktop only** (not Firefox for Android) in the
  AMO compatibility settings: it requires the FDM **desktop** application, so it
  cannot function on Android. (web-ext's lone remaining lint warning is just the
  Android `data_collection_permissions` floor, 142, and is moot here.)

---

## Pre-upload checklist

```sh
extension/pack-firefox.sh                       # build extension/dist-firefox/
npx web-ext lint  -s extension/dist-firefox     # must be error-free
npx web-ext build -s extension/dist-firefox     # produces the .zip to upload
```

Then submit the zip at https://addons.mozilla.org/developers/ as a **Listed**
add-on, paste the fields above, attach a screenshot (reuse `docs/screenshots/`),
and submit for review.
