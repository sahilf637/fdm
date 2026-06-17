# FDM Browser Extension — Privacy Policy

_Last updated: 2026-06-17_

The **FDM Integration** browser extension is a bridge between your browser and
the FDM download manager running on your own computer. It is designed so that
your data never leaves your machine.

## What the extension accesses

To redirect a download into FDM with the correct context, the extension reads:

- the **URL** of the file (or video) you choose to download;
- the **cookies**, **Referer**, and **User-Agent** associated with that request,
  so that login-protected downloads keep working;
- request/response headers for the download, via the `webRequest` API;
- the **URL of the page** you are on, when the in-page video panel is shown;
- a small amount of **local settings** (e.g. whether auto-capture is on), stored
  with the browser `storage` API.

## Where that data goes

All of the above is sent **only** to the FDM application on the same computer,
through the browser's [native messaging](https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/Native_messaging)
channel (`com.fdm.native_host`). From there FDM performs the download you asked
for.

- The extension **does not** send your data to the developer or to any remote or
  third-party server.
- The extension contains **no analytics, telemetry, advertising, or tracking**.
- Local settings stay in your browser profile on your device.

When you download a video, the URL and User-Agent are handed to FDM's local
helper, which runs `yt-dlp` on your machine; any network requests from that
point are the normal download traffic to the media site you chose — the same
as if you had downloaded it yourself.

## DRM detection

The extension observes calls to `requestMediaKeySystemAccess` (the EME API) only
to detect DRM-protected media so it can decline cleanly. This check happens
entirely in the page and is not recorded or transmitted.

## Permissions

Each permission the extension requests is used solely for the functionality
described above. The extension is open source; you can review exactly what it
does at <https://github.com/sahilf637/fdm>.

## Data retention

The extension stores no history. It holds a download's details only for the
moment it takes to forward them to FDM, then discards them.

## Contact

Questions about this policy: **sahilfartyal3@gmail.com**
