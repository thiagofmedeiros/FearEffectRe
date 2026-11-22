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
