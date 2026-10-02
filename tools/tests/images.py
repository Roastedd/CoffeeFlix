#!/usr/bin/env python3
"""The picture size check against files made by real encoders (Pillow, cwebp): the size their headers
state is read as it is, and pictures over 16 million pixels are refused before they are decoded.
Build with make -f desktop.mk -f tools/tests/images.mk images-tests. Needs Pillow; cwebp is used when
there is one. The checks that need no files run with build-desktop/images-test alone."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

try:
    from PIL import Image, ImageDraw
except ImportError:
    print('SKIP images: needs Pillow')
    sys.exit(0)

root = Path(__file__).resolve().parents[2]
folder = Path(tempfile.mkdtemp(prefix='coffeeflix-images-'))
print('Artifacts:', folder, flush=True)
sizes = []


def note(name, w, h, big=False):
    sizes.append(f'{name} {w} {h} {"big" if big else "ok"}')


def picture(w, h, mode='RGB'):
    im = Image.new(mode, (w, h), 200 if mode in ('L', '1') else (200, 100, 50) + ((255,) if mode == 'RGBA' else ()))
    ImageDraw.Draw(im).rectangle((w // 4, h // 4, w // 2, h // 2), fill=20 if mode in ('L', '1') else (20, 30, 40) + ((128,) if mode == 'RGBA' else ()))
    return im


for w, h in [(1, 1), (300, 200), (1920, 1080), (4096, 4096)]:
    for mode in ('RGB', 'RGBA', 'L'):
        picture(w, h, mode).save(folder / f'{mode}-{w}x{h}.png')
        note(f'{mode}-{w}x{h}.png', w, h)
for w, h in [(1, 1), (640, 360), (3840, 2160), (4096, 4096), (17, 4001)]:
    picture(w, h).save(folder / f'baseline-{w}x{h}.jpg', quality=85)
    note(f'baseline-{w}x{h}.jpg', w, h)
picture(800, 600).save(folder / 'progressive.jpg', progressive=True, quality=80)
note('progressive.jpg', 800, 600)
picture(800, 600).save(folder / 'with-tables.jpg', quality=90, optimize=True, icc_profile=b'\0' * 3000, dpi=(300, 300))
note('with-tables.jpg', 800, 600)
exif = Image.Exif()
exif[0x010f] = 'Camera maker'
exif[0x0110] = 'Model'
picture(1024, 768).save(folder / 'with-exif.jpg', exif=exif.tobytes())
note('with-exif.jpg', 1024, 768)
picture(100, 80, 'RGB').convert('P').save(folder / 'still.gif')
note('still.gif', 100, 80)
frames = [picture(64, 48).point(lambda v, i=i: (v + i * 20) % 256) for i in range(4)]
frames[0].save(folder / 'animated.gif', save_all=True, append_images=frames[1:], duration=100, loop=0)
note('animated.gif', 64, 48)
picture(1000, 1000).save(folder / 'lossy.webp', quality=80)
note('lossy.webp', 1000, 1000)
picture(700, 500, 'RGBA').save(folder / 'lossless-alpha.webp', lossless=True)
note('lossless-alpha.webp', 700, 500)
picture(700, 500).save(folder / 'lossless.webp', lossless=True)
note('lossless.webp', 700, 500)
picture(1, 1).save(folder / 'lossless-1x1.webp', lossless=True)
note('lossless-1x1.webp', 1, 1)
picture(320, 200).save(folder / 'extended.webp', quality=80, exif=exif.tobytes(), icc_profile=b'\0' * 200)
note('extended.webp', 320, 200)
picture(4096, 4096).save(folder / 'lossless-4096.webp', lossless=True)
note('lossless-4096.webp', 4096, 4096)
if shutil.which('cwebp'):
    picture(640, 360).save(folder / 'from-cwebp.png')
    subprocess.run(['cwebp', '-quiet', '-q', '70', str(folder / 'from-cwebp.png'), '-o', str(folder / 'cwebp-lossy.webp')], check=True)
    subprocess.run(['cwebp', '-quiet', '-lossless', str(folder / 'from-cwebp.png'), '-o', str(folder / 'cwebp-lossless.webp')], check=True)
    note('cwebp-lossy.webp', 640, 360)
    note('cwebp-lossless.webp', 640, 360)
    (folder / 'from-cwebp.png').unlink()

# Pictures that state more than 16 million pixels, with a file that is small (flat colour compresses to nothing).
for w, h in [(4097, 4096), (5000, 5000), (20000, 20000), (60000, 300)]:
    Image.new('1', (w, h), 1).save(folder / f'bomb-{w}x{h}.png', optimize=False, compress_level=9)
    note(f'bomb-{w}x{h}.png', w, h, True)
for w, h in [(6000, 6000), (8000, 8000), (4097, 4097)]:
    Image.new('L', (w, h), 128).save(folder / f'bomb-{w}x{h}.jpg', quality=50)
    note(f'bomb-{w}x{h}.jpg', w, h, True)
Image.new('P', (20000, 5000), 0).save(folder / 'bomb-20000x5000.gif')
note('bomb-20000x5000.gif', 20000, 5000, True)
Image.new('L', (8200, 8200), 128).save(folder / 'bomb-8200x8200.webp', lossless=True)
note('bomb-8200x8200.webp', 8200, 8200, True)
Image.new('RGB', (9000, 9000), (5, 5, 5)).save(folder / 'bomb-9000x9000-lossy.webp', quality=10)
note('bomb-9000x9000-lossy.webp', 9000, 9000, True)
print('biggest file', max(p.stat().st_size for p in folder.iterdir()), 'bytes', flush=True)

(folder / 'sizes.txt').write_text('\n'.join(sizes) + '\n')
subprocess.run([str(root / 'build-desktop/images-test'), str(folder)], cwd=root,
               env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'), check=True, timeout=180)
print('Picture size limit passed')
