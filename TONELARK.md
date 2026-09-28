# Tonelark

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
- AI tools (1.1), using the AI command line tools already installed and logged in on the computer
  (Claude Code, Codex, Gemini CLI: the user's subscription, no API key). The work is done by
  `data/lightspeed/ai/lsai.py`, run by a bundled Python with OpenCV
  (`packaging/windows/bundle_python.py`):
  - Library > AI culling: *Find Best Shots* (local, no AI service: bursts, sharpness, exposure,
    eyes open, smiles), *Rate with AI* (numbered contact sheets of 12 photos, one request per
    sheet, JSON answer: stars, picks, a note per photo, an optional "looking for" match), and
    *Best Take* (local: the best face of everyone in a group burst blended into a new photo).
  - Develop > AI edit: mark an area and describe the change, Codex's image model edits it and
    only the area is blended into the full resolution photo, as a new photo grouped with it.
  - Every panel has the choice of the tool and model, with Install / Connect buttons that open
    the login of a missing or logged out tool.
- AI assistant (1.2, Library and Develop, `src/libs/aiassist.c`): the AI answers with Lightroom
  settings (`data/lightspeed/ai/lsedit.py`) that are applied as normal history items
  (`dt_lsai_apply_edit()`, `dt_lightroom_update_develop()`: only the given settings change):
  *Auto Edit* with words (one photo: a second request refines the rendered result; several:
  contact sheets of 12), *Match Look* (the look of a reference, exposure from the camera
  settings as Lightroom's Match Total Exposures, white balance relative to each camera white
  balance, optional AI fine-tuning), *Suggest Crops* (crop module + rotate and perspective),
  *Keywords & Captions*.
- Chat bridge (MCP, 1.2): `data/lightspeed/ai/lsmcp.py` is an MCP server (stdio) for Claude
  Code, Codex and Gemini CLI; it talks to the running Tonelark through request files in
  `<config dir>/mcp` (`src/common/lightspeed_bridge.c`). *Connect Chat* registers it.
- White balance tint follows Adobe's definition (Planckian locus, positive = green light, the
  photo gets more magenta), so Lightroom tints import with the right sign.
- Fixes: Exif kept in exports with a fresh configuration, guided filter on the CPU (garbage at
  the bottom of GPU exports with haze removal on Intel Xe), Lightroom color grading hues.

The user guide (Hebrew) is `data/lightspeed/guide.html`, installed to `share/doc/lightspeed`.

## Build on Windows

MSYS2 UCRT64 with the dependencies listed in `packaging/windows/README.md`, then:

```
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBINARY_PACKAGE_BUILD=ON -DUSE_AI=ON \
      -DCMAKE_INSTALL_PREFIX=C:/lightspeed/install -S . -B ../build
cmake --build ../build && cmake --install ../build
python packaging/windows/bundle_python.py       # Python + OpenCV for the AI helper
python packaging/windows/make_installer.py      # Inno Setup 6: installer + portable zip
```

`tools/lightspeed` holds the scripts used during development: incremental build
(`build.ps1`), window capture for GUI tests, the Lightroom test catalog generator
(`qa/make_lrcat.py`) and the edit/mask tests (`qa/golden`). They use absolute paths under
`C:\lightspeed`.

## License

GPL v3 or later, like darktable.
