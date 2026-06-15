# GUI Redesign — Design & Implementation Plan

## 1. Problems with the current UI

From `gui/MainWindow.cpp` and the screenshots in `docs/screenshots/`:

- **Everything is one flat table.** Active downloads, failed ones, and months of
  history are interleaved. You can't see "what is downloading right now" at a
  glance.
- **Progress is a text percentage** ("25.0%") instead of a progress bar; speed
  and ETA are invisible unless a row happens to be active.
- **Stock `QStyle` icons** (SP_MediaPause etc.) look dated and inconsistent.
- **Toolbar is a flat strip of 9 always-visible actions**, most disabled most of
  the time. No search, no filtering, no batch operations.
- **Single selection only** — no "pause these three" or "clear all completed".
- **Modal popups** for completion interrupt the user.
- No empty-state, no global stats (total speed / active count beyond "Ready").

## 2. Design direction

**Keep Qt Widgets.** A QML rewrite would be a multi-week rebuild for no
functional gain. Widgets + a custom item delegate + an app-wide QSS theme gets
us a modern look (references: AB Download Manager, Motrix, FDM 6) while reusing
all existing wiring (tray, IPC, dialogs, details window).

Core architectural pieces:

1. **`QSortFilterProxyModel`** between `DownloadListModel` and the view —
   powers search, category filters, and sorting. (Requires the model to expose
   custom roles; see §5.)
2. **Custom `QStyledItemDelegate`** rendering each download as a two-line card
   row instead of 6 text columns.
3. **App-wide QSS theme + bundled SVG icon set** (single accent color,
   light/dark variants tinted from the system palette), shipped in `fdm.qrc`.

## 3. Main window layout

```
┌────────────────────────────────────────────────────────────────────┐
│  [＋ Add ▾]  [⏸ Pause all] [▶ Resume all]      [🔍 Search…    ] [⋮] │  top bar
├──────────────┬─────────────────────────────────────────────────────┤
│  All      12 │  ┌───────────────────────────────────────────────┐  │
│ ▸Downloading2│  │ 📦 ubuntu-24.04.iso                  ⏸  ✕  ⋯  │  │
│  Completed 7 │  │ ▕████████████▏62% · 4.2 MiB/s · ETA 3m · 8 cx │  │
│  Failed    2 │  ├───────────────────────────────────────────────┤  │
│  Paused    1 │  │ 🎬 lecture-video.mp4                  ⏸  ✕  ⋯ │  │
│  ──────────  │  │ ▕███▏18% · 1.1 MiB/s · ETA 12m                │  │
│  Videos    3 │  ├───────────────────────────────────────────────┤  │
│              │  │ 📄 report.pdf      2.3 MiB   Jun 8   Completed│  │
│              │  │    https://example.com/report.pdf             │  │
│              │  └───────────────────────────────────────────────┘  │
├──────────────┴─────────────────────────────────────────────────────┤
│  ⬇ 5.3 MiB/s total · 2 active · 7 completed                        │  status bar
└────────────────────────────────────────────────────────────────────┘
```

- **Sidebar (left, fixed ~170px)**: category filters with live counts —
  *All / Downloading / Completed / Failed / Paused* by status, plus *Videos*
  by `kind`. **Downloading is the default view on launch when anything is
  active**, which gives the requested "current downloads separate from
  history". In *All*, active rows sort to the top.
- **Top bar** replaces the current toolbar:
  - **Add** split-button: *Add URL…* (current dialog), *Add from clipboard*
    (pre-fills if clipboard holds a URL), *Add video…*.
  - **Pause all / Resume all** batch actions.
  - **Search field** (right-aligned, `Ctrl+F`): live substring filter over
    name + URL.
  - **Overflow menu (⋮)**: Remove completed from list, Update yt-dlp, Quit.
  - Menu bar stays (File/Download/Tools) for discoverability + shortcuts.
- **Row-level actions move into the rows**: hover/selected rows show inline
  pause/resume, cancel, and a "⋯" menu (retry, redownload, open folder,
  details, remove). The context menu keeps the full set. This empties the
  toolbar of the 7 perpetually-disabled buttons.
- **Status bar**: aggregate speed, active count; replaces "Ready".
- **Empty state**: centered hint ("No downloads yet — press Ctrl+N or drop a
  URL") instead of a blank grid.

### Row card spec (delegate)

Two visual variants on one delegate:

- **Active/Paused row (taller, ~56px)**: file-type icon (via
  `QFileIconProvider`/mime), filename (bold, elided), inline action buttons;
  second line: slim progress bar + `62% · 4.2 MiB/s · ETA 3m12s · 8 conn`
  (paused shows `Paused at 62%`). Data already available in
  `DownloadLiveRow` (`bytesReceived`, `bytesPerSec`, `activeConnections`);
  ETA = remaining / speed.
- **Finished row (compact, ~44px)**: icon, filename, size, finished date
  (`updatedAt`), colored status pill (green Completed / red Failed with error
  tooltip / gray Cancelled), URL in dim small text. Failed rows get an inline
  *Retry* button.

Status colors: Completed `#2da44e`, Failed `#d1242f`, Active accent blue,
Paused amber, Cancelled gray.

## 4. Behavior / UX changes

- **Multi-select** (`ExtendedSelection`); all actions iterate the selection;
  remove-confirmation handles N items.
- **Sorting** by the proxy (date added desc default; active pinned first in
  *All*).
- **Double-click**: completed → open file; otherwise → details window
  (today it's always details).
- **Completion**: replace the modal `QMessageBox` with a tray notification
  (`QSystemTrayIcon::showMessage`) + transient status-bar message. Keep the
  modal only for downloads launched from the browser flow where no window is
  visible.
- **Drag & drop / paste**: drop or `Ctrl+V` a URL onto the window to open the
  Add dialog pre-filled.
- Details window and New Download dialog get the same QSS theme but keep
  their current structure (restyle only, last phase).

## 5. Required model/store changes (small, additive)

`DownloadListModel` currently only serves display text. Add:

- Custom roles: `IdRole`, `StatusRole`, `KindRole`, `BytesReceivedRole`,
  `TotalBytesRole`, `SpeedRole`, `CreatedAtRole`, `UpdatedAtRole`,
  `ErrorRole`, `UrlRole`, `OutputPathRole` — consumed by the delegate, proxy
  filter, and sorter.
- No schema or `DownloadManager` API changes needed. (`MainWindow` row-id
  lookups must map through the proxy — replace `idForRow(row)` calls with
  `data(IdRole)` on the view index.)

## 6. Implementation phases

Each phase compiles, runs, and is verifiable on its own.

1. **Model + proxy foundation** — add roles to `DownloadListModel`; introduce
   `DownloadFilterProxyModel` (status/kind/search filtering, sort, counts-per-
   category signal); rewire `MainWindow` id lookups through the proxy.
   *Verify: existing table still works, unit-testable proxy filtering.*
2. **Layout restructure** — sidebar + top bar (Add split-button, search box,
   pause/resume all, overflow), status-bar aggregates, multi-select, empty
   state. Still the plain table as the list.
   *Verify: category filters + search narrow the list live; batch actions work.*
3. **Visual pass** — `DownloadItemDelegate` (card rows, progress bar, pills,
   inline buttons), switch `QTableView`→`QListView`, bundle SVG icons, QSS
   theme with light/dark from system palette.
   *Verify: screenshot review against mockup; hover/inline actions fire.*
4. **UX behaviors** — completion notifications, double-click-to-open,
   drag-drop/paste URL, failed-row inline retry, "remove completed".
5. **Secondary windows** — restyle DownloadDetailsWindow + NewDownloadDialog
   to match the theme.

## 7. Open decisions

- **Sidebar vs. split panes**: alternative to the sidebar is a vertical split
  (always-visible "Active" pane on top, "History" table below). Sidebar is
  recommended — one view/delegate code path, scales to more categories, and
  search applies uniformly.
- **Settings dialog** (default download dir, connection limits, speed cap) is
  deliberately out of scope — it needs engine/store support and deserves its
  own plan.
