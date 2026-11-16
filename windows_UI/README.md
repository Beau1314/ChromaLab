# Windows Qt Quick frontend

This is a native Qt Quick/C++ application using the existing synchronous
`spk_desktop_host`. There is no Python, Node, browser or subprocess renderer at
runtime. The upstream SwiftUI theme supplied the grey rails, centre canvas,
warm accent and bottom strip; unsupported controls are omitted deliberately.

Implemented: multi-file RAW import and drag/drop; independent per-photo
film/paper, decode, exposure, grain, halation, glare, film format and filter
settings; a navigator and selected-photo strip; free/ratio cropping; and
TIFF16, PNG8/16, JPEG8 export with JPEG quality and optional downsize. Batch
export processes selected photos serially, supports cancellation between files,
and skips existing files. Single export refuses overwrites. Output stays sRGB
with the bundled ICC profile. Select-all and synchronization copy supported
settings to selected photos, preserving their own crops by default.

Ctrl+O imports, Ctrl+E opens export options, Ctrl+0 fits and Ctrl+1 selects
physical 100%. Double-click also switches fit/100%. Changes update the preview
automatically after a 180 ms settling delay. The filmstrip restores each
photo's wanted edits, including edits pending when the user switches away.
Edits automatically save under the application's local data directory (`Edits`)
and restore when the same photo is reopened. RAW files and their directories
are not modified. Save failures, incompatible records, changed sources and
conflicting writes from another app instance remain visible and preserve the
existing record. After a concurrent-write conflict, restart the application and
reopen the photo to load the other instance's saved settings. Undo/redo is per
photo, bounded to 64 in-memory steps, and a
slider drag is one step. Undo history itself is not persisted. Only the current
photo is fully decoded/developed; unvisited
photos have an explicit placeholder until opened. Small thumbnails are retained,
not one full RAW/GPU session per imported photo.

Crop is post-render geometry and does not re-run the emulsion. Preview,
navigator and exports share the same rounded source-pixel edges. The first
Windows crop supports free drag, 1:1, 3:2, 4:3 and 16:9, clockwise/counterclockwise
quarter turns, horizontal/vertical flips and -45 to +45 degree straightening.
Straightening applies on release, uses original 16-bit samples and zooms to
fill the crop; quarter turns/flips are lossless. The crop editor uses the
unrotated source explicitly, while presets describe the final output ratio.
Masked full-photo strip thumbnails remain pending. The strip currently
shows the cropped output. See [DESIGN_ALIGNMENT.md](DESIGN_ALIGNMENT.md) for the
author's drawing transcription and upstream comparison.

Slide film is scanned directly, matching the upstream macOS film-stage rule.
The host resolves this from each profile's `info.type`, including Provia 100F,
Velvia 100, Ektachrome 100 and Kodachrome 64. Paper and paper-brightness controls
are disabled for slides; their previous settings return with negative film.
The colour model, profile data and shader maths are unchanged.

Layer 2 editing grades the developed print: exposure, contrast, brightness,
highlights, shadows, black/white points, saturation, scan white balance
(temperature/tint), five-channel curves (RGB, luma, R, G, B), four-zone colour
balance and vignette. One CPU implementation serves preview and export; it is
ported expression by expression from the desktop port's
`desktop/src/shared/adjustments.ts` — the verified port of the macOS
`Adjustments.swift`/`Shaders.metal` layer 2 — and field names, defaults and the
JSON record match that file, so a record saved here still opens on the Mac and
desktop builds. Grade edits apply to the already-rendered print and do not
re-render the engine; export grades the print with the same code before
geometry, so preview and exported pixels agree. Grade values ride the same
persistence, gesture and per-photo undo integration as the other settings; one
slider drag is one undo step. The 对比 toggle (V) splits the canvas between the
print without the grade on the left and the current edit on the right.

An output histogram plots 256 bins per channel (R/G/B/luma) over the published
print — geometry and grade included. A 宽容度 (scene latitude) card analyses the
current development on request: medium/scene latitude, the scene histogram
against the medium ramp and the fit verdict. The analysis is read-only —
applying the solved scene placement to the settings is still refused on
Windows. White balance rows set decode-side temperature (2000–50000 K) and tint
(±150), backfill the decoder-reported As Shot values, or choose one of six
named illuminant presets; changing them re-decodes the current photo, and a WB
override is refused while the headroom decode is selected. The 测光 switch turns
automatic exposure off so film exposure alone determines scene placement.

All engine work runs on one serial worker. Edits during a render are coalesced
into the newest settings; superseded results never replace the displayed frame
or newer controls. File dialogs pause pending updates until they close; opening
and exporting lock conflicting commands. A failed
current render retains the displayed frame and restores its controls; a retry
is available. Export captures that same immutable frame, including when a new
preview is pending, and pending edits resume after export succeeds or fails.
Closing during work waits for the current operation. A failed image-publication
allocation is reconciled against the displayed source/decode mode on the next
render.

Ctrl+Z undoes the current photo; Ctrl+Shift+Z / Ctrl+Y redo. Text editors retain
their own undo shortcuts. Synchronization is undoable on each recipient photo.
Batch export and file dialogs lock history commands along with other edits.
`--edit-store <absolute-directory>` overrides the persistent directory; all
automated tests inject an isolated directory, including `--self-test` and
`--snapshot`, so no test reads or writes the user's real edit store. Persistent
photo identity includes canonical path, size, modification time and first/last
64 KiB hash. Renamed/moved source files are not automatically matched yet.

## Build

Use the official Qt 6.11.3 Windows MinGW SDK with its MinGW-w64 13.1 compiler.
Do not link these Qt binaries with the earlier GCC 15 Windows build. Build into
an independent directory; the script changes only its own process PATH.
Use `-Fresh` to discard an existing CMake configuration before rebuilding.

```powershell
# From the repository root. The bootstrap uses only PowerShell and Windows
# bsdtar, downloads into ../deps and checks pinned SHA256 for every file.
./windows_UI/setup-qt.ps1 -DependencyDirectory ../deps
./windows_UI/build.ps1 -SourceDirectory . -BuildDirectory ./build/qt -RunTests
```

Local dependencies are in the workspace's `deps/qt-6.11.3`, alongside Vulkan
headers, glslang and LibRaw used by the engine. `-DependencyDirectory` overrides
that location. The output `app/` is self-contained for this Windows machine;
keep the complete directory with its DLLs, QML plugins, resources and notices.
The Vulkan driver is supplied by Windows/the installed GPU driver.

`dependencies/qt-packages.json` lists the exact official archives and hashes;
`dependencies/qt-licenses.json` lists each official source license and hash.
`setup-qt.ps1 -VerifyOnly` verifies the existing archive/license cache and tool
versions without network or extraction. The initial download is about 278 MB.
This script does not install Vulkan headers, glslang, CMake or LibRaw; set those
up using the repository's Windows engine instructions first. It neither
downloads an unpinned "latest" SDK nor depends on a private local manifest.

For a parent build, enable the native RAW and desktop host targets and call
`add_subdirectory(windows_UI)` after `add_subdirectory(engine)`. The Qt frontend
links the existing `spk_desktop_host`; the macOS build is untouched. Resources
are synchronized on every build, including builds where the executable itself
does not need relinking. Deployment is an explicit build-script step.

## Validation

The first validated build used Qt 6.11.3 / MinGW 13.1 on an RTX 5070 Ti:
16 engine CTests passed, followed by the real 4688 x 7028 ARW scene test below.
The same decoded input produced byte-identical deterministic RGBA16 full and
print-edit outputs under this compiler and the existing GCC 15.2 baseline.
The Vulkan startup gate uses an exact product-residual reference because the
legacy MSVCRT `fmaf` returns incorrect residuals for some of the probe inputs;
the GPU comparison remains exact and also has fixed-bit regression vectors.

```powershell
$env:QT_QPA_PLATFORM='offscreen'
$env:QT_QUICK_BACKEND='software'
./build/qt/app/SpektraLabQt.exe --open C:/photos/supported.ARW `
  --reject C:/photos/unsupported-HE.NEF --self-test C:/SpektraLab-validation/new-qt-test
```

The output directory must not exist. The test drives actual QML/controller
operations and the native Vulkan engine, saves fit/100%/final PNG captures,
exports the displayed frame, checks negative-cache use, film/paper changes,
failed RAW retention, overwrite refusal with pending settings, single flight,
panning and physical-pixel scale, old-frame immutability, and injected display
publication failure recovery. It also checks automatic selection/brightness,
rapid edits before and during rendering, returning to the displayed settings,
all four slide films, negative/slide transitions, automatic decode changes and
reset. It exits nonzero on failure and writes JSON and scene captures.

`spk_qt_frame_output` is an additional CTest for shared crop geometry, actual
PNG/JPEG/TIFF pixels/depth/ICC, JPEG quality, resize, extension agreement,
atomic no-overwrite publication (including concurrent writers), and cleanup.
The explicit full-RAW library gate uses isolated copies of both photo inputs:

```powershell
# Use the same Qt/MinGW runtime PATH as build.ps1 and the SDK's plugin directory.
./build/qt/spk_qt_library_workflow_test.exe ./build/qt/app/resources `
  C:/photos/supported.ARW C:/photos/unsupported-HE.NEF `
  C:/SpektraLab-validation/new-library-test
```

It checks pending edits while switching photos, independent crops, synchronization,
preview/export pixel agreement, PNG16 and JPEG size, batch source/filename
agreement, selection locks, no-overwrite, cancellation and failed RAW retention.
The output directory must be new; missing fixtures are a hard failure.

`spk_qt_edit_store` adds strict document/version validation, atomic save failure,
source replacement (including before first save), concurrent-instance conflicts
and preservation of damaged/future records to CTest. `spk_qt_editing_workflow_test`
uses the same four arguments as the library gate and verifies history during
in-flight renders, geometry/PNG agreement, mirrored rotation direction,
destructor flushing of pending changes, recreation/reopening, per-photo history,
batch locks and visible persistence warnings. It owns copies of all inputs.
`spk_qt_layer2` pins the grade's JSON contract and its arithmetic anchor by
anchor against `adjustments.ts`; `spk_qt_histogram` pins the bin counting;
`spk_qt_wb_math` pins the Robertson temperature/tint round trip and the as-shot
solve within measured bounds. The library and editing workflow gates carry
grade, compare, histogram, latitude and white-balance blocks that are
compile-verified but not yet executed — they run when RAW fixtures are
supplied.

The captures are **offscreen scene captures**, not a live monitor screenshot.
Software scene display does not make the engine a CPU renderer. This gate does
not automate the native file dialogs or verify monitor profiles. At this phase
the host renders encoded sRGB and the GUI shows an 8-bit SDR texture. A QImage
sRGB tag is not a proof that the default Windows Qt swapchain applies a monitor
ICC transform; display colour management is not wired and the monitor ICC path
remains unverified. Wide-gamut, EDR/HDR, ColourSync-equivalent display
validation and perspective geometry are future work. Vibrance and EDR grading
are not included, and the interface is Chinese-only; four-language i18n is not
included. TIFF and PNG16 retain 16-bit
pixels; PNG8/JPEG8 use 8-bit samples, and resizing is explicitly requested.

Nikon HE/HE* RAW remains explicitly unsupported by this LibRaw build. A failed
NEF open must leave the previous ARW and its session usable.

## Phase 2 engine features in the UI

Phase 2 (`phase2-kernels` branch) ported six formerly Metal-only engine
features to the Windows/Vulkan backend and wired them into this frontend; the
engine no longer refuses any of them (`capabilities` reports
`unsupported_features = []`):

- **Cineon digital intermediate (DI 数字中间片)** -- `io.digital_intermediate`
  renders the session's negative as Cineon through `spk_di_encode`; slide
  films are still refused by the engine.
- **Scene latitude mapping with fit commit (宽容度映射 + 拟合提交)** --
  `spk_scene_latitude`/`spk_scene_curve` apply the mapping in real renders;
  the controller commits fitted values through the C ABI.
- **Contrast mask (对比遮罩)** -- RFC-024 fields, `spk_mask_reduce`/
  `spk_mask_epilogue` and `spk_contrast_mask_field`.
- **Scan HDR expand (HDR 扫描)** -- `spk_scan_hdr_expand` on the scan path.
- **Film edge / overscan (胶片边缘)** -- canvas, gate, holes, carrier and
  Fujifilm/Kodak edge print via four overscan kernels, an EdgeRaster text
  rasteriser and a threaded Gaussian.
- **Date imprint (日期背印)** -- three faces (date, text, shooting data) with
  style/corner/size/exposure/inset controls in the edit store.

Acceptance on the port: CTest 28/28 (21 before phase 2), the engine's
`overscan_checks.py` Python gate at 145 assertions / 0 failures (the
whole-feature acceptance for film edge and the date/data faces, including
half-frame pairs), `windows_features.py` success, offscreen Qt smoke clean.

Honest boundaries, unchanged from the engine docs: there is no Mac in this
port, so there are no Metal whole-image reference dumps for the new kernels --
parity rests on the float64 kernel gates and the Python overscan acceptance.
The Qt workflow gates that would walk these features against real RAW inputs
are compile-verified but stay unexecuted until RAW fixtures exist.
`text_b` (date imprint pairing) is future work, and striped execution still
refuses overscan layouts by name.

Font deployment: the edge print and date faces rasterise **TeX Gyre Heros**
(GUST font license, see `license/GUST-FONT-LICENSE.txt` and
`windows_UI/THIRD_PARTY.md`) from `resources/fonts`. The `app/` deployment
copies that directory; a hand-rolled deployment must carry `resources/fonts`
-- the engine refuses to start when a named face file is missing, by design,
rather than silently dropping the edge print.

## Phase 3 half-frame pairs (半幅对)

Two photographs can be composed into one 37 x 24 mm film piece -- two 18 x
24 mm half-frame gates separated by an unexposed gap -- that develops as a
single engine session. 建对 in the filmstrip tool row takes two selected
photographs (the first two in library order; a pair is refused as a hole) and
creates a pair as its own library item. The pair's strip thumbnail and canvas
are the composed piece preview; the source photographs stay untouched in the
library.

Piece-level controls live in the 半幅对 panel: the gap spacing (0.5-2.0 mm;
locked to the strip's 1.0 mm while the film edge is on, since the strip
advance is fixed) and the turned toggle (side-by-side or stacked). Each hole
has its own exposure EV (±5) with the metered reading next to it, its own
date imprint switch and text, and its print-trim values shown read-only.
Metering is per hole: a short single-hole session at most 1024 px solves the
exposure and the host multiplies the solved gain into the composed pixels
with the engine's automatic exposure off. The metering key excludes
placement, so moving a photograph inside its hole never re-meters.

Composition assembles the two decoded RAWs into one linear ProPhoto float32
buffer in which the gap is zero -- literally unexposed film -- and develops
it through the ordinary single-session pipeline. The frame cut lands on the
gap midpoint, so exporting the piece yields the two half frames; the
latitude-split and second-frame (b_*) fields ride the same settings and the
b_* fields belong to the second frame only. Pairs persist in an independent
`.spektrapair` record (version/sources/holes/look semantics) keyed by the
ordered union hash of both sources, with the same atomic-save, locking and
conflict discipline as photo records; `scene_latitude_split` and the b_*
fields ride the settings record as an optional carried group.

On a pair item the shared panels guard what does not apply: input/camera
exposure, white balance, metering, crop, format, film-edge format (a pair
forces 135_half) and RAW decode are disabled with a note pointing at the
piece controls; latitude analysis and fit-commit are refused (the applied
mapping still rides the whole piece); the date-imprint switch and per-frame
date text move to the per-hole sections while style, corner and size remain
live.

Honest boundaries. There are still no RAW fixtures on this machine, so the
pair render flows are verified by code review plus synthetic evidence: the
composer/metering unit gates (990 + 29 checks, the pair-host count including
a synthetic session-loss chain -- a failed settings operation drops the pair
session and a retry on the emptied decode refuses until the piece is handed
over again), the edit-store pair record
gate (506 checks), a synthetic pair session walked through the C API against
the engine schema, and the engine's `overscan_checks.py` pair cases (145
PASS / 0 FAIL on this machine). Per-hole print trim (brightness, magenta,
yellow, preflash) persists in the pair record but does not change pixels yet
-- the §12 print-layer cut is future work, so the panel shows the values
read-only instead of live controls. Per-hole Scene Placement (Fit) is
deferred until the latitude request carries a region argument; the engine
already pins region Fits in `overscan_checks.py` but the controller surface
is not plumbed. A dated pair without the film edge is capped by the engine's
37:24 canvas shape gate at roughly 2.85 mm of spacing: the panel warns when
a loaded record carries a wider gap (the slider itself caps at 2.0 mm), and
records stay loadable. A pair whose source file has disappeared is refused
on single export (源照片缺失，无法导出半幅对, PAIR-WIRING §6.4) instead of
being exported from the shown piece alone; the batch path's pair refusal
stands.

## Phase 4 the library page (图库)

The library page ports the Mac app's 图库 on top of spklib -- the same
library SDK the Mac reads: the Windows helper build is upstream spklib
0.4.0 (abi 1, schema 3; the Mac ships 0.3.1 against the same schema 3), so
this helper opens the same SQLite `.spkcatalog` libraries the Mac app
reads, and the wire it speaks is the SDK protocol documented in
`API-SPEC-library-helper.md`. The data layer (`windows_UI/src/`) is four
pieces over that wire:

- **CatalogSourcesStore** -- the kept-sources registry, the Mac
  `sources.json` key-for-key (uppercase UUIDs, second-precision ISO-8601
  UTC, pretty+sorted+atomic writes; an unreadable file is quarantined once
  per content as `sources.unreadable-<time>.json` instead of being silently
  skipped). A source is a catalog or a folder of photographs; the selected
  source falls through exactly like the Mac's `??` chain.
- **LibraryHost** -- one helper process, 32 typed methods, single-flight;
  the wire payloads are built by pure protocol builders (snake_case field
  names verbatim, Omit/Null/Number tri-state), replies come back as
  `Reply<T>` that separates a helper error from a transport failure.
- **LibraryModel** -- the Mac `MacLibraryData` surface: sources +
  createCatalog/selectSource/addFolder/importFiles, the collection tree
  (the fixed rows, the user albums, and a per-folder collection with live
  counts), ratings and color tags written in place with facet refresh,
  kind/raw/raster/pair/movie filters, a text filter (300 ms debounce),
  page-wise loading (1000 rows per `loadMore`), and the develop join that
  reads the app's own per-photo EditStore records for the develop badge and
  the 显影 film/paper names.
- **ThumbnailStore** -- path-keyed two-tier cache (in-memory LRU bounded by
  the Mac's 64 MB budget; one PNG per (path, maxPixel) on disk under the
  MacLibrary `Cache/Thumbnails` layout), a truncated write is deleted and
  decoded again.

The 图库 mode UI: a mode row (图库/冲洗/编辑) heads the left column;
冲洗/编辑 keep the existing panels and the filmstrip, 图库 swaps in the
library sidebar and the grid. The sidebar's three tabs are 库 (source
picker + collection tree), 过滤 (search, and the rating / color-label /
kind facets with cross counts) and 信息 (the selected row's details); the
bottom status line carries busy, the last error and the live import
progress. The grid is a lazy `GridView` (112 px cells, 64-256 px in 1.25
steps, zoom is layout only -- a cell decodes at the store's 256 px cap
once and rescales), each cell showing the tile, the elided file name and
the star / color / develop badges. Keyboard: 0-5 rates the whole selection,
`-` `*` `+` set the red/yellow/green labels, arrows move the selection,
Return opens the photo in develop, Ctrl+A selects all, Ctrl+=/− zooms.
Ctrl+C opens the copy sheet over **SettingsClipboard**, the Mac clipboard
port: eight offered groups (couplers and preflash have no fields in this
host, so they are not offered -- the Mac's own rule that a checkbox you
cannot see is a lying checkbox), and a paste writes the checked groups'
fields only, never placement, crop, decode mode or the layer-2 switch, and
keeps the target's own As Shot white balance.

Import and decoding run on a one-thread worker pool (the worker seam): the
import walks the existing index, dedupes, chunks `images.add` calls and
creates one album per import, posting a progress frame per chunk that QML
repaints mid-import; while an import runs, every other public wire call is
refused (the pipe is single-conversation) and clicks and keys are not
delivered. Thumbnail decodes go through the same pool, so a scroll never
blocks the UI thread.

Acceptance on the port: CTest 35/35 (30 before phase 4; new
`spk_qt_sources_store`, `spk_qt_library_host`, `spk_qt_library_model`,
`spk_qt_thumbnail_store`, `spk_qt_settings_clip`; check counts at the
closeout: sources_store 147, library_host 172, library_model 342,
thumbnail_store 68, settings_clip 41 -- the helper-gated suites run
against the real spklib-host binary). The engine was untouched by this
phase (all 21 changed files are under `windows_UI/`), and the closeout
reran the phase 2/3 gates unchanged: `overscan_checks.py` 145 PASS / 0
FAIL, byte-identical to the phase 3 log, the 45 built `.spv` files sha1-
equal to the ASCII parity copy, `windows_features.py` success, and the
offscreen smokes (冲洗 default, 图库 with and without the helper beside
the app) all at zero stderr bytes.

Honest boundaries. RAW thumbnails are a deterministic placeholder tile, by
decision: the only RAW decode paths in reach do a full develop
(`spk_image_io`'s raw entry points), exactly what a thumbnail must never
do, so real RAW tiles arrive with real RAW usage through a future
decode path. Library paste writes **records**: a photograph currently open
in the editor does not see the paste until the controller re-saves it, and
the unified browser (one model for open photographs) is recorded future
work -- the filmstrip stays shared between 冲洗/编辑 for the same reason,
and the right-hand parameter column still shows in 图库 mode. The
helper's `capture_one.*` wire methods (probe/link/copy/update) are present
in the SDK but untyped in LibraryHost and unsmoked -- there is no Capture
One on this machine. Import progress has no cancel (the Mac's stopAsked is
not ported), the 图库 page's import / new-folder buttons and the sidebar
"+" menu are future work, and the date/camera/lens facet rows are cut (the
helper's facets carry them; the UI ships the rating/color/kind contract).
The clipboard group memory is in-process, not persisted (the persistent
layer belongs to the settings page). The helper is a local checkout build
of upstream spklib main (`f4aced8`, version 0.4.0) carrying five Windows
portability deviations recorded in that checkout's `portability-notes.md`
-- entries 4-5 landed during this phase (sync_tree's mixed-separator path
keys; absolute locations losing the drive letter, a location cache that
never refetched, and a rebuilt cache whose keys were not normalized) --
and must ride along any spklib update; a second machine needs the same
host build.
