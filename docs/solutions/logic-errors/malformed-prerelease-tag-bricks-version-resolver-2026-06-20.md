---
title: Malformed prerelease tag permanently bricks the SemVer version resolver
date: 2026-06-20
category: logic-errors
module: release tooling (scripts/ci/resolve-version.sh)
problem_type: logic_error
component: tooling
symptoms:
  - "the release-alpha / release-beta workflow aborts on every run with a set -u unbound-variable error coming from the $((n + 1)) counter arithmetic"
  - "no new vX.Y.Z-alpha.N or vX.Y.Z-beta.N tag is ever minted again for that base version"
  - "re-running the workflow never recovers, because git release tags are immutable and the offending tag cannot be deleted or moved"
root_cause: missing_validation
resolution_type: code_fix
severity: critical
tags:
  - ci-cd
  - semver
  - prerelease-tags
  - release-automation
  - shell-scripting
  - immutable-tags
---

# Malformed prerelease tag permanently bricks the SemVer version resolver

## Problem

`scripts/ci/resolve-version.sh` (the shared alpha/beta/stable tag resolver, byte-identical across all four SST repos) computes the next prerelease counter by string-splitting the highest existing tag. A single malformed prerelease tag for a base version — e.g. `v0.1.0-alpha.x` (non-numeric counter) or `v0.1.0-alpha.10.2` (extra dot) — makes the resolver crash, and because release tags are immutable, that base version's alpha/beta line is **permanently bricked**: no future merge can ever mint another prerelease.

## Symptoms

- The `release-alpha.yml` / `release-beta.yml` job fails on every run with a `set -u` "unbound variable" error from the `$((n + 1))` arithmetic.
- No new `vX.Y.Z-alpha.N` / `-beta.N` tag is minted once a malformed tag exists for that base.
- The failure is **unrecoverable by re-running** — the "Release Tags" ruleset makes `v*` tags immutable (no delete / move / force-push), so the poison tag cannot be removed without an admin ruleset-bypass.

## What Didn't Work

- The original `resolve-version.sh` shipped with an 18-case test suite that was **shellcheck-clean and fully green** — but every case used well-formed counters, so the suite gave false confidence. The defect was invisible until an adversarial reviewer deliberately constructed a malformed tag.
- "The tag ruleset will keep tags well-formed" is **false**. The "Release Tags" ruleset regex is `^v\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$` — the prerelease group `[0-9A-Za-z.-]+` happily admits `-alpha.x` and `-alpha.10.2`. The ruleset does **not** defend the script; the script must defend itself.

## Solution

Filter `git tag -l` output to **strictly-formed numeric counters** before the sort + arithmetic, so a malformed tag is ignored instead of parsed.

```bash
# BEFORE — crashes on a non-numeric / multi-dot suffix
max_counter() {
  local base="$1" rung="$2" top
  top="$(git tag -l "v${base}-${rung}."'*' --sort=-v:refname | head -1 || true)"
  [ -z "$top" ] && { echo 0; return; }
  echo "${top##*.}"        # v0.1.0-alpha.x -> "x";  $((x + 1)) aborts under set -u
}                          # v0.1.0-alpha.10.2 -> "2"; silently regresses the counter

# AFTER — only vBASE-rung.N with an all-digit N survives the grep
max_counter() {
  local base="$1" rung="$2" top
  local re_base="${base//./\\.}"                       # escape dots in the base for the regex
  top="$(git tag -l "v${base}-${rung}."'*' --sort=-v:refname \
    | grep -E "^v${re_base}-${rung}\.[0-9]+\$" \
    | head -1 || true)"
  [ -z "$top" ] && { echo 0; return; }
  echo "${top##*.}"
}
```

The companion test suite grew 18 → 32 cases, adding malformed-counter coverage: `-alpha.x` ignored, `-alpha.10.2` ignored (no silent regression), an only-malformed base falling back to `.1`, and the equivalent beta cases.

## Why This Works

`git tag -l 'v0.1.0-alpha.*'` returns **every** matching ref — including any hand-cut, imported, or typo'd tag. The original code trusted positional parsing: `${top##*.}` takes the text after the last dot. For `-alpha.x` that is `x`, and `$(( x + 1 ))` under `set -u` dereferences an unset variable named `x` → the script aborts (exit 1) → the workflow fails. For `-alpha.10.2` it takes `2`, silently regressing below `alpha.10`, which then collides with an existing immutable tag on the next `gh release create`. Anchoring the suffix to `\.[0-9]+$` before sort+`head` means a malformed tag can never reach the arithmetic. Tag immutability is what turns a transient crash into a permanent brick, so the fix has to make the resolver tolerant rather than relying on cleanup.

## Prevention

- **Validate at the source, not at the point of use.** When deriving a value from `git tag -l` / `git for-each-ref`, filter to the exact expected grammar (`grep -E`) before any arithmetic or sort-by-position. Never feed an externally-influenced token straight into `$(( ))` under `set -u`.
- **Always include malformed / adversarial inputs in tests for parsers.** A green suite over only well-formed inputs proves nothing about robustness. The brick bug would have been caught at authoring time by a single `-alpha.x` fixture.
- **Treat immutable state as a hazard multiplier.** Any logic that reads immutable artifacts (release tags, published releases) must fail *safe* — skip/ignore the bad item — because there is no cleanup path.
- **Optional defense-in-depth:** tighten the "Release Tags" ruleset `tag_name_pattern` to `^v\d+\.\d+\.\d+(-(alpha|beta)\.\d+)?$` so the ladder grammar is enforced at push time too. The script self-defense above is the primary guard; the ruleset is a backstop.

## Related Issues

- Sibling design doc: `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md` (in sst-cam-proto / sst-cam-app / sst-cam-firmware) — the alpha/beta/stable maturity-ladder + GITHUB_TOKEN pipeline this resolver powers. Cross-reference: that ladder relies on the counter math hardened here.
- `docs/ci/version-reset-runbook.md` — before deleting bogus `v0.1.0` / `v0.1.1` tags, confirm no malformed prerelease tags remain, or the resolver will brick again on the next merge.
- Surfaced by the post-implementation adversarial code review (P1) alongside the squash-merge promote-derivation gap and the app-vs-firmware promote digest-verification asymmetry.
