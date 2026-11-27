"""Enlarge one chunk of movie frames with SeedVR2 for the HD cache.

Run by FearEffectRe --build-seedvr2-cache, once per chunk:

    python seedvr2_chunk.py <chunk dir> <result file> <seedvr2 dir>

<chunk dir> holds the frames in order as 00000.ppm, 00001.ppm, ... (320x160
as decoded from the disc) and hashes.txt, one 16-digit hex picture hash per
line in the same order. <seedvr2 dir> holds `repo` (ComfyUI-SeedVR2_VideoUpscaler)
and `models`. The frames are packed into a lossless video so SeedVR2 treats
them as a sequence (steadier than frame by frame), enlarged 4x, and written
to <result file> one after the other as raw RGB, in the same order; the game
then encodes them as H.264 into hdcache/seedvr2.vpack.
Exit code 0 when every frame was written.
"""
import glob
import os
import subprocess
import sys

import cv2
import numpy as np


def main() -> int:
    chunk, result, seedvr = sys.argv[1], sys.argv[2], sys.argv[3]
    frames = sorted(glob.glob(os.path.join(chunk, '*.ppm')))
    with open(os.path.join(chunk, 'hashes.txt')) as f:
        hashes = [int(line, 16) for line in f.read().split()]
    if not frames or len(frames) != len(hashes):
        print('seedvr2: frames and hashes do not match', flush=True)
        return 1

    first = cv2.imread(frames[0])
    h, w = first.shape[:2]
    video = os.path.join(chunk, 'clip.avi')
    out = cv2.VideoWriter(video, cv2.VideoWriter_fourcc(*'FFV1'), 15, (w, h))
    for f in frames:
        out.write(cv2.imread(f))
    out.release()

    results = os.path.join(chunk, 'out')
    env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1')
    # the settings tried on the intro movie: they fit a 24 GB card
    cmd = [sys.executable, os.path.join(seedvr, 'repo', 'inference_cli.py'), video,
           '--output', results, '--output_format', 'png',
           '--model_dir', os.path.join(seedvr, 'models', 'SEEDVR2'),
           '--dit_model', 'seedvr2_ema_3b_fp16.safetensors',
           '--resolution', str(h * 4), '--batch_size', '21', '--uniform_batch_size',
           '--temporal_overlap', '4', '--color_correction', 'lab',
           '--vae_encode_tiled', '--vae_decode_tiled', '--vae_encode_tile_size', '512',
           '--vae_decode_tile_size', '512', '--dit_offload_device', 'cpu', '--vae_offload_device', 'cpu']
    log = open(os.path.join(chunk, 'seedvr2.log'), 'w', encoding='utf-8')
    if subprocess.run(cmd, cwd=os.path.join(seedvr, 'repo'), env=env, stdout=log, stderr=subprocess.STDOUT).returncode:
        print('seedvr2: SeedVR2 failed, see ' + os.path.join(chunk, 'seedvr2.log'), flush=True)
        return 1

    pngs = sorted(glob.glob(os.path.join(results, '**', '*.png'), recursive=True))
    if len(pngs) != len(frames):
        print(f'seedvr2: expected {len(frames)} pictures, got {len(pngs)}', flush=True)
        return 1
    with open(result, 'wb') as p:
        for path in pngs:
            img = cv2.imread(path)
            if img is None or img.shape[0] != h * 4 or img.shape[1] != w * 4:
                print('seedvr2: unexpected picture ' + path, flush=True)
                return 1
            rgb = np.ascontiguousarray(img[:, :, ::-1]).tobytes()
            p.write(rgb)
    return 0


if __name__ == '__main__':
    sys.exit(main())
