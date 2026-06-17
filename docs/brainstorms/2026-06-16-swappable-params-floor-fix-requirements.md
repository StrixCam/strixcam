---
title: "Fix floor-check transposable params: strong-type the swap-risk signatures + harden the NOLINT policy"
date: 2026-06-16
status: ready-for-planning
category: brainstorm
module: control, overlay, storage, network, ci
origin: brainstorm of docs/bugs/2026-06-16-swappable-params-floor-nolint.md
branch: fix/clang-tidy-hard-gate
---

# Requirements — Floor-check transposable params

## Problem

The clang-tidy hard gate declares `bugprone-*` / `performance-*` a **floor** (fix or
justified per-site NOLINT — never blanket removal). During cleanup-to-zero,
`bugprone-easily-swappable-parameters` was silenced with per-site `// NOLINT` at
several production sites where the two adjacent same-typed params are **genuinely
transposable**: a caller can swap them, it compiles, the gate stays green, the wrong
value binds. The suppression is permanent, so it also silences any *future* swapping
call site. Worst case: `BeginOutbound(correlation_id, data)` — two `std::string`s,
easy to transpose, high impact.

See source bug: `docs/bugs/2026-06-16-swappable-params-floor-nolint.md`.

## Goal

Remove the floor-check NOLINTs **honestly** by making the swap a compile error at the
genuinely-transposable sites, keep documented NOLINTs only where the order is fixed by
an external library, and harden the policy so floor suppressions can't grow silently
again.

## Decisions (locked in this brainstorm)

1. **Direction: strong-type the transposable pairs, including the `IJpegEncoder` port.**
   Grounding correction to the bug doc: `IJpegEncoder` has **one** production
   implementer (`src/adapters/storage/opencv/opencv-jpeg-encoder.cpp`) + one test
   double (`tests/control/thumbnail_handler.test.cpp`) — not a costly multi-implementer
   ripple. Its `Encode(frame, width, height, quality)` is three adjacent `u32`s;
   folding `width`/`height` into `OutputSize` collapses the swap pair and leaves
   `quality` as a lone trailing `u32` (no pair left).

2. **Shared `OutputSize` value type for dimension pairs.**
   `struct OutputSize { std::uint32_t width; std::uint32_t height; };` lives in
   `src/domain/common/models/` (alongside `pixel-format.hpp`, `rotation.hpp`) and ships
   a `fmt::formatter` per the CLAUDE.md Models rule. Used by:
   - `src/app/overlay/services/overlay_controller/overlay-controller.cpp` (`out_width`/`out_height`)
   - `src/adapters/overlay/gstreamer/gst-overlay-compositor.cpp` (`width`/`height`)
   - `src/app/storage/ports/jpeg-encoder.hpp` + its one impl + the thumbnail test double

3. **Distinct strong-typed wrappers for the non-dimension pairs.**
   Transposition becomes a hard compile error; the type documents intent at each call.
   - `PreviewPort` / `DownloadPort` (each `{ std::uint32_t value; }`) for
     `src/app/control/services/handlers/wifi-direct.handler.cpp`.
   - `CorrelationId` (`{ std::string value; }`) for
     `src/adapters/control/ble/bluez/chunk-assembler.cpp` `BeginOutbound(CorrelationId, const std::string& data)`.
   - Inferred placement (confirm in planning): control-specific wrappers under
     `src/domain/control/`, each with a `fmt::formatter`. `OutputSize` stays in
     `domain/common` because it's cross-module.

4. **Lower-risk different-width pairs: reorder to break adjacency, don't strong-type.**
   - `src/app/control/services/handlers/download.handler.cpp` (`download_port` u32, `token_ttl_seconds` u64)
   - `src/app/overlay/services/overlay_scene/overlay-scene.cpp` (`duration_s_override` u32, `now_ms` u64)
   Convertible but harder to swap by accident; a non-adjacent natural order clears the
   check without a new type. If no natural reorder exists at a site, fall back to a
   documented NOLINT rather than inventing a wrapper.

5. **Documented NOLINT kept ONLY for library-fixed order.**
   - `src/adapters/network/http/http-download-server.cpp:69` — httplib `ContentProvider` callback signature.
   - `src/adapters/control/wifi/wpa_supplicant/wpa-p2p-parse.hpp:13` — `ParseQuotedField` public parse API consumed positionally.
   Each gets a one-line written justification at the suppression site.

6. **Policy hardening — both parts.**
   - Reword the `.clang-tidy` "floor, never relaxed" header to "fix OR
     *justified + reviewed* NOLINT" so the wording matches per-site reality.
   - Add a CI check that scans the PR diff for **newly added**
     `NOLINT(bugprone-*)` / `NOLINT(performance-*)` and fails unless the addition
     carries the sanctioned justification marker. Stops silent micro-hollowing of the
     floor. (Mechanism — diff grep vs. a labelled-exception registry — is a planning
     detail.)

## Out of scope

- Test-side data builders (`MakeNv12Frame`, `MakeBgr8Frame`, etc.) stay NOLINT'd —
  prior decision: explicit-call-site test builders don't warrant production-grade
  strong typing. Revisit only if a test bug traces to a builder arg swap.
- No sibling-repo work. These sites are firmware-only (BLE chunk protocol, image
  dimensions, service ports) with no proto/cross-repo surface; everything lands on
  `fix/clang-tidy-hard-gate`. If planning surfaces a shared contract, branch the
  affected repo then.

## Success criteria

- Every "genuinely transposable" site from the bug doc either (a) takes a strong type
  so transposition fails to compile, or (b) is reordered to non-adjacency.
- The only remaining `NOLINT(bugprone-easily-swappable-parameters)` in production are
  the two library-fixed sites, each with a written justification.
- `OutputSize` and each new wrapper ship a `fmt::formatter` (Models rule).
- `cmake --build --preset test` clean; `ctest --preset test` green (hardware-bound
  failures excepted); `tidy` gate green.
- `.clang-tidy` wording no longer overpromises; CI fails a synthetic PR that adds an
  unsanctioned floor-NOLINT.

## Affected sites (reference)

| Site | Change |
| --- | --- |
| `src/app/overlay/services/overlay_controller/overlay-controller.cpp` | `OutputSize` |
| `src/adapters/overlay/gstreamer/gst-overlay-compositor.cpp` | `OutputSize` |
| `src/app/storage/ports/jpeg-encoder.hpp` (+ opencv impl + thumbnail test double) | `OutputSize`, `quality` left as lone u32 |
| `src/app/control/services/handlers/wifi-direct.handler.cpp` | `PreviewPort` / `DownloadPort` |
| `src/adapters/control/ble/bluez/chunk-assembler.cpp` (+ `.hpp:68`) | `CorrelationId` |
| `src/app/control/services/handlers/download.handler.cpp` | reorder |
| `src/app/overlay/services/overlay_scene/overlay-scene.cpp` | reorder |
| `src/adapters/network/http/http-download-server.cpp` | documented NOLINT (library-fixed) |
| `src/adapters/control/wifi/wpa_supplicant/wpa-p2p-parse.hpp` | documented NOLINT (library-fixed) |
| `.clang-tidy` header + CI workflow | policy reword + new-NOLINT guard |

## Open items for planning

- Exact home + formatter wiring for the control wrappers (`domain/control` vs a control
  value-objects header).
- CI guard mechanism: PR-diff grep for added floor-NOLINTs vs. an allowlist registry;
  how the "sanctioned" marker is expressed and reviewed.
- Whether `OutputSize` should carry validation/invariants (e.g. non-zero) or stay a
  plain aggregate — `0` currently means "keep frame's native size" in the jpeg port.
