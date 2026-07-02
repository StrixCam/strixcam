---
title: "IMX477 magenta/pink cast is a per-sensor ISP white-balance problem, not saturation"
date: 2026-06-30
category: tooling-decisions
module: adapters
problem_type: tooling_decision
component: gstreamer_pipeline
severity: medium
applies_when:
  - "An IMX477 (or similar Argus/nvarguscamerasrc sensor) shows a pink/magenta or green colour cast"
  - "You are tempted to fix camera colour by tweaking `saturation` or `wbmode` on `nvarguscamerasrc`"
  - "Two cameras of the same model render colour differently on one device"
tags:
  - imx477
  - nvarguscamerasrc
  - white-balance
  - isp
  - camera
  - color
  - jetson
---

# IMX477 magenta/pink cast is a per-sensor ISP white-balance problem, not saturation

## Context

The dual-IMX477 camera showed a pink/magenta colour cast (reported as "pink" by the operator). The obvious levers on `nvarguscamerasrc` are `wbmode` and `saturation`, and the plan assumed white balance. On-device characterization (stop the firmware — Argus is exclusive, so `gst-launch nvarguscamerasrc` can't run alongside the service — then sweep `wbmode` and `saturation` while capturing JPEGs) showed a more subtle picture.

## Guidance

- **`wbmode` already defaults to `1` (auto)** on `nvarguscamerasrc`. Adding `wbmode=1` changes nothing. The non-auto modes (`daylight`, `cloudy`, `off`, …) each impose a *worse* cast (heavy green or muddy red) — auto is the correct setting.
- **`saturation` only scales colour intensity, it cannot correct a hue bias.** Lowering `saturation` from the default `1.0` toward `0.7` mutes an over-saturated frame (the default over-saturates and pushes magenta; `saturation=1.5` makes it extreme pink), but on a sensor whose white balance is genuinely biased toward magenta, desaturating just yields a *greyer* magenta — the hue offset remains.
- **The cast can be per-sensor.** With the same pipeline (`wbmode=1 saturation=0.85`) on both cameras, `cam1` rendered neutral while `cam0` kept a magenta hue — sensor-to-sensor variation in the Argus ISP tuning, not a pipeline bug.
- **`nvarguscamerasrc` exposes no per-channel white-balance gains.** Its colour-relevant properties are `wbmode`, `awblock`, `saturation`, `gainrange`, `ispdigitalgainrange`, `tnr-*` — none of which can push a green/magenta tint. `videobalance` exists but only does hue-rotation/saturation/brightness, which distorts all hues rather than correcting a tint.
- **The real fix lives in vendor ISP tuning**, not the GStreamer pipeline: the per-sensor Argus tuning objects under `/var/nvidia/nvcam/settings/` (`imx477.nito`, per-sensor `nvcam_cache_{0,1}.bin`), or a custom colour-matrix stage. This is deferred research, not a one-line prop.

## Why This Matters

Shipping `saturation=0.85` as "the pink fix" is a false positive: it visibly helps one sensor and mutes the other, so a quick check on the good camera reads as solved while the reported camera is still wrong. Chasing colour via `saturation`/`wbmode` burns device-session time on levers that physically cannot correct a hue bias.

## When to Apply

Before attributing an Argus-sensor colour cast to white balance: confirm `wbmode` is already `auto`, characterize **each** sensor independently (Argus is exclusive — stop the consumer first), and treat a residual hue offset as ISP-tuning work (`.nito`/`nvcam_cache`), not a pipeline property. Use `saturation` only to tame genuine over-saturation, never to fix a tint.

Related: camera colour + focus context in the workspace notes; the capture pipeline lives in `src/adapters/capture/frame/gstreamer/`.
