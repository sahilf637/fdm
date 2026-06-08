# Plan — In-browser video downloads (quality picker, "all internet cases")

## Goal
Let the browser extension offer "Download this video" with a quality picker for
streaming sites (HLS/DASH/progressive) and YouTube — IDM-style — routing the
actual byte transfer through FDM's fast multi-connection engine.

## Decisions (locked)
- **Backend:** bundle `yt-dlp` (extraction, quality, YouTube signature) + `ffmpeg` (mux).
- **Transfer:** yt-dlp *resolves* direct URLs → FDM engine downloads → ffmpeg muxes.
  True segmented HLS/DASH falls back to yt-dlp's own downloader.
- **Scope:** general media downloader (YouTube works via yt-dlp, not marketed).
- **UI:** in-page floating panel (content script) with a quality dropdown.
- **Excluded:** DRM (Widevine/PlayReady/FairPlay) — detect & refuse, never attempt.

## Architecture
```
content script (in-page panel + quality dropdown)
   ▲ detected-media          │ user picks quality
background SW ── webRequest sniffer (per-tab media registry)
   │ probe-video / download-video  (native messaging)
   ▼
fdm-native-host ── probe-video: runs `yt-dlp -J`, returns curated formats (stateless, fast)
   │ download-video ──(local socket)──▶
   ▼
fdm-gui  VideoJob: yt-dlp -g (fresh URL+headers) → FDM engine download → ffmpeg -c copy mux
```
Probe is stateless and snappy → handled in the **host**. Download is long-lived →
forwarded to the **GUI** (mirrors today's download forwarding in host/main.cpp).

## Detection — every case
| Case | Trigger | yt-dlp input |
|---|---|---|
| Progressive `.mp4/.webm` | response Content-Type video/* | media URL |
| HLS `.m3u8` / DASH `.mpd` | manifest seen on the wire | manifest URL |
| YouTube / known sites | content script knows the watch page | page URL |
| Blob / MSE (`blob:`) | underlying manifest from webRequest | manifest URL (blob ignored) |
| DRM (EME/Widevine) | `requestMediaKeySystemAccess` hook / `#EXT-X-KEY` | none → "protected" |
| Live HLS | no end tag | disabled in v1 |

## Changes by component
- **Extension:** `webRequest` + `scripting` + `tabs` perms; `content.js`+panel CSS;
  background webRequest sniffer + per-tab registry (`chrome.storage.session`,
  SW is ephemeral); two native calls `probe-video` / `download-video`; EME hook for DRM.
- **Native host (`host/main.cpp`):** add a `type` discriminator (keep legacy
  `{url,filename,headers}` download path working). `probe-video` runs bundled
  `yt-dlp -J`, returns a curated format list (metadata only, not URLs). `download-video`
  forwards to the GUI.
- **GUI/store:** new `VideoJob` orchestrator — `yt-dlp -g -f <id> --print http_headers`
  for fresh URL(s)+headers, drive the engine (video-only + audio-only = one item with
  combined progress), then `ffmpeg -i v -i a -c copy out`. Segmented → yt-dlp downloads,
  parse its progress. DownloadManager models a multi-file job + a mux post-step + temp cleanup.
- **Engine:** minimal — replay yt-dlp's `http_headers` per request (header plumbing
  already exists via `ResumeSpec.headers` / `start(...,headers,...)`).
- **IPC (`ipc/IpcProtocol.h`):** extend framed JSON with the `type` + video fields.
- **Packaging:** bundle yt-dlp + an **LGPL** ffmpeg next to the binaries; add a yt-dlp
  self-update check (YouTube breaks it often).

## Phasing
1. **Probe path** — host `probe-video` + bundled yt-dlp → curated formats. ✅ done
2. **In-page panel** — webRequest detection + badge + quality dropdown. ✅ done
3. **Download path** — `DownloadManager::startVideo` → yt-dlp child download+mux,
   shown as a `kind='video'` row with live progress. ✅ done
4. **Segmented / merge** — same yt-dlp child path handles HLS/DASH/YouTube-merge
   uniformly (no separate code). ✅ done
5. **DRM + live detection + updater** — MAIN-world EME hook → panel refuses DRM;
   `is_live` → panel refuses live; Tools ▸ "Update video downloader" runs
   `yt-dlp -U`. ✅ done
   - *Packaging* (bundle yt-dlp + LGPL ffmpeg into the installer) belongs to the
     cross-platform installer epic; the discovery hook (`findHelper`, next-to-
     binary then PATH) is already in place.

## Edge cases
Separate audio+video → two downloads, one item, mux + cleanup. Expired resolved URL →
re-resolve & resume. Playlists → single video in v1. Age-gated → forward cookies to yt-dlp.
Missing yt-dlp/ffmpeg → friendly error. Filenames from yt-dlp `title`, sanitized.

## Verification (end state)
Progressive `.mp4`, public HLS `.m3u8`, a Creative-Commons YouTube video (1080p =
video+audio mux; audio-only option), a DRM page (refused), and an age-gated CC video
(cookies). Each: badge → pick quality → correct file that plays.

## Notes
- Licensing: yt-dlp is Unlicense; use an **LGPL** ffmpeg build to bundle cleanly.
- DRM stays excluded by design (legal + technical).

## Transfer path (implemented)
Video downloads are `kind='video'` rows. `startVideo` → `resolveVideo` runs
`yt-dlp -J` to get the chosen stream URL(s) + headers, then:
- **Single-file https streams** (YouTube adaptive video/audio, progressive) →
  the **multi-connection engine** downloads each stream in parallel to a temp
  file, then **ffmpeg `-c copy`** muxes them. This bypasses YouTube's per-
  connection throttling. (`requested_formats` → `VideoEngineJob`.)
- **Segmented HLS/DASH** (protocol m3u8/dash) → fall back to the **yt-dlp child
  process** (`spawnVideo`), which fragments+muxes itself.

Cookies are NOT forwarded (a logged-in YouTube session forces the PO-token-gated
web client → no formats). Container/ext follows yt-dlp's choice (mp4 or mkv).
pause/cancel tear down engine streams + ffmpeg; resume/retry re-resolve (URLs
expire) and restart.
