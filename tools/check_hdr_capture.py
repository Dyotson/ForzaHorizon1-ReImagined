#!/usr/bin/env python3
"""Check an opt-in Vulkan HDR attachment readback (see docs/hdr.md).

Requires NumPy. Does not open the game, change settings, or upload captures.
The reference curve allows half an 8-bit source step for GPU sRGB sampler
precision, plus RGBA16F rounding. Brightness bounds use a 0.5-nit margin.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def check(path, expect_scene=False):
    path = str(path)
    settings = json.loads(Path(path + ".json").read_text())
    height, width = settings["height"], settings["width"]
    output = np.fromfile(path + ".rgba16f", dtype="<f2").reshape(height, width, 4).astype(np.float32)
    stand_in = np.fromfile(path + ".bgra8", dtype=np.uint8).reshape(height, width, 4)
    code = stand_in[:, :, [2, 1, 0]].astype(np.float32) / 255
    coverage = stand_in[:, :, 3].astype(np.float32) / 255
    white, peak, strength = settings["paper_white"], settings["peak"], settings["strength"]

    def reference(source):
        linear = np.where(source <= 0.04045, source / 12.92, ((source + 0.055) / 1.055) ** 2.4)
        decoded = source ** settings["gamma"] if settings["gamma"] > 0 else linear
        brightness = (0.25 * np.sum(decoded * np.array([0.2126, 0.7152, 0.0722]), axis=2)
                      + 0.75 * decoded.max(axis=2))
        knee = np.clip((brightness - 0.5) * 2, 0, 1)
        boost = 1 + max(peak / white - 1, 0) * strength * knee * knee * (1 - coverage)
        result = decoded * (white / 80) * boost[:, :, None]
        if settings["compare"]:
            result[:, :width // 2] = linear[:, :width // 2] * (white / 80)
            result[:, width // 2 - 3:width // 2] = white / 80
        return result

    expected = reference(code)
    lower = reference(np.clip(code - 0.5 / 255, 0, 1)).astype(np.float16).astype(np.float32)
    upper = reference(np.clip(code + 0.5 / 255, 0, 1)).astype(np.float16).astype(np.float32)
    hdr_half = np.ones((height, width), bool)
    if settings["compare"]:
        hdr_half[:, :width // 2] = False
    ordinary = np.ones((height, width), bool)
    if settings["pattern"]:
        ordinary[int(height * 0.62):int(height * 0.88) + 1] = False
    hud = ordinary & hdr_half & (coverage == 1)
    scene = ordinary & hdr_half & (coverage == 0)
    error = np.abs(expected - output[:, :, :3])[ordinary]
    outside = ((output[:, :, :3] < lower - 0.002) | (output[:, :, :3] > upper + 0.002))[ordinary]
    max_nits = output[:, :, :3].max(axis=2) * 80
    hud_max = float(max_nits[hud].max()) if hud.any() else None
    expanded = int((scene & (max_nits > white + 0.5)).sum())
    report = {
        "width": width, "height": height, "settings": settings,
        "finite": bool(np.isfinite(output).all()),
        "peak_channel_nits": float(max_nits.max()),
        "hud_pixels": int(hud.sum()), "hud_max_channel_nits": hud_max,
        "scene_pixels": int(scene.sum()), "scene_pixels_above_paper_white": expanded,
        "mean_curve_error_nits": float(error.mean() * 80),
        "max_curve_error_nits": float(error.max() * 80),
        "channels_outside_quantization_bounds": int(outside.sum()),
    }
    failures = []
    if not report["finite"]:
        failures.append("non-finite output")
    if report["channels_outside_quantization_bounds"]:
        failures.append("output differs from reference curve")
    if hud_max is not None and hud_max > white + 0.5:
        failures.append("opaque HUD exceeds paper white")
    limit = white + max(peak - white, 0) * strength
    if settings["pattern"] == 3:
        limit = max(limit, peak)
    if report["peak_channel_nits"] > limit + 0.5:
        failures.append("output exceeds configured highlight limit")
    if expect_scene and (not expanded or not hud.any()):
        failures.append("capture does not demonstrate both HUD protection and scene expansion")
    report["failures"] = failures
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trigger", type=Path)
    parser.add_argument("--expect-scene", action="store_true",
                        help="require HUD pixels and highlights above paper white")
    args = parser.parse_args()
    report = check(args.trigger, args.expect_scene)
    print(json.dumps(report, indent=2))
    return bool(report["failures"])


if __name__ == "__main__":
    raise SystemExit(main())
