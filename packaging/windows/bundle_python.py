"""Add the Python runtime of the Tonelark AI helper to an install tree.

    python packaging/windows/bundle_python.py [--install C:/lightspeed/install]

Downloads the embeddable Python from python.org and installs numpy and
OpenCV (headless) into it: <install>/share/darktable/lightspeed/ai/python.
Run it after `cmake --install` and before make_installer.py.
"""
import argparse
import io
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, '..', '..'))
PY_VERSION = '3.12.10'
PACKAGES = ['numpy', 'opencv-python-headless']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default=os.path.join(SRC, '..', 'install'))
    args = ap.parse_args()
    target = os.path.join(os.path.normpath(args.install), 'share', 'darktable', 'lightspeed', 'ai', 'python')

    if os.path.isdir(target):
        shutil.rmtree(target)
    os.makedirs(target)

    url = 'https://www.python.org/ftp/python/%s/python-%s-embed-amd64.zip' % (PY_VERSION, PY_VERSION)
    print('downloading', url)
    with urllib.request.urlopen(url, timeout=120) as r:
        zipfile.ZipFile(io.BytesIO(r.read())).extractall(target)

    # site-packages next to the runtime
    tag = ''.join(PY_VERSION.split('.')[:2])
    pth = os.path.join(target, 'python%s._pth' % tag)
    with open(pth, encoding='utf-8') as f:
        lines = [l.rstrip('\n') for l in f]
    lines = [('import site' if l.strip() == '#import site' else l) for l in lines]
    if 'Lib\\site-packages' not in lines:
        lines.insert(1, 'Lib\\site-packages')
    with open(pth, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    site = os.path.join(target, 'Lib', 'site-packages')
    print('installing', ' '.join(PACKAGES))
    subprocess.check_call([sys.executable, '-m', 'pip', 'install', '--quiet', '--no-warn-script-location',
                           '--target', site, '--platform', 'win_amd64', '--python-version', PY_VERSION,
                           '--implementation', 'cp', '--only-binary=:all:'] + PACKAGES)

    # not needed by the helper: video input/output and the test suites
    for dirpath, dirnames, files in os.walk(site):
        for f in files:
            if f.lower().startswith('opencv_videoio_ffmpeg'):
                os.remove(os.path.join(dirpath, f))
        for d in list(dirnames):
            if d == 'tests' and 'numpy' in dirpath:
                shutil.rmtree(os.path.join(dirpath, d))
                dirnames.remove(d)

    # check it works
    exe = os.path.join(target, 'python.exe')
    out = subprocess.check_output([exe, '-c', 'import cv2, numpy; print(cv2.__version__, numpy.__version__)'],
                                  text=True).strip()
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(target) for f in fs)
    print('bundled Python %s, OpenCV and numpy %s, %.0f MB' % (PY_VERSION, out, size / 1e6))


if __name__ == '__main__':
    main()
