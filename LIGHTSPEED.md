# Lightspeed

A fork of [darktable](https://github.com/darktable-org/darktable) 5.6.1 with a Lightroom Classic
style interface and Lightroom catalog import. darktable's processing engine is unchanged.

## What is different from darktable

- Lightroom-like UI: Library / Develop names, panel order, a Basic panel (Temp, Tint, Tone,
  Presence with Auto), bottom panel buttons (Import..., Export..., Copy..., Paste, Sync Settings,
  Previous, Reset), pick flags, before/after (`\` and `Y`), Lightroom keyboard shortcuts,
  its own theme and logo.
- Lightroom catalog import (`.lrcat`): folders, ratings, flags, color labels, keywords,
  collections, virtual copies, stacks, metadata and develop settings (Basic, curves, HSL,
  color grading, detail, lens, vignette, grain, crop, spot removal). Double click on a
  catalog, or Library > Import > "Lightroom catalog...".
- `darktable-cli` develops images with a Lightroom XMP, and writes a darktable XMP when the
  output ends with `.xmp`.
- Fixes: Exif kept in exports with a fresh configuration, guided filter on the CPU (garbage at
  the bottom of GPU exports with haze removal on Intel Xe), Lightroom color grading hues.

The user guide (Hebrew) is `data/lightspeed/guide.html`, installed to `share/doc/lightspeed`.

## Build on Windows

MSYS2 UCRT64 with the dependencies listed in `packaging/windows/README.md`, then:

```
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBINARY_PACKAGE_BUILD=ON -DUSE_AI=ON \
      -DCMAKE_INSTALL_PREFIX=C:/lightspeed/install -S . -B ../build
cmake --build ../build && cmake --install ../build
python packaging/windows/make_installer.py      # Inno Setup 6: installer + portable zip
```

`tools/lightspeed` holds the scripts used during development: incremental build
(`build.ps1`), window capture for GUI tests, the Lightroom test catalog generator
(`qa/make_lrcat.py`) and the edit/mask tests (`qa/golden`). They use absolute paths under
`C:\lightspeed`.

## License

GPL v3 or later, like darktable.
