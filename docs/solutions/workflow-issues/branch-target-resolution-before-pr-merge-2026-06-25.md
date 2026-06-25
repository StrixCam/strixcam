---
title: "Resolve the active branch target from live repo state before opening or merging a PR"
date: 2026-06-25
category: workflow-issues
module: development_workflow
problem_type: workflow_issue
component: development_workflow
severity: high
applies_when:
  - "a documented branch model exists (e.g. feat/* → development → release/* → main)"
  - "a release/* branch is open and actively iterating beta tags"
  - "an agent or contributor opens or merges a PR without an explicit target branch"
  - "development is stale relative to the active release branch"
tags:
  - branch-targeting
  - pr-merge
  - release-workflow
  - agent-autonomy
  - beta-release
  - git-state
  - ci
---

# Resolve the active branch target from live repo state before opening or merging a PR

## Context

A repo's documented branch flow (here, `feat/* → development → release/X.Y.Z → main`)
describes the **ideal steady-state lifecycle**, not necessarily the **live active
line**. In a release-branch model the team cuts `release/X.Y.Z` off `development`
and then iterates betas *on the release branch* (flashing each `vX.Y.Z-beta.N` to
hardware), while `development` goes dormant.

During an autonomous pipeline (`ce-work → ce-doc-review → ce-code-review → "pr
merge"`) an agent finished a CI change and was told to "pr merge". It noticed that
remote `development` was stale, but still followed the documented flow and opened +
**auto-merged** the PR into `development`. That was wrong: the active line was
`release/0.1.0` (already at `v0.1.0-beta.10`, being flashed and tested), and the fix
belonged there as `v0.1.0-beta.11`. The agent read CLAUDE.md's canonical flow but
never checked live state — that `release/0.1.0` existed, that the highest tag was a
*beta*, or that `development` was ~13 commits / 89 files behind. "Auto-merge when
green" was treated as license to pick the target unilaterally.

## Guidance

Run a **Pre-Merge Branch-Target Check** *before* writing or auto-merging any
cross-branch PR — not after.

**Step 1 — enumerate live remote branches:**
```bash
git ls-remote --heads origin
```
Any `refs/heads/release/*` means the team is in an active release cycle.

**Step 2 — enumerate live tags, newest first:**
```bash
git ls-remote --tags origin | grep -v '\^{}' | sort -t/ -k3 -V | tail -20
```
If the highest tag is `vX.Y.Z-beta.N`, the live line is `release/X.Y.Z` and you are
mid-beta. If the highest is `vX.Y.Z-alpha.N` with no betas, `development` is correct.

**Step 3 — measure your branch's divergence from the candidate target:**
```bash
git fetch origin
git rev-list --count origin/<candidate-target>..HEAD   # commits ahead
git diff --stat origin/<candidate-target>...HEAD        # files touched
```
A large diff (tens of files / hundreds of lines) for a small change means you are
probably targeting the wrong base.

**Decision rule:**

| Live state | Change type | Correct target |
|---|---|---|
| `release/X.Y.Z` open (mid-beta) | **fix, improvement, or CI/CD change — anything for the current release** | `release/X.Y.Z` |
| `release/X.Y.Z` open (mid-beta) | genuinely next-cycle feature (rare) | **confirm with the user** — do *not* default to a direct `development` PR |
| No `release/*` open, highest tag is `-alpha.N` | any | `development` |
| No tags at all | any | `development` |

**Fixes go into the current cut-off.** While a `release/X.Y.Z` is open, that branch
is the integration target for essentially all work — fixes, improvements, and CI/CD
changes alike. A bug fix or CI fix is *not* "next-cycle"; it belongs on the active
release so it's flashed/tested in the next beta.

**The cascade — `development` is NOT a direct merge target while a release is open.**
`development` catches up *after* the release finishes, not via direct PRs:

```
release/X.Y.Z  ──(fixes land here, beta.N…)──►  merge to main (release done)
                                                      │
main  ──────────────────────────────────────────────►  development pulls from main
                                                      └─►  cascades down
```

So during an active release: land everything on `release/X.Y.Z`; let it reach
`development` through `main` afterward. Opening a PR straight to `development`
mid-release duplicates the work and pointlessly runs the alpha pipeline.

**When base divergence is large (>~10 commits or >~20 files), stop and confirm the
target with the user before opening the PR.** Do not treat "auto-merge when green"
as permission to choose the target — that authorization presumes the *right* target.

## Why This Matters

Getting the merge target wrong in a release-branch model compounds:

- **The fix lands on the wrong line.** The team keeps flashing betas from
  `release/X.Y.Z`; a fix merged to dormant `development` is invisible to them and the
  gap it closed persists in the live line.
- **A revert round-trip on a shared branch.** Reverting on `development` / `release/*`
  needs its own PR + checks + merge, churning shared history other branches may have
  already consumed.
- **Release-tag arithmetic is disturbed.** A spurious commit on `development` can
  trigger an unintended alpha bump or pollute the commit log that
  `scripts/ci/resolve-version.sh` reads for bump-type detection.
- **Operator trust in autonomous merging erodes.** One wrong auto-merge and the team
  stops granting auto-merge, adding friction to all later autonomous work.

## When to Apply

- Before **any** cross-branch PR in a repo that uses a release-branch model
  (`release/*` exists or has ever existed).
- **Doubly** when auto-merge is granted or inside an autonomous pipeline where no
  human reviews the target before merge.
- Whenever `git rev-list --count origin/<target>..HEAD` is large relative to the size
  of your change — large divergence signals the documented default and the live
  target have drifted apart.
- Any time CLAUDE.md / AGENTS.md document a branch flow: that flow is the
  steady-state ideal; verify it against live remote state before acting.

## Examples

**Naive (wrong):**
```
Read CLAUDE.md: "feat/* → development → release/X.Y.Z → main"
No explicit target given → default to development
Open PR: fix → development ; auto-merge on green
Result: fix on dormant alpha line; beta-10 hardware line never sees it
Remediation: revert PR on development, cherry-pick onto release/0.1.0, new PR → beta.11
```

**Correct:**
```
git ls-remote --heads origin            → refs/heads/release/0.1.0 exists
git ls-remote --tags origin | ... | tail → highest tag v0.1.0-beta.10  (live line is beta)
git diff --stat origin/development...HEAD → 89 files / 13 commits  (development is dormant)
Conclusion: CI fix for the current release → target release/0.1.0
Open PR → release/0.1.0 → merge → v0.1.0-beta.11 cut automatically
```

## Related

- `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md` — documents
  the pipeline + the `feat → development → release/X.Y.Z → main` flow this learning
  caveats. The flow there is correct for the *normal* case; this doc applies when a
  `release/*` branch is the active integration line.
- `CLAUDE.md` → "Branch + commit + tag rules" / "Release lifecycle", and `AGENTS.md`
  → "Branch + commit + tag rules" — the documented default-to-`development` guidance
  that is correct but incomplete when a release branch is open.
