---
title: "fix: Strong-type transposable params + harden floor-check NOLINT policy"
type: fix
status: completed
date: 2026-06-16
origin: docs/brainstorms/2026-06-16-swappable-params-floor-fix-requirements.md
---

# fix: Strong-type transposable params + harden floor-check NOLINT policy

## Summary

Replace the `// NOLINT(bugprone-easily-swappable-parameters)` suppressions on genuinely-transposable production signatures with strong types that make a swap a compile error: a shared `OutputSize{width,height}` for dimension pairs, distinct `PreviewPort`/`DownloadPort` and `CorrelationId` wrappers for the rest. Keep documented NOLINTs only where order is fixed by an external library or no natural reorder exists. Then reword the `.clang-tidy` "floor, never relaxed" header to match per-site reality and add a CI guard that fails on new unsanctioned floor-check NOLINTs.

---

## Problem Frame

The clang-tidy hard gate declares `bugprone-*`/`performance-*` a floor (fix or justified per-site NOLINT — never blanket removal). During cleanup-to-zero, `bugprone-easily-swappable-parameters` was silenced per-site at signatures where two adjacent same-typed params are genuinely transposable — a caller can swap them, it compiles, the gate stays green, the wrong value binds, and the suppression silences every *future* call site too. Worst case: `ChunkAssembler::BeginOutbound(correlation_id, data)` — two `std::string`s, easy to transpose, high impact. (see origin: `docs/brainstorms/2026-06-16-swappable-params-floor-fix-requirements.md`)

---

## Requirements

- R1. Every "genuinely transposable" site either takes a strong type (swap fails to compile) or is reordered to non-adjacency.
- R2. `OutputSize` and each new wrapper ship a `fmt::formatter` (CLAUDE.md Models rule).
- R3. The only remaining `NOLINT(bugprone-easily-swappable-parameters)` in production are external-order-fixed sites, each with a written justification.
- R4. `.clang-tidy` floor wording no longer overpromises ("fix OR justified+reviewed NOLINT").
- R5. CI fails a PR that adds an unsanctioned `NOLINT(bugprone-*|performance-*)`.
- R6. `tidy`/`format`/`test` gates stay green; build clean under `cmake --build --preset test`.

**Origin acceptance examples:** none enumerated (origin is a design-decision requirements doc).

---

## Scope Boundaries

- Test-side data builders (`MakeNv12Frame`, `MakeBgr8Frame`, etc.) stay NOLINT'd — prior decision; explicit-call-site builders don't warrant production-grade strong typing.
- No sibling-repo work — all sites are firmware-only with no proto/cross-repo surface. All work lands on `fix/clang-tidy-hard-gate`.
- No change to runtime behavior — pure type/signature refactor + lint policy. Wire values bound stay identical.

---

## Context & Research

### Relevant Code and Patterns

- **Value-type + formatter pattern:** `src/domain/common/models/pixel-format.hpp` + `src/domain/common/models/formatter/pixel-format-fmt.hpp`. Each domain model has a `formatter/<model>-fmt.hpp` re-exported from a sibling aggregator. `OutputSize` follows this in `src/domain/common/models/`.
- **Strong-type precedent:** `src/domain/common/utils/uuid.hpp`.
- **Control domain home for wrappers:** `src/domain/control/models/` already exists.
- **Existing suppression sites** carry rationale comments explaining each was suppressed because reordering would break a cross-file (test-instantiated) signature — exactly the case strong typing fixes honestly.
- **CI tidy job:** `.github/workflows/ci.yml` (`tidy` job, lines ~84-140) sources `scripts/tidy-args.sh`, runs `clang-tidy-14 -p build/test`. `.clang-tidy` floor header at lines ~13-25.

### Institutional Learnings

- Floor policy origin: `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md` and `docs/plans/2026-06-15-001-fix-clang-tidy-hard-gate-plan.md` (Key Technical Decisions).

---

## Key Technical Decisions

- **`OutputSize` is a plain aggregate `struct {std::uint32_t width, height;}`, not a validated value object.** The jpeg port uses `0` to mean "keep native size", so no non-zero invariant. Validation would change behavior — out of scope. (Open question in origin resolved: plain aggregate.)
- **Wrappers are distinct single-field structs** (`PreviewPort`/`DownloadPort`/`CorrelationId`), not a grouping struct — transposition becomes a hard compile error and the type names document intent at the call. (see origin decision 3)
- **`IJpegEncoder::Encode` folds `width`/`height` into `OutputSize`; `quality` stays a trailing lone `u32`** — collapses the swap pair, no pair left. One production impl + one test double, so the ripple is small (origin corrected the bug doc's "costly" framing).
- **`download.handler` / `overlay-scene` (u32/u64 pairs): documented NOLINT, not reorder.** Each has only one differently-typed param, so no natural reorder breaks adjacency; inventing a wrapper for a convertible-but-different-width pair is over-engineering. Convert the bare NOLINT into a justified one. (see origin decision 4 fallback clause)
- **Wrapper placement:** `src/domain/control/models/` with formatters, consistent with the Models rule.

---

## Open Questions

### Resolved During Planning

- Where does `OutputSize` live? → `src/domain/common/models/` (cross-module, plain aggregate).
- Where do the control wrappers live? → `src/domain/control/models/` (exists), each with a formatter.
- Reorder vs NOLINT for the u32/u64 pairs? → documented NOLINT (no natural non-adjacent order).
- CI guard mechanism? → see U7 Approach (diff-scoped grep for newly-added floor NOLINTs without a sanction marker).

### Deferred to Implementation

- Exact sanction-marker syntax for the CI guard (e.g. a trailing `// floor-ok: <reason>` token on the NOLINT line) — finalize when writing the guard script; must be greppable and reviewer-visible.
- Whether the `OutputSize` formatter aggregator needs a new `_fmt.hpp` in `domain/common/models/` or extends an existing one — confirm against the current formatter wiring at edit time.

---

## Implementation Units

### U1. `OutputSize` value type + formatter

**Goal:** Introduce the shared dimension type that collapses width/height swap pairs.

**Requirements:** R1, R2

**Dependencies:** None

**Files:**
- Create: `src/domain/common/models/output-size.hpp`
- Create: `src/domain/common/models/formatter/output-size-fmt.hpp`
- Modify: formatter aggregator in `src/domain/common/models/formatter/` (add re-export, mirroring `pixel-format-fmt.hpp` wiring)
- Test: `tests/common/output_size.test.cpp` (formatter render + aggregate construction)

**Approach:**
- `struct OutputSize { std::uint32_t width; std::uint32_t height; };` — plain aggregate, no invariant (`0` = native size remains valid).
- `fmt::formatter<sst::...::OutputSize>` renders `OutputSize{width=…, height=…}`, mirroring `pixel-format-fmt.hpp`.

**Patterns to follow:** `src/domain/common/models/pixel-format.hpp` + its formatter.

**Test scenarios:**
- Happy path: aggregate-init `{1920,1080}` → fields read back equal.
- Happy path: `fmt::format` of an `OutputSize` produces the expected `width=`/`height=` string.
- Edge case: `{0,0}` formats and round-trips (native-size sentinel must remain representable).

**Verification:** Type + formatter compile; the new test passes; nothing else changed yet.

---

### U2. Apply `OutputSize` to overlay controller + gst compositor

**Goal:** Remove the two overlay dimension NOLINTs honestly.

**Requirements:** R1, R3

**Dependencies:** U1

**Files:**
- Modify: `src/app/overlay/services/overlay_controller/overlay-controller.hpp` + `.cpp` (ctor `out_width,out_height` → `OutputSize`)
- Modify: `src/adapters/overlay/gstreamer/gst-overlay-compositor.hpp` + `.cpp` (ctor `width,height` → `OutputSize`)
- Modify: every instantiation site (production + tests) of both ctors
- Test: existing `tests/overlay/*` updated to construct with `OutputSize`

**Approach:**
- Replace the two `std::uint32_t` ctor params with a single `OutputSize`; drop the `// NOLINT` and its rationale comment. Internal `out_width_`/`width_` members can stay or read from the struct — implementer's call, behavior unchanged.

**Patterns to follow:** existing ctor + member-init style in both files.

**Test scenarios:**
- Happy path: overlay controller / compositor constructed with `OutputSize{w,h}` produces the same output dimensions as before.
- Integration: existing overlay render/composite tests still pass unchanged in behavior.

**Verification:** Both NOLINTs gone; `tidy` clean for both files; overlay tests green.

---

### U3. Apply `OutputSize` to `IJpegEncoder` port + impl + caller

**Goal:** Collapse the jpeg encoder's width/height swap pair across the port and its one implementer.

**Requirements:** R1, R3

**Dependencies:** U1

**Files:**
- Modify: `src/app/storage/ports/jpeg-encoder.hpp` (`Encode(frame, OutputSize, quality)`)
- Modify: `src/adapters/storage/opencv/opencv-jpeg-encoder.hpp` + `.cpp` (impl signature + body; drop NOLINT at `opencv-jpeg-encoder.cpp:64`)
- Modify: `src/app/control/services/handlers/thumbnail.handler.cpp:32` (call becomes `Encode(*frame, OutputSize{req.width(), req.height()}, req.quality())`)
- Test: `tests/control/thumbnail_handler.test.cpp` (test double `Encode` signature + assertions)

**Approach:**
- Port `Encode(const Frame&, std::uint32_t width, std::uint32_t height, std::uint32_t quality)` → `Encode(const Frame&, OutputSize, std::uint32_t quality)`. `quality` is now the lone `u32` — no swappable pair, NOLINT unneeded.
- Update the port doc comment (`0 = keep native size`) to refer to `OutputSize` fields.

**Patterns to follow:** existing port/impl signature; proto-field access at the thumbnail call site.

**Test scenarios:**
- Happy path: `Encode` with `OutputSize{W,H}` yields the same encoded bytes / dimensions as the old positional call.
- Edge case: `OutputSize{0,0}` (keep-native-size) path behaves as before.
- Integration: thumbnail handler builds `OutputSize` from proto `width()`/`height()` and the test double records the expected size + quality.

**Verification:** Port + impl + caller + test double compile; jpeg NOLINT removed; thumbnail tests green.

---

### U4. `PreviewPort`/`DownloadPort` wrappers for wifi-direct handler

**Goal:** Make the two service ports non-transposable.

**Requirements:** R1, R2, R3

**Dependencies:** None

**Files:**
- Create: `src/domain/control/models/service-ports.hpp` (`struct PreviewPort {std::uint32_t value;}` + `struct DownloadPort {std::uint32_t value;}`)
- Create: `src/domain/control/models/formatter/service-ports-fmt.hpp` + aggregator re-export
- Modify: `src/app/control/services/handlers/wifi-direct.handler.hpp` + `.cpp:15` (ctor params → `PreviewPort`/`DownloadPort`; drop NOLINT)
- Modify: `src/main.cpp:182` (construction site — wrap the two ports)
- Test: `tests/control/*` wifi-direct handler tests + `tests/control/service_ports.test.cpp` (formatter)

**Approach:**
- Two distinct single-field structs so `PreviewPort`↔`DownloadPort` transposition is a compile error. Each ships a formatter (Models rule).

**Patterns to follow:** `pixel-format` value+formatter pattern; existing wifi-direct ctor.

**Test scenarios:**
- Happy path: handler constructed with `PreviewPort{p}, DownloadPort{d}` binds the same ports as before.
- Edge case (compile-time, documented): passing them swapped no longer compiles — note as a rationale comment, not a runtime test.
- Happy path: each wrapper's formatter renders its value.

**Verification:** wifi-direct NOLINT removed; `main.cpp` updated; tests green; `tidy` clean.

---

### U5. `CorrelationId` wrapper for `BeginOutbound`

**Goal:** Eliminate the highest-impact two-string swap.

**Requirements:** R1, R2, R3

**Dependencies:** None

**Files:**
- Create: `src/domain/control/models/correlation-id.hpp` (`struct CorrelationId {std::string value;}`)
- Create: `src/domain/control/models/formatter/correlation-id-fmt.hpp` + aggregator re-export
- Modify: `src/adapters/control/ble/bluez/chunk-assembler.hpp:68` + `.cpp:96` (`BeginOutbound(CorrelationId, const std::string& data, …)`; drop NOLINT)
- Modify: `src/adapters/control/ble/bluez/bluez-ble-transport.cpp:284` (caller wraps the id)
- Test: `tests/control/chunk_assembler.test.cpp` (4 call sites: lines ~148, 167, 307, 349 → wrap id in `CorrelationId{…}`)

**Approach:**
- `CorrelationId` wraps the id string; `data` stays a bare `const std::string&`. Two strings are no longer the same type → swap is a compile error. Keep `data` second.

**Patterns to follow:** value+formatter pattern; existing `BeginOutbound` body unchanged except param type.

**Test scenarios:**
- Happy path: `BeginOutbound(CorrelationId{"r1"}, data, send)` chunks identically to the old positional call (same chunk count for the existing byte-length cases: 8B/4 → 2, 10B → 3, etc.).
- Error path: existing empty/oversize behaviors in the test file remain unchanged.
- Happy path: `CorrelationId` formatter renders its value.

**Verification:** chunk-assembler NOLINT removed; transport caller + all 4 test call sites updated; chunk_assembler tests green.

---

### U6. Justify the residual u32/u64 NOLINTs

**Goal:** Convert the two lower-risk bare NOLINTs into sanctioned, justified ones (no signature change).

**Requirements:** R3

**Dependencies:** U7 (sanction-marker syntax must be defined first, or land together)

**Files:**
- Modify: `src/app/control/services/handlers/download.handler.cpp:14` (NOLINT + written justification)
- Modify: `src/app/overlay/services/overlay_scene/overlay-scene.cpp:46` (NOLINT + written justification)
- Modify: `src/adapters/network/http/http-download-server.cpp:69` (httplib `ContentProvider` — add/confirm sanction marker)
- Modify: `src/adapters/control/wifi/wpa_supplicant/wpa-p2p-parse.hpp:13` (`ParseQuotedField` — add/confirm sanction marker)

**Approach:**
- For `download.handler` / `overlay-scene`: the pair is `u32`/`u64` (convertible, flagged) with only one other param — no natural reorder breaks adjacency, and a wrapper for a different-width pair is over-engineering. Keep the NOLINT but attach the sanction marker + one-line reason (distinct, self-documenting names; different widths).
- For httplib + wpa-parse: external/library-fixed order — attach the same marker so U7's CI guard treats them as sanctioned.

**Test scenarios:** Test expectation: none — comment-only changes, no behavioral change.

**Verification:** All four sites carry the U7 sanction marker; `tidy` clean; CI guard (U7) passes them.

---

### U7. Reword `.clang-tidy` floor + add CI guard against new floor NOLINTs

**Goal:** Make the floor wording match reality and prevent silent re-growth of suppressions.

**Requirements:** R4, R5

**Dependencies:** None (U6 depends on the marker syntax defined here)

**Files:**
- Modify: `.clang-tidy` header comment (lines ~13-25): "FLOOR, never relaxed" → "FLOOR: fix, or a *justified + sanctioned* per-site NOLINT — never a blanket removal."
- Create: `scripts/check-floor-nolints.sh` (diff-scoped guard)
- Modify: `.github/workflows/ci.yml` (`tidy` job or a new step: run the guard on PR diff)

**Approach:**
- Guard scans the PR diff (added lines only) for `NOLINT(bugprone-*` / `NOLINT(performance-*`. An added suppression passes only if the line also carries the sanction marker (e.g. trailing `// floor-ok: <reason>` — exact token finalized at implementation). Otherwise the step fails with the offending file:line and a pointer to the floor policy.
- Diff-scoped (not whole-tree) so pre-existing sanctioned suppressions don't re-trip; only *new* additions are gated. Use the PR base ref available in the workflow.

**Patterns to follow:** `scripts/tidy-args.sh` sourcing convention; existing `tidy` job step structure in `ci.yml`.

**Test scenarios:**
- Happy path: a diff adding `// NOLINT(bugprone-easily-swappable-parameters) // floor-ok: …` passes.
- Error path: a diff adding a bare `// NOLINT(bugprone-…)` (no marker) fails with file:line.
- Edge case: a diff that touches a line near an existing sanctioned NOLINT without adding a new one passes (diff-scoped, not whole-file).
- Edge case: `NOLINT(performance-*)` additions are gated the same as `bugprone-*`.

**Verification:** `.clang-tidy` wording updated; guard script fails a synthetic unsanctioned-NOLINT diff and passes a sanctioned one; CI `tidy` job invokes it.

---

## System-Wide Impact

- **Interaction graph:** Pure signature refactor — call sites updated in lockstep with each port/ctor. No new runtime paths. `main.cpp:182` and `bluez-ble-transport.cpp:284` are the only production construction sites touched.
- **Error propagation:** Unchanged — no behavior change; values bound are identical.
- **API surface parity:** `IJpegEncoder` is the one cross-layer port touched; its single impl + test double move together. No other implementers.
- **Unchanged invariants:** Wire protocol (BLE chunking, ports bound, output dimensions) is identical. `0`-means-native-size jpeg contract preserved by the plain-aggregate `OutputSize`.
- **Integration coverage:** Existing overlay, thumbnail, and chunk-assembler module tests prove behavior is unchanged after the type swap — they're the regression net.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| Missed construction site → build break | `tidy`/build is the gate; `cmake --build --preset test` must be clean before done (CLAUDE.md: compilation is the first test). |
| New domain types missing formatters → Models-rule violation / build break | U1/U4/U5 each create the formatter in the same unit; verified by the per-type formatter test. |
| CI guard false-positives on diff context lines | Guard inspects added (`+`) lines only, requires the literal `NOLINT(` token, and is diff-scoped against the PR base ref. |
| Sanction-marker syntax churn between U6 and U7 | Define the marker in U7 first (or land U6+U7 together); U6 depends on U7. |

---

## Sources & References

- **Origin document:** `docs/brainstorms/2026-06-16-swappable-params-floor-fix-requirements.md`
- **Source bug:** `docs/bugs/2026-06-16-swappable-params-floor-nolint.md`
- Floor policy: `.clang-tidy` header; `docs/plans/2026-06-15-001-fix-clang-tidy-hard-gate-plan.md`; `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md`
