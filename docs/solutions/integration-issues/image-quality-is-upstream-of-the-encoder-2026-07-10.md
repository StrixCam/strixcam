---
title: "\"Phone-like\" record/stream quality was upstream of the encoder — binned sensor mode, 720p postprocess ceiling, and a fooled grey-world AWB (not bitrate)"
date: 2026-07-10
category: integration-issues
module: capture
problem_type: integration_issue
component: pipeline
root_cause: logic_error
resolution_type: code_fix
severity: high
symptoms:
  - "1080p record + RTMP stream look soft/pixelated/noisy with a green-teal cast, described as phone-like"
  - "Raising encode bitrate (4->14 Mbps), adding B-frames/lookahead, and a slower x264 preset produced NO visible quality change (only added latency)"
  - "gst-discoverer confirms the file/stream really is 1080p High-Profile 14 Mbps, yet the picture is still soft"
applies_when:
  - "A software (x264) capture->ISP->postprocess->encode pipeline produces a soft/noisy/colour-cast image and the instinct is to tune the encoder"
  - "Judging record/stream image quality on the Jetson Orin Nano IMX477 path"
tags:
  - image-quality
  - imx477
  - argus
  - sensor-mode
  - supersampling
  - white-balance
  - grey-world
  - auto-white-balance
  - postprocess
  - encode
  - gstreamer
  - jetson
  - focus
related_components: [capture-launch, opencv-postprocessor, auto-color, encode-fragment, vic-convert-stage]
---

# "Phone-like" record/stream quality was upstream of the encoder

## Problem

Software-encoded 1080p record + RTMP on the Orin Nano (IMX477, no NVENC) looked
soft, noisy, and green-tinted. The natural first move — a full **encode** rework
(bitrate 4→14 Mbps, drop `tune=zerolatency`, add B-frames + lookahead, slower
preset, deeper buffer) — moved the needle **zero** on visible quality. Every real
cause was **upstream of the encoder**.

## What Didn't Work

- **Encode bitrate / B-frames / preset / buffer.** Shipped and measured on metal
  (record + RTMP genuinely 1080p/14 Mbps/High-Profile per `gst-discoverer`), but
  the operator saw no change — only added latency from the deep pre-encode queue
  and B-frame reorder. The encode was never the bottleneck for detail. The one
  useful by-product: the deep buffer + B-frames are pure latency on a live stream
  whose quality comes from resolution, so `SST_STREAM_QUEUE_MS=0` removed the lag
  with no quality loss.
- **Chasing "wrong format."** NV12 is the *only* format `nvarguscamerasrc` emits
  (the ISP debayers RG10 10-bit Bayer → 8-bit NV12); luma is full-res and 4:2:0
  matches H.264. Not a lever.

## The isolation that cracked it

The **scoreboard overlay is composited digitally** (perfect pixels) while the
**camera scene is optical**. Crop both from one frame and compare: the overlay
text was razor-sharp while the room was mushy. Same 1080p, same 14 Mbps encode →
**the softness is optical/upstream, not resolution/bitrate/encode.** If the
overlay had been soft too, the encoder would be the suspect. It wasn't.

For the colour cast, **measure neutral surfaces**, not the whole frame: a white
ceiling read R=74 **G=87 B=87** (green-cyan), proving a real cast even though the
*whole-frame* mean was balanced (green walls offset by red shadows).

## Solution — three fixes, all upstream of the encoder

1. **Sharpness: capture 4K, downscale to 1080p (supersampling).** The IMX477
   exposes only two Argus modes — 4K30 (mode 0) and a **binned** 1080p60 (mode 1).
   The pipeline was requesting 1080p → the binned readout is soft + noisy. Switch
   the sensor caps to 3840×2160@30 and VIC-downscale to the delivered 1080p inside
   the capture pipeline (`capture-launch.cpp`), so only the ISP + VIC (hardware)
   run at 4K and the CPU-side pipeline still sees 1080p. ~2× real detail (JPEG size
   at equal quality) and ~2× less noise. `CameraConfig` gained `sensor_width/height`
   (4K) distinct from the delivered `width/height` (1080p).

2. **Resolution ceiling: postprocess output 720p → 1080p.** `kDefaultOutputWidth/
   Height` (postprocess-config.hpp) + `kOverlayWidth/Height` (runtime-defaults.hpp)
   were 1280×720, so record/stream were *upscaling* 720p→1080p regardless of the
   4K capture. Raised to 1920×1080. The VIC convert stays cheap; the added CPU is
   colour-correction + overlay compositing at 2.25× pixels.

3. **Colour: grey-world AWB → highlight-weighted (white-patch).** The continuous
   `AutoColorService` grey-world loop drove the *whole-frame* channel means equal
   — fooled by a scene whose green walls are offset by red shadows, so it applied
   ~neutral gains and the green cast survived. Fix in `auto-color.cpp`
   `MeasureFrameMeans`: estimate the illuminant from the **brighter-than-frame-mean,
   non-clipped** pixels (the lit surfaces, which carry the light's colour) and
   ignore dark shadows, with a full-frame fallback for genuinely dark frames. On
   metal the loop then immediately measured the real cast (R=78 G=107 B=117) and
   corrected it automatically — no venue-specific hardcoded gains.

The noise that remained is genuine **low-light sensor noise** (dim room, high
gain); the ISP TNR (`tnr-strength`, `SST_ISP_TUNING`) is the dial, and 4K
supersampling already halved it. Also confirmed the lens has working motorized
autofocus (converged in-log), so focus was not the issue here.

## Why This Works

Detail and colour are set at capture and ISP time. An 8-bit H.264 encoder
faithfully carries whatever the sensor/ISP/postprocess hand it — it cannot add
resolution the binned mode never captured, nor remove a cast the ISP baked in.
Bitrate only stops the encoder *throwing away* detail; here there was little to
throw away. Grey-world assumes the scene averages grey; white-patch assumes the
brightest surfaces are white — far more robust when the scene has an unbalanced
colour distribution.

## Prevention

- **Before tuning an encoder for "soft/bad" video, prove where the softness is.**
  In-frame digital-vs-optical crop comparison (overlay text vs camera scene) is a
  10-second test that rules the encoder in or out.
- **Quantify colour casts on neutral surfaces**, not whole-frame means — grey-world
  balance of the mean can hide a real cast.
- **On the IMX477 (and similar), prefer capturing the highest sensor mode and
  downscaling** over requesting a low binned mode — supersampling buys sharpness +
  noise for free (hardware VIC scale).
- **Grey-world AWB is fragile**; highlight/white-patch weighting is the cheap
  robustness upgrade. Unit-test it with a two-band frame (bright cast over dark
  shadow) that a full-frame average would report as neutral (see
  `AutoColorMathTest.HighlightWeightingLocksOntoLitBandNotDarkShadow`).
- Sibling: `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md`
  (the encode ceiling this work sits on top of).
