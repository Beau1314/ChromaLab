# Windows portability notes (local checkout)

This checkout of SpektraLab_library `main` (spklib 0.4.0) is upstream plus the
minimal shims below, added under the project's MIT licence so the SDK and its
host build on Windows with MinGW g++ 13.1.0. The upstream `build.sh` (macOS /
clang++) is untouched; `build-windows.ps1` was added beside it. This file is
NOT part of upstream and the checkout is not a git repository — upstream
changes stay limited to the three source files listed here.

## Deviations from upstream

1. `src/library.cpp` — include block + rename:
   - The Windows shim block (`_POSIX_THREAD_SAFE_FUNCTIONS` so MinGW's
     `time.h` declares `gmtime_r`/`localtime_r`, and `#define timegm _mkgmtime`)
     sits **above** `#include "library.hpp"` because library.hpp transitively
     includes `<ctime>` and MinGW's POSIX declarations only take effect when
     the feature macro is set before `time.h` is first included.
   - `#include <sys/file.h>` is guarded out on `_WIN32` (MinGW lacks the
     header; nothing in spklib calls `flock` — the lock the comment at
     `Library::create` mentions is SQLite's own file lock).
   - In `move_image`, the non-Apple rename branch gained a `_WIN32` case:
     `::_wrename(fs::path(from).c_str(), target.c_str())`. On Windows
     `fs::path::c_str()` is `wchar_t*`, so plain `::rename` does not compile;
     `_wrename` keeps Unicode names and maps errors to `errno` the same way
     (`EXDEV` → `cross_volume`, `EEXIST` → `conflict`).
2. `src/capture_one.cpp` — `fingerprint()`: `st.st_mtimespec` (macOS
   nanosecond mtime) is guarded; on Windows the fingerprint falls back to
   `st.st_mtime` in whole seconds (`mtime_ns` = seconds × 1e9). Consequence:
   two writes to a catalog within the same second that leave the size and
   SQLite change-counter unchanged are indistinguishable on Windows; the
   SQLite header change-counter (`change_counter`) still catches ordinary
   edits.
3. `build-windows.ps1` — new file. MinGW g++/gcc/ar direct calls, same flag
   set as `build.sh`'s `CXXFLAGS` (`-std=c++20 -O2 -Wall -Wextra
   -Wno-unused-parameter`, `-DNDEBUG` instead of `-g`), SQLite amalgamation
   compiled in with `-DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE
   -DSQLITE_THREADSAFE=1`. Notes:
   - `sqlite3.c` must go through `gcc`, not `g++` (it is C; identifiers like
     `new` are C++ keywords).
   - `-DSPKLIB_VERSION=\"0.4.0\"` needs literal backslash-escaped quotes on
     the PowerShell command line.
   - `-lws2_32` is linked (harmless; keeps winsock symbols resolvable).
   - Upstream `tests/tests.cpp` is not Windows-clean (`sys/stat.h` is fine,
     but the fixtures walk POSIX paths); the test binary builds only with
     `-Test` and is not part of the supported Windows surface.
4. `src/library.cpp` — `sync_tree()`'s two path keys (2026-11-16, phase 4
   task 4.3): `by_path` is keyed through `join_path`, which writes forward
   slashes, while the directory walk produces `fs::directory_entry::path()`
   in the platform's own separators — on Windows one photograph was spelled
   `C:\dir\name` by the walk and `C:\dir/name` by `by_path`, the lookup
   missed, and every sync re-classified unchanged files as fresh (a restored
   file became a duplicate row instead of its own row coming back; a move
   only worked because `add_images`' own lookup papered over the gap). Both
   sides now go through one canonical spelling:
   `nfc(fs::path(join_path(...)).lexically_normal().string())` for the map
   key and `nfc(fs::path(f).lexically_normal().string())` for the walk side.
   `lexically_normal` collapses mixed separators to the platform's preferred
   form, so POSIX behaviour is unchanged (upstream's macOS build normalises
   to forward slashes on both sides already). Found by the app's
   library_model sync test (move-keeps-rating / gone-hides / restored).
5. `src/library.cpp` — absolute locations' drive letter and cache shape
   (2026-11-16, phase 4 task 4.4): two Windows-only cuts in the location
   table, both hit by the Windows model's two-process import (the import
   speaks to the catalog through a second, worker-owned spklib-host while
   the UI's own process keeps browsing; SQLite serialises them). (a)
   `ensure_location` stored an absolute location's relative half as
   `dir.substr(1)` — the POSIX cut past the leading slash — which on Windows
   chopped the DRIVE LETTER off (`C:\photos` became root `/` + rel
   `:\photos`), so any process other than the writer reconstructed an
   unresolvable path and `check_files`' `exists` read false for every
   imported row. Windows now stores root `C:` + the remainder, joining back
   to a statable path. (b) `resolve_location` built its location cache once
   and never refetched, so a location another process had added resolved to
   the empty string forever; the lookup now rebuilds once on a miss.
   (c) `build_location_cache`'s reconstructed dirs go through
   `fs::path(...).lexically_normal()` on Windows, so a rebuilt cache's
   `by_dir` keys match insert-time dirs' native shape — without it the
   folder walk (`folders.add`/`folders.sync`) re-added every already-known
   file as a fresh row (the app's library_model test caught the duplicate
   rows as `total == 60` after a source round trip). POSIX behaviour is
   unchanged: the substr(1) cut stays, and macOS builds already normalise
   to forward slashes. Found by the app's library_model workflow test
   (check_files' exists; the source switch's row count).

## Not shimmed (known limitations, documented for the app side)

- NFC normalisation of paths stays macOS-only (`nfc()` is `#ifdef __APPLE__`;
  upstream already returns input unchanged elsewhere) — identity off Apple
  platforms is upstream's documented behaviour.
- Narrow-string path handling: the API speaks UTF-8 `std::string` paths; on
  Windows these are converted through `fs::path` with the ANSI code page at
  the file boundary. The app should prefer ASCII-safe temp paths for smoke
  use; full wide-char plumbing is out of scope for the port shell.
- `renamex_np`'s `RENAME_EXCL` (never clobber) has no direct Windows
  equivalent used here; `_wrename` clobbers on Windows, the pre-check above
  the rename keeps the same `conflict` behaviour for the checked race.

## Build provenance (this machine)

- compiler: MinGW-Builds g++ 13.1.0, `D:\spk-deps\qt-6.11.3\Tools\mingw1310_64\bin`
- SQLite: 3.54.1 amalgamation (public domain), `D:\spk-deps\sqlite`
- warnings (rel: `-Wall -Wextra`): 16 × `-Wimplicit-fallthrough` in
  `sqlite3.c` (upstream SQLite, known), 2 × `-Wmaybe-uninitialized` in
  `std::variant` internals reached from `library.cpp` (libstdc++ false
  positive on `Val` binds), 1 × `-std=c++20` not valid for C (sqlite3.c via
  `gcc`, benign).

## Capture One reader (2026-11-20, synthetic-catalog drill)

A synthetic `.cocatalog` (per the public reverse-engineering notes: Core
Data-style tables, schema 1621) probes `supported: true` and links end to
end — collections, album membership, ratings, colour tags, variants and
referenced paths all sync into the mirror. Two Windows-port findings:

- **Absolute locations resolve through `ZMACROOT` only.** `ZWINROOT` is
  selected but contributes nothing, so a real Windows catalog (which stores
  absolute roots in ZWINROOT with a NULL ZMACROOT) would come back with every
  original offline (`/filename`, `exists: false`). First thing to check with a
  real sample; the fix belongs in spklib (fall back to ZWINROOT).
- `captured_local` stays null: no capture-time column name appears in the
  host binary — 0.4.0 appears not to read it.

Repro: `tools/c1-sample/` (generator + line-protocol probe).
