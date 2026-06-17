---
title: "Floor check bugprone-easily-swappable-parameters is NOLINT-suppressed on genuinely transposable production signatures"
date: 2026-06-16
status: open
category: bugs
module: control, overlay, storage, network
problem_type: design_smell
component: api-design
severity: medium
origin: code review of fix/clang-tidy-hard-gate (clang-tidy hard-gate work)
applies_when:
  - "Adding or reordering constructor/method params with two+ adjacent same-typed args"
  - "Deciding whether to NOLINT a bugprone-* floor check vs fix it"
  - "Designing value types for ports/dimensions/identifiers"
---

# Floor check suppressed where it would catch a real swap

## Summary

The clang-tidy hard gate (see
[ci-cd-release-pipeline](../solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md))
declares `bugprone-*` and `performance-*` a **floor**: "may NOT be removed from the
enforced set; fix the code or use a justified per-site `// NOLINT` — never a blanket
removal." During the cleanup to zero, `bugprone-easily-swappable-parameters` was
satisfied at several production sites with a per-site `// NOLINT` **instead of**
fixing the signature. At some of those sites the two adjacent same-typed parameters
are **genuinely transposable** — a caller can swap them, it compiles, the gate stays
green, and the wrong value is bound. The suppression is permanent, so it also
silences any *future* call site that transposes the pair.

This is the floor check doing exactly its job (flagging a real swap risk) and being
told to be quiet. The gate's "floor never relaxed" guarantee is therefore softer than
it reads: it was relaxed per-site, just not globally.

## Why it matters

- **Correctness, not style.** Two `std::uint32_t` ports or `width`/`height` are
  silently swappable; the compiler can't help and the one linter that could was
  suppressed. Today the single production call site is correct, but a new call site,
  a refactor, or a constant swap would bind the wrong value with no diagnostic.
- **Worst case is the two-string one.** `ChunkAssembler::BeginOutbound(const
  std::string& correlation_id, const std::string& data)` — transposing a correlation
  id and a payload is both easy and high-impact.
- **Erodes the gate's promise.** The whole point of making tidy a hard gate was "no
  silent hollowing." A floor-check NOLINT on a genuinely-transposable signature is a
  micro-hollowing: defensible per-site, but it accumulates.

## Affected sites (classified)

**Genuinely transposable — the bug (two adjacent, same type, real swap risk):**

| Site | Params | Type |
| --- | --- | --- |
| `src/adapters/control/ble/bluez/chunk-assembler.cpp:96` | `correlation_id`, `data` | `const std::string&` ×2 |
| `src/app/control/services/handlers/wifi-direct.handler.cpp:15` | `preview_port`, `download_port` | `std::uint32_t` ×2 |
| `src/app/overlay/services/overlay_controller/overlay-controller.cpp:14` | `out_width`, `out_height` | `std::uint32_t` ×2 |
| `src/adapters/overlay/gstreamer/gst-overlay-compositor.cpp:14` | `width`, `height` | `std::uint32_t` ×2 |
| `src/adapters/storage/opencv/opencv-jpeg-encoder.cpp:64` | `width`, `height` | `std::uint32_t` ×2 (but signature is fixed by the `IJpegEncoder` port) |

**Lower risk — different widths (convertible, so still flagged, harder to swap by accident):**

| Site | Params |
| --- | --- |
| `src/app/control/services/handlers/download.handler.cpp:14` | `download_port` (u32), `token_ttl_seconds` (u64) |
| `src/app/overlay/services/overlay_scene/overlay-scene.cpp:46` | `duration_s_override` (u32), `now_ms` (u64) |

**Justified NOLINT (true false positive — leave as-is):**

| Site | Why |
| --- | --- |
| `src/adapters/network/http/http-download-server.cpp:69` | httplib `ContentProvider` callback — order fixed by the library |
| `src/adapters/control/wifi/wpa_supplicant/wpa-p2p-parse.hpp:13` | `ParseQuotedField` — public parse API consumed positionally |

(Test-side builders — `MakeNv12Frame`, `MakeBgr8Frame`, etc. — are also NOLINT'd; they
are out of scope here. The decision was that test data builders with explicit call
sites don't warrant production-grade strong typing. Revisit only if test bugs trace
to a builder arg swap.)

## Candidate directions (for brainstorm — not decided)

1. **Strong-type the transposable pairs.** A small value type makes the swap a
   compile error and removes the NOLINT honestly:
   - `struct OutputSize { std::uint32_t width; std::uint32_t height; };` shared by the
     overlay controller, gst overlay compositor, jpeg encoder, and anything else that
     passes a resolution. Likely a `src/domain/common/` (or processing) type.
   - A port pair for wifi-direct: either a `struct ServicePorts { std::uint32_t
     preview; std::uint32_t download; }` or distinct strong-typed `PreviewPort` /
     `DownloadPort` wrappers.
   - `BeginOutbound` could take the data as a distinct type, or reorder so the two
     strings aren't adjadent, or take a small request struct.
   - **Ripple:** each strong type touches the declaring header + every call site +
     the matching test doubles/fixtures. The jpeg-encoder one also touches the
     `IJpegEncoder` port interface and all implementers — that one is the costliest.
2. **Reorder to break adjacency** where a natural non-adjacent order exists (cheap,
   but doesn't help true same-type pairs like width/height).
3. **Accept the NOLINTs but tighten the rule.** Document explicitly which floor-check
   NOLINTs are sanctioned and require a written justification + a reviewer sign-off,
   so the "floor" wording in `.clang-tidy` matches reality.
4. **Hybrid.** Strong-type the cheap, high-value ones (width/height via one shared
   `OutputSize`; the two-string `BeginOutbound`), accept documented NOLINTs for the
   interface-fixed (`IJpegEncoder`) and library-fixed (httplib) cases.

## Open questions

- Is a shared `OutputSize`/dimensions value type worth introducing now, or does it
  invite over-abstraction? Where should it live (domain/common vs processing)?
- Is the `IJpegEncoder` port worth changing (ripples to all implementers + the postproc
  path), or is that NOLINT acceptable as interface-fixed?
- Should the `.clang-tidy` "floor, never relaxed" wording be softened to "fix or
  *justified+reviewed* NOLINT", to stop the wording overpromising?
- Do we want a lint/CI check that flags *new* `NOLINT(bugprone-*)` / `NOLINT(performance-*)`
  additions in a PR diff, so floor suppressions can't grow silently?

## Evidence / provenance

- Surfaced by the adversarial reviewer during `/ce-code-review` of
  `fix/clang-tidy-hard-gate` (finding #6), deliberately deferred as a design call.
- Floor policy: `.clang-tidy` header comments + the plan
  `docs/plans/2026-06-15-001-fix-clang-tidy-hard-gate-plan.md` (Key Technical
  Decisions, "Strict-everywhere with a non-negotiable floor").
