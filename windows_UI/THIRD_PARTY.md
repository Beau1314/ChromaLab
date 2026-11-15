# Windows Qt frontend dependencies

SpektraLab code remains GPL-3.0-or-later. The film/paper profiles retain the
upstream CC BY-SA 4.0 notices. This development build dynamically links the
open-source Qt 6.11.3 Windows MinGW SDK; it does not use a Qt commercial license.

The adjacent `Qt/` directory contains official Qt license texts. The exact SDK
source revisions are recorded in `Qt-SBOM/*.spdx.json`. The Qt base, declarative,
SVG and tools license texts were retrieved at those source revisions. The
shader-tools and translations SBOMs do not identify a commit, so their license
texts were retrieved from separately pinned commits on the Qt 6.11 branch;
`Qt/manifest.json` records every source URL and SHA256. SDK archive URLs and
verified hashes are in `Qt-packages.json`.

- Qt source: <https://code.qt.io/cgit/qt/> (also mirrored at <https://github.com/qt>)
- Qt licensing: <https://doc.qt.io/qt-6/licensing.html>
- Qt module and bundled third-party notices: `Qt-SBOM/` and `Qt/` in this folder
- LibRaw 0.22.2 source: <https://github.com/LibRaw/LibRaw/tree/0.22.2>
- LibRaw license choices and copyright: `LibRaw-LICENSE.LGPL`,
  `LibRaw-LICENSE.CDDL`, `LibRaw-COPYRIGHT`
- MinGW-w64 compiler/runtime source and licenses:
  `MinGW/` and <https://github.com/niXman/mingw-builds-binaries/releases/tag/13.1.0-rt_v11-rev1>

The SDK and compiler are local development dependencies outside the Git source
tree. Before distributing a release binary, retain these notices and provide
corresponding source and build/relink information for the selected open-source
license terms. The current output is a local development bundle, not a signed
installer or a release-compliance certification.

Bundled with the engine's resources (phase 2 film edge):

- TeX Gyre Heros Regular/Bold (`engine/resources/fonts/`), the film edge's
  substitute faces for the Apple system fonts the upstream code reads:
  <http://www.gust.org.pl/projects/e-foundry/tex-gyre/heros> - GUST Font
  License (LPPL 1.3c), `license/GUST-FONT-LICENSE.txt`
- stb_truetype.h v1.26 (`engine/third_party/stb/`, already vendored) draws the
  faces off Apple - MIT / public domain dual license, `engine/third_party/stb/LICENSE`
