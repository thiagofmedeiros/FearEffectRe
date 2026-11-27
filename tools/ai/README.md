# AI upscaling files

The game loads these from an `ai` folder next to `FearEffectRe.exe`:

| File | Where it comes from |
|---|---|
| `onnxruntime.dll` | NuGet package [Microsoft.ML.OnnxRuntime.DirectML](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML) 1.20.1, `runtimes/win-x64/native/` |
| `DirectML.dll` | NuGet package [Microsoft.AI.DirectML](https://www.nuget.org/packages/Microsoft.AI.DirectML) 1.15.4, `bin/x64-win/` |
| `fast_x4.onnx` | `realesr-animevideov3.pth`, converted by `export_models.py` |
| `best_x4.onnx` | `RealESRGAN_x4plus.pth`, converted by `export_models.py` |

The `.pth` weights are from the [Real-ESRGAN releases](https://github.com/xinntao/Real-ESRGAN/releases)
(v0.2.5.0 and v0.1.0). A `.nupkg` is a zip file.

Converting needs PyTorch and onnx, best in a virtual environment:

```sh
python -m venv venv
venv/Scripts/pip install torch --index-url https://download.pytorch.org/whl/cpu
venv/Scripts/pip install onnx
venv/Scripts/python export_models.py <folder with the .pth files> <ai folder>
```

Then configure with `-DFE_AI_DIR=<ai folder>` so the build copies it next to the program.

`onnxruntime_c_api.h` in `third_party/onnxruntime` is from the same ONNX Runtime package
(MIT licence). Real-ESRGAN is BSD-3-Clause.

## ffmpeg (for building the HD caches)

The cache builders store the enlarged pictures as H.264 (x264, quality 18, a keyframe
every 30 frames, BT.709) in chunks of up to 300 frames, encoded by `ffmpeg.exe` from the
`ai` folder or the PATH, e.g. a "essentials" build from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/).
Playing needs only Windows' own H.264 decoder (Windows N editions: the Media Feature Pack).

## SeedVR2 (optional, for the SeedVR2 video mode)

The launcher installs SeedVR2 the first time it is chosen (after asking): it runs
`setup_seedvr2.ps1` (copied into `ai` by the build), which puts a portable Python 3.12,
PyTorch for CUDA, the SeedVR2 code (a pinned commit) and the 3B model in `ai/seedvr2`,
about 12 GB of downloads. Nothing is installed on the system. It can be run again to
finish an interrupted installation:

    powershell -NoProfile -ExecutionPolicy Bypass -File ai\setup_seedvr2.ps1 -Dir ai\seedvr2

The SeedVR2 cache builder runs `seedvr2_chunk.py` (copied into `ai` by the build) with
the Python in `ai/seedvr2/venv`, on chunks of up to 300 frames; it writes the enlarged
frames to a raw file that the builder then encodes into `hdcache/seedvr2.vpack`. `ai/seedvr2` holds:

| Folder | Contents |
|---|---|
| `python` or `venv` | the portable Python from the installer, or a Python 3.13 virtual environment made by hand, with PyTorch for CUDA 12.6 (`pip install torch torchvision --index-url https://download.pytorch.org/whl/cu126`) and the repo's `requirements.txt` |
| `repo` | [ComfyUI-SeedVR2_VideoUpscaler](https://github.com/numz/ComfyUI-SeedVR2_VideoUpscaler) (its standalone `inference_cli.py`) |
| `models/SEEDVR2` | `seedvr2_ema_3b_fp16.safetensors` and `ema_vae_fp16.safetensors` from [numz/SeedVR2_comfyUI](https://huggingface.co/numz/SeedVR2_comfyUI) |

Settings used (they fit a 24 GB card): 3B fp16, batches of 21 frames with 4 frames of
overlap, LAB colour correction, tiled VAE (512), DiT and VAE offloaded to the CPU between
phases. On an RTX 3090 that is about 1.3 seconds per frame. SeedVR2 is Apache-2.0.
