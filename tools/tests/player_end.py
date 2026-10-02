#!/usr/bin/env python3
"""Real playback regression: natural completion, frozen clock, replay, seek and queue.
Build with make -f desktop.mk -f tools/tests/player_end.mk player-end-tests.
Needs ffmpeg and a desktop SDL dummy driver; uses only generated local media.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
folder = Path(tempfile.mkdtemp(prefix='coffeeflix-player-end-'))
print('Artifacts:', folder, flush=True)
subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=24',
                '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '2',
                '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-g', '12', '-c:a', 'aac',
                '-movflags', '+faststart', str(folder / 'av.mp4')], check=True)
for name, options in [('silent.mp4', ['-an', '-c:v', 'copy']), ('audio.m4a', ['-vn', '-c:a', 'copy'])]:
    subprocess.run(['ffmpeg', '-v', 'error', '-i', str(folder / 'av.mp4'), *options,
                    str(folder / name)], check=True)
subprocess.run([str(root / 'build-desktop/player-end-test'), str(folder)], cwd=root,
               env=dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'),
               check=True, timeout=80)
