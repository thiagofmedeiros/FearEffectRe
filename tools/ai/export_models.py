"""Convert the Real-ESRGAN PyTorch weights to ONNX for the video upscaler.

Usage: python export_models.py <weights dir> <output dir>

Needs torch and onnx. Expects realesr-animevideov3.pth (fast, used while
playing) and RealESRGAN_x4plus.pth (best quality, used to build the local HD
cache) from https://github.com/xinntao/Real-ESRGAN/releases. Writes
fast_x4.onnx and best_x4.onnx: float32 NCHW RGB in 0..1, output 4x larger.
The network definitions follow Real-ESRGAN / BasicSR (BSD-3-Clause).
"""
import os
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F


class SRVGGNetCompact(nn.Module):
    def __init__(self, num_feat=64, num_conv=16, upscale=4):
        super().__init__()
        self.upscale = upscale
        self.body = nn.ModuleList()
        self.body.append(nn.Conv2d(3, num_feat, 3, 1, 1))
        self.body.append(nn.PReLU(num_parameters=num_feat))
        for _ in range(num_conv):
            self.body.append(nn.Conv2d(num_feat, num_feat, 3, 1, 1))
            self.body.append(nn.PReLU(num_parameters=num_feat))
        self.body.append(nn.Conv2d(num_feat, 3 * upscale * upscale, 3, 1, 1))
        self.upsampler = nn.PixelShuffle(upscale)

    def forward(self, x):
        out = x
        for layer in self.body:
            out = layer(out)
        out = self.upsampler(out)
        return out + F.interpolate(x, scale_factor=self.upscale, mode="nearest")


class RDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf, gc=32):
        super().__init__()
        self.rdb1 = RDB(nf, gc)
        self.rdb2 = RDB(nf, gc)
        self.rdb3 = RDB(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, nf=64, nb=23, gc=32):
        super().__init__()
        self.conv_first = nn.Conv2d(3, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(nb)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, 3, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode="nearest")))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode="nearest")))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


def load(model, path):
    state = torch.load(path, map_location="cpu", weights_only=True)
    for key in ("params_ema", "params"):
        if key in state:
            state = state[key]
            break
    model.load_state_dict(state, strict=True)
    return model.eval()


def export(model, out):
    x = torch.rand(1, 3, 64, 64)
    torch.onnx.export(model, x, out, input_names=["input"], output_names=["output"], opset_version=17,
                      dynamic_axes={"input": {2: "h", 3: "w"}, "output": {2: "h4", 3: "w4"}}, dynamo=False)
    print("wrote", out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    export(load(SRVGGNetCompact(), os.path.join(src, "realesr-animevideov3.pth")), os.path.join(dst, "fast_x4.onnx"))
    export(load(RRDBNet(), os.path.join(src, "RealESRGAN_x4plus.pth")), os.path.join(dst, "best_x4.onnx"))


if __name__ == "__main__":
    main()
