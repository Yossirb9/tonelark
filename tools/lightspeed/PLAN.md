# Tonelark — darktable fork with a Lightroom-style UI

Base: darktable release-5.6.1 (C:\lightspeed\darktable). Engine untouched; changes are UI/UX, import, defaults, branding.
Build: MSYS2 UCRT64 (C:\msys64), BINARY_PACKAGE_BUILD=ON (portable across CPUs), OpenMP + OpenCL (runtime-loaded).

## Work items
1. [x] Build toolchain + deps (pacman), first clean build of upstream
2. [x] Branding: "Tonelark" name in title/about/splash/installer; separate config dir (%LOCALAPPDATA%\lightspeed)
3. [x] Lightroom-like theme (lightspeed.css) as default
4. [x] View names: Library / Develop / Map / Slideshow / Print / Tethering
5. [x] Develop: "Basic" panel (proxy lib: Temp, Tint, Exposure, Contrast, Highlights, Shadows, Whites, Blacks,
       Texture, Clarity, Dehaze, Vibrance, Saturation) mapped onto darktable modules
6. [x] Develop: module group preset "Tonelark" ordered like LR panels (Tone Curve, HSL, Color Grading, Detail,
       Lens Corrections, Transform, Effects, Calibration) + extra groups for all darktable modules
7. [x] Lightroom keyboard shortcuts (shortcutsrc.lightroom applied on first run)
8. [x] Before/after "\" toggle
9. [x] Lightroom catalog (.lrcat) import: folders, images, virtual copies, ratings, picks/rejects, color labels,
       keywords, collections -> tags, title/caption/copyright, GPS, develop settings -> darktable history
10. [x] Defaults: OpenCL on, legacy chromatic adaptation (Temp/Tint), sensible perf settings, no welcome quiz spam
11. [x] Installer (Inno Setup, English/Hebrew, packaging/windows/make_installer.py) + portable zip -> C:\lightspeed\dist
12. [x] QA: CLI exports (JPEG/TIFF/PNG), GUI screenshots, lrcat import test, fixes

## Mapping (Basic panel & LR import share the same mapping code)
- Temp/Tint -> temperature module widgets (legacy CAT)
- Exposure -> exposure.exposure (relative to auto-applied default)
- Contrast, Vibrance, Saturation(chroma), Whites(highlights_Y), Blacks(global_Y) -> colorbalancergb
- Highlights/Shadows -> shadhi
- Clarity -> bilat.detail ; Texture -> sharpen ; Dehaze -> hazeremoval.strength

## Status 2026-09-27: v1.0 complete
- All items done and QA'd: exports in 12 formats (CLI + GUI) with EXIF, LR catalog import (dialog, relocation, double click
  on .lrcat), before/after (\ and Y), installer install/run/uninstall on a fresh config, OpenCL on Iris Xe.
- User guide (Hebrew): data/lightspeed/guide.html, installed to share/doc/lightspeed.
