# Packaging FDM as a `.deb`

This builds an **unconfined** Debian package so the browser native-messaging
host works (Snap/Flatpak confinement breaks it — that's why FDM ships as a
`.deb`, not a Snap).

## What the package installs

| Path | Contents |
|------|----------|
| `/usr/bin/fdm-gui`, `/usr/bin/fdm-cli`, `/usr/bin/fdm-native-host` | the three binaries |
| `/usr/share/applications/fdm.desktop` | launcher entry |
| `/usr/share/icons/hicolor/*/apps/fdm.png` | app icons |
| `/usr/lib/mozilla/native-messaging-hosts/com.fdm.native_host.json` | Firefox host registration |
| `/etc/opt/chrome/native-messaging-hosts/com.fdm.native_host.json` | Chrome host registration *(only if a Chrome ID was set)* |
| `/etc/chromium/native-messaging-hosts/com.fdm.native_host.json` | Chromium host registration *(same)* |

Installing the package registers the host **system-wide**, so a user just
installs the `.deb` + the published extension and they connect — no
`install-host.sh` step.

## Dependencies

Runtime deps are computed automatically by `dpkg-shlibdeps` (`${shlibs:Depends}`),
so the package depends on the correctly-named Qt/libcurl packages for whatever
release you build on (e.g. `libqt6sql6t64` on Ubuntu 24.04, `libqt6sql6` on
22.04). Two things shlibdeps can't see are added by hand in `debian/control`:

- **`libqt6sql6-sqlite`** — the Qt SQL SQLite driver is `dlopen`'d at runtime,
  not linked. Without it the app starts but can't open its database.
- **`ffmpeg`, `yt-dlp`** (Recommends) — only needed for video/streaming.

> Build on the **oldest** release you want to support; the resulting `.deb`
> installs on that release and newer. For multiple releases, build once per
> release (a container or `sbuild` is the usual way).

## Build it

```sh
# one-time: install the Debian build toolchain
sudo apt install build-essential debhelper cmake \
                 libcurl4-openssl-dev qt6-base-dev qt6-base-dev-tools

# from the repo root -- binary-only, unsigned:
dpkg-buildpackage -b -us -uc

# the package lands one level up:
ls ../fresh-download-manager_1.0.0_*.deb
```

Install / test:

```sh
sudo apt install ../fresh-download-manager_1.0.0_*.deb     # apt pulls the dependencies
fdm-gui                                  # or launch "FDM" from the app grid
```

## Wiring the published extensions (the productized flow)

The host only talks to extension IDs it's been told to trust. After publishing:

1. **Firefox** — set the add-on's gecko ID (`browser_specific_settings.gecko.id`
   in [`extension/manifest.firefox.json`](../extension/manifest.firefox.json));
   it defaults to `{d324a09c-fb86-4cff-918a-54a1a1e4bf1a}`.
2. **Chrome** — the Web Store assigns a permanent ID on publish.
3. Rebuild the package with both baked in:

   ```sh
   FDM_FIREFOX_EXTENSION_ID={d324a09c-fb86-4cff-918a-54a1a1e4bf1a} \
   FDM_CHROME_EXTENSION_ID=<web-store-id>
   # pass them through dh:
   DEB_CMAKE_EXTRA_FLAGS="-DFDM_FIREFOX_EXTENSION_ID={d324a09c-fb86-4cff-918a-54a1a1e4bf1a} \
                          -DFDM_CHROME_EXTENSION_ID=<web-store-id>" \
   dpkg-buildpackage -b -us -uc
   ```

   (Or add the `-D...` flags to `override_dh_auto_configure` in `debian/rules`.)

Until a Chrome ID is set, the Chrome/Chromium manifests are simply not installed
(Firefox still works).

## Caveats

- **Snap Firefox** (Ubuntu's default) can't reach a host-installed bridge — its
  sandbox blocks it. Direct users to Mozilla's `.deb` Firefox, or to Chrome /
  Chromium `.deb`. This is a confinement limit, not a packaging bug.
- No `LICENSE` file exists yet; `debian/copyright` has a TODO. Pick a license
  before publishing.
