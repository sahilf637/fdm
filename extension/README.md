# FDM Browser Integration

Redirects browser downloads into FDM: when you start a download, the browser's
own download is cancelled and FDM's **New Download** dialog opens pre-filled with
the URL, suggested filename, and the auth context (cookies / referer /
user-agent) so login-protected files still download.

## How it fits together

```
Browser ──(native messaging, stdio)──▶ fdm-native-host ──(local socket)──▶ fdm-gui
```

- **extension/** — the MV3 extension (Chrome + Firefox). Captures downloads and
  the right-click "Download with FDM" action.
- **fdm-native-host** — small local helper (built with the app) the browser is
  allowed to launch. Forwards the request to the running FDM over a local
  socket, starting FDM if it isn't open.
- **fdm-gui** — gains a single-instance IPC server; the request opens the
  pre-filled dialog in the running window.

## Setup (manual / local)

> If you installed FDM from the **`.deb`**, the native host is already registered
> system-wide (`/usr/lib/mozilla/native-messaging-hosts/`, plus `/etc/opt/chrome/`
> and `/etc/chromium/` when the package was built with a Chrome ID) — skip
> straight to loading the extension and **skip step 3**. The steps below are for
> running from a source build.

### 1. Build the app + host

```sh
cmake -S . -B build && cmake --build build -j
```

This produces `build/host/fdm-native-host` and `build/gui/fdm-gui`.

### 2. Load the extension (unpacked)

Chrome and Firefox need different `background` manifest keys (`service_worker`
vs `scripts`), and Chrome rejects the Firefox form — so there are two manifests.

- **Chrome / Chromium:** go to `chrome://extensions`, enable **Developer mode**,
  click **Load unpacked**, and select the `extension/` folder. Copy the
  **extension ID** shown on the card.
- **Firefox:** build the Firefox folder first, then load it:
  ```sh
  extension/pack-firefox.sh        # creates extension/dist-firefox/
  ```
  `about:debugging` → **This Firefox** → **Load Temporary Add-on** → select
  `extension/dist-firefox/manifest.json`. (Temporary add-ons reset on restart;
  for persistence use Developer/ESR with `xpinstall.signatures.required=false`,
  or sign via AMO. The extension ID is fixed: `{d324a09c-fb86-4cff-918a-54a1a1e4bf1a}`.)

### 3. Register the native host

```sh
# Firefox is registered immediately (fixed id). Add Chrome with its id:
extension/install-host.sh --chrome-id <ID from chrome://extensions>
```

Re-run with a different `--host-path` if your host binary lives elsewhere.

> **⚠️ Snap Firefox (Ubuntu default) is not supported.** The strictly-confined
> Firefox snap will not launch an external native-messaging host: it finds the
> manifest but the spawn fails inside the sandbox ("An unexpected error
> occurred"), and its runtime ships no Qt for the host anyway. There is no
> manifest/wrapper workaround. Use the **non-snap Firefox** (Mozilla's `.deb`
> from <https://packages.mozilla.org/apt>) or Chrome/Chromium `.deb`, which run
> unconfined and read `~/.mozilla/native-messaging-hosts/` normally.
> `install-host.sh` still writes to the snap/flatpak manifest dirs when present
> (correct location, harmless), but the snap sandbox is the blocker.

### 4. Verify

Open the extension popup → **Test connection** → should say *connected*. Then
click any direct file link: FDM's dialog appears pre-filled. The popup toggle
turns auto-capture on/off; the right-click **Download with FDM** works either
way.

## Notes

- **FDM does not need to be open.** If it isn't running, the native host launches
  it (it locates `fdm-gui` next to itself, or in the build tree, or on `PATH`),
  waits for it to come up, then injects the download.
- Only `http`/`https` URLs are captured (and validated at every layer). `blob:` /
  `data:` downloads are left to the browser.
- Cookies are forwarded over the local socket, never via process arguments.
- If FDM can't be reached, the extension notifies you and falls back to a normal
  browser download so nothing is lost. So "it downloaded in the browser" usually
  means the native host isn't registered for this browser yet (re-run
  `install-host.sh --chrome-id <ID>`) or `fdm-gui` couldn't be found/launched.
