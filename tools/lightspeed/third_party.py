"""Third party notices of the Windows package.

Run in the MSYS2 UCRT64 shell after `cmake --install`:

    python tools/lightspeed/third_party.py <install dir>

Every DLL of <install dir>/bin is traced to its MSYS2 package (pacman), the
license texts of those packages are copied to <install dir>/share/licenses,
and THIRD_PARTY_NOTICES.md lists the packages, their versions and licenses,
with the Python runtime and the AI models of the AI tools.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else '/c/lightspeed/install'
OUT = os.path.join(ROOT, 'THIRD_PARTY_NOTICES.md')
PREFIX = 'mingw-w64-ucrt-x86_64-'
# the Python of MSYS2 is a Windows program: the Windows path of the license texts
LICENSES = subprocess.run(['cygpath', '-m', '/ucrt64/share/licenses'], capture_output=True,
                          text=True).stdout.strip() or '/ucrt64/share/licenses'
# built next to darktable, not from MSYS2
EXTRA = {'onnxruntime.dll': 'ONNX Runtime (MIT, https://onnxruntime.ai)',
         'onnxruntime_providers_shared.dll': None}


def sh(args):
    return subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace').stdout


def owners(files):
    """MSYS2 package of every file, one pacman call"""
    out = {}
    text = sh(['pacman', '-Qo'] + ['/ucrt64/bin/' + f for f in files])
    for line in text.splitlines():
        m = re.match(r'^/ucrt64/bin/(\S+) is owned by (\S+) ', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def infos(pkgs):
    """name -> fields of `pacman -Qi` for all packages, one call"""
    out, cur = {}, {}
    for line in sh(['pacman', '-Qi'] + list(pkgs)).splitlines() + ['']:
        m = re.match(r'^(\w[\w ]*?)\s*: (.*)$', line)
        if m:
            cur[m.group(1).strip()] = m.group(2).strip()
        elif not line.strip() and cur:
            out[cur.get('Name', '')] = cur
            cur = {}
    return out


def main():
    bindir = os.path.join(ROOT, 'bin')
    ours = {'libdarktable.dll'}
    packages, unknown = {}, []
    files = [f for f in sorted(os.listdir(bindir)) if f.lower().endswith(('.dll', '.exe')) and f not in ours]
    own = owners(files)
    for f in files:
        if f in own:
            packages.setdefault(own[f], []).append(f)
        elif f.lower().endswith('.dll'):
            unknown.append(f)
    all_info = infos(packages)

    licdir = os.path.join(ROOT, 'share', 'licenses')
    os.makedirs(licdir, exist_ok=True)
    rows = []
    for pkg in sorted(packages):
        i = all_info.get(pkg, {})
        name = pkg[len(PREFIX):] if pkg.startswith(PREFIX) else pkg
        lic = i.get('Licenses', '').replace('spdx:', '')
        rows.append((name, i.get('Version', ''), lic, i.get('URL', '')))
        src = os.path.join(LICENSES, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(licdir, name), dirs_exist_ok=True)

    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Third party software in Tonelark for Windows\n\n'
                'Tonelark is free software under the GNU General Public License v3 (LICENSE). '
                'It is a fork of darktable (https://www.darktable.org, GPL v3) and contains the '
                'software below, each under its own license. The license texts are in '
                '`share/licenses`, and the source code of every package is available from its '
                'project page and from MSYS2 (https://packages.msys2.org).\n\n'
                'Tonelark is not affiliated with, endorsed by or sponsored by Adobe. Adobe and '
                'Lightroom are trademarks of Adobe Inc., used here only to describe compatibility.\n\n'
                '## Libraries (MSYS2 UCRT64 packages)\n\n'
                '| Package | Version | License | Project |\n|---|---|---|---|\n')
        for name, ver, lic, url in rows:
            f.write('| %s | %s | %s | %s |\n' % (name, ver, lic, url))
        extra = [EXTRA[u] for u in unknown if EXTRA.get(u)] + [u for u in unknown if u not in EXTRA]
        if extra:
            f.write('\nAlso: %s.\n' % ', '.join(extra))
        f.write('''
## AI tools (share/darktable/lightspeed/ai)

| Component | License | Project |
|---|---|---|
| Python 3.12 (embeddable) | PSF License | https://www.python.org |
| NumPy | BSD-3-Clause (bundled OpenBLAS: BSD-3-Clause; GCC runtime: GPL-3.0 with GCC Runtime Library Exception) | https://numpy.org |
| OpenCV (opencv-python-headless) | Apache-2.0 | https://opencv.org |
| YuNet face detection model | MIT | https://github.com/opencv/opencv_zoo |
| SFace face recognition model | Apache-2.0 | https://github.com/opencv/opencv_zoo |
| Facial expression recognition model (MobileFaceNet) | Apache-2.0 | https://github.com/opencv/opencv_zoo |

The license texts of the Python packages are in their `*.dist-info` folders, the ones of
the models in `models/`.

AI models that darktable downloads on request (for example the object masks and the
denoise and upscale models) come with their own licenses, shown when they are downloaded.

The AI assistant tools send reduced copies of photos to the AI command line tool chosen by
the user (Claude Code, Codex or Gemini CLI), under the user's own account and its terms.
''')
    print('%d packages, %d unknown DLLs -> %s' % (len(rows), len(unknown), OUT))


if __name__ == '__main__':
    main()
