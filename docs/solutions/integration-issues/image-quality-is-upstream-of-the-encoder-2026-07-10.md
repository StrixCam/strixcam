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
  - "Raising encode bitrate, adding B-frames/lookahead, and a slower x264 preset produced NO visible quality change (only added latency)"
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
  - white-patch
  - postprocess
  - encode
  - gstreamer
  - jetson
related_components: [gstreamer-adapter, opencv-postprocessor, encode, camera-config]
---

# "Phone-like" record/stream quality was upstream of the encoder

## Problem

Software-encoded 1080p record + RTMP on the Orin Nano (IMX477, no NVENC) looked
soft, noisy, and green-tinted. The natural first move — an **encode** rework
(bitrate, drop `tune=zerolatency`, B-frames + lookahead, slower preset, deeper
buffer) — moved the needle **zero** on visible quality. Every real cause was
**upstream of the encoder**.

## What Didn't Work

- **Encode bitrate / B-frames / preset / buffer.** The stream was genuinely
  1080p/14 Mbps/High-Profile (`gst-discoverer`), yet no visible change — only added
  latency. The encode was never the detail bottleneck. Useful by-product: a deep
  pre-encode buffer + B-frames are pure latency on a live stream whose quality
  comes from resolution, so a zero-depth stream queue removed the lag for free.
- **Chasing "wrong format."** NV12 is the *only* format `nvarguscamerasrc` emits
  (ISP debayers RG10 10-bit Bayer -> 8-bit NV12); luma is full-res and 4:2:0
  matches H.264. Not a lever.

## The isolation that cracked it

The **scoreboard overlay is composited digitally** (perfect pixels) while the
**camera scene is optical**. Crop both from one frame: the overlay text was
razor-sharp while the room was mushy. Same 1080p, same encode -> **the softness is
optical/upstream, not resolution/bitrate/encode.** For the colour cast, **measure
neutral surfaces**, not the whole frame: a white ceiling read R=74 **G=87 B=87**
(green-cyan) even though the *whole-frame* mean was balanced (green walls offset by
red shadows).

## Solution — three fixes, all upstream of the encoder

1. **Sharpness: capture 4K, downscale to 1080p (supersampling).** The IMX477
   exposes only 4K30 (mode 0) and a **binned** 1080p60 (mode 1). The pipeline
   requested 1080p -> the binned readout is soft + noisy. Set the sensor caps to
   3840x2160@30 and VIC-downscale to the delivered 1080p inside the capture
   pipeline (`GStreamerAdapter::CreatePipeline`), so only ISP + VIC (hardware) run
   at 4K and the CPU-side pipeline still sees 1080p. ~2x real detail + ~2x less
   noise. `CameraConfig` gained `sensor_width/height` distinct from the delivered
   `width/height`.

2. **Resolution ceiling: postprocess output 720p -> 1080p.** `kDefaultOutputWidth/
   Height` + the `main.cpp` overlay consts were 1280x720, so record/stream were
   *upscaling* 720p->1080p regardless of the 4K capture. Raised to 1920x1080.

3. **Colour: grey-world AWB -> highlight-weighted (white-patch).** The
   postprocessor sampled a **full-frame** `cv::meanStdDev` into `FrameColorStats`
   for the auto-WB handler — fooled by green walls offset by red shadows, so it
   applied ~neutral gains and the cast survived. Fix in `opencv-postprocessor.cpp`:
   mask the sample to the **brighter-than-frame-mean, non-clipped** pixels (the lit
   surfaces carrying the illuminant) via `cv::inRange` + a masked `meanStdDev`, with
   a full-frame fallback for dark scenes. Automatic, no venue-specific gains. Test:
   `OpenCvPostprocessorTest.AutoWbSampleIsHighlightWeightedNotFullFrame` (a scene
   whose full-frame average is red but whose lit band is green).

The residual noise is genuine **low-light sensor noise** (dim room, high gain); the
ISP TNR (`SST_ISP_TUNING tnr-strength`) is the dial, and 4K supersampling already
halved it. The lens has working motorized autofocus (converges in-log).

## Why This Works

An 8-bit H.264 encoder faithfully carries whatever the sensor/ISP/postprocess hand
it — it cannot add resolution the binned mode never captured, nor remove a cast the
ISP baked in. Bitrate only stops the encoder *throwing away* detail; here there was
little to throw away. Grey-world assumes the scene averages grey; white-patch
assumes the brightest surfaces are white — far more robust when the scene's colour
distribution is unbalanced.

## Prevention

- **Before tuning an encoder for "soft" video, prove where the softness is** — the
  in-frame digital-overlay-vs-camera-scene crop test rules the encoder in or out in
  seconds.
- **Quantify colour casts on neutral surfaces**, not whole-frame means.
- **On the IMX477, capture the highest sensor mode and downscale** rather than
  requesting a low binned mode — supersampling buys sharpness + noise for free (VIC).
- **Grey-world AWB is fragile; white-patch/highlight-weighting is the cheap
  robustness upgrade.** Unit-test with a two-band frame (bright cast over dark
  shadow) whose full-frame average would report the wrong cast.
- Sibling: `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md`.
