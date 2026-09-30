"""Download the models of the AI helper that are too big for git, and check
them: python tools/lightspeed/fetch_models.py (before a build; the ones there
already are only checked)."""
import hashlib
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, '..', '..', 'data', 'lightspeed', 'ai', 'models')

FILES = [
    # ArcFace ResNet100, ONNX Model Zoo, Apache 2.0 (the People panel)
    ('arcfaceresnet100-8.onnx',
     'https://github.com/onnx/models/raw/main/validated/vision/body_analysis/arcface/model/arcfaceresnet100-8.onnx',
     'f3a6bc281e72f88862f5748b53be3d76b3b48f8f1ab1f4a537941bdc4e1b01da'),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    bad = 0
    for name, url, digest in FILES:
        path = os.path.normpath(os.path.join(MODELS, name))
        if not os.path.exists(path):
            print('downloading', name)
            tmp = path + '.part'
            urllib.request.urlretrieve(url, tmp)
            os.replace(tmp, path)
        if sha256(path) != digest:
            print('WRONG CHECKSUM:', path)
            bad += 1
        else:
            print('ok', name)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
