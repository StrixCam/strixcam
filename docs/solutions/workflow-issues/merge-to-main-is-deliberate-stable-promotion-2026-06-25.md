---
title: "Merging a PR whose base is main is a deliberate stable promotion, not blanket authorization"
date: 2026-06-25
category: workflow-issues
module: cicd-release-ladder
problem_type: workflow_issue
component: development_workflow
severity: high
applies_when:
  - "told to merge a PR without its base branch named"
  - "the PR base branch is main (a stable promotion)"
  - "merging a long-lived release branch with delete_branch_on_merge enabled"
  - "a push to main auto-cuts a stable release"
tags:
  - cicd
  - release-ladder
  - main-promotion
  - pr-merge
  - branch-protection
  - admin-bypass
  - branch-deletion
  - github-actions
---

# Merging a PR whose base is main is a deliberate stable promotion, not blanket authorization

## Context

A GitHub Actions release ladder governs this repo: `feat → development →
release/X.Y.Z → main`. Pushes auto-cut prereleases on the lower rungs (alpha on
`development`, beta on `release/**`), and a push to `main` runs `release.yml`,
which cuts a **stable** release ("tag stable + copy the beta binary"). The rule is
**main-never-builds**: nothing lands on `main` except a deliberate stable promotion.

The user gave a terse instruction — **"merge pr 24."** PR #24 had base `main` (it was
`release/0.1.0 → main`), i.e. a stable promotion, not a merge up the ladder. The
agent ran `gh pr merge 24 --merge --admin` (admin-bypass, because the new `main`
ruleset requires one approval and a PR author cannot self-approve). One under-specified
command cascaded into three production-visible mutations:

1. `main` advanced.
2. The push to `main` fired `release.yml`, cutting stable tag `v0.1.0` and a
   non-prerelease GitHub release.
3. The repo's `delete_branch_on_merge: true` setting **deleted** the merged
   `release/0.1.0` head branch — its commit survived only because the
   `v0.1.0-beta.13` tag still pinned it.

The user objected: "why are you merging to main? no it merges to release." The terse
"merge PR N" had been read as blanket authorization; it was not.

## Guidance

- **Treat any PR whose base is `main` as a deliberate stable-promotion decision.**
  Before merging, check the base branch and state it back: *"PR #24 targets `main` —
  merging promotes to stable and cuts a release. Confirm?"* Do this even for a terse
  "merge PR N." "Merge PR N" is never blanket authorization to promote to `main`.
- **Know the release automation before you merge.** Where a push to `main` auto-cuts a
  stable release, merging to `main` is never "just a merge" — it ships. Read
  `release.yml`'s triggers as part of evaluating the merge.
- **Account for `delete_branch_on_merge: true`.** Merging a long-lived `release/X.Y.Z`
  branch into `main` deletes that branch. Either promote via a throwaway branch or
  release-please (don't merge the release head directly), or go in knowing recovery
  means recreating the ref.

## Why This Matters

A single under-specified merge command produced a shipped stable tag, an advanced
protected branch, and a deleted release line — each hand-unwound, including lifting
and re-arming a branch ruleset to force-push `main`. The cost of a five-second
base-branch check is trivially smaller than recovery, and recovery is not always
clean: the deleted branch's SHA was salvageable only because a tag happened to pin it;
without it the branch head could have been lost. Terse instructions compress intent —
decompress them against what the action actually *does*, not the literal verb.

## When to Apply

- Any time you're about to merge a PR and haven't confirmed its **base branch**.
- Repos with a release ladder where pushes to certain branches auto-cut releases.
- Repos with `delete_branch_on_merge: true`, especially with long-lived `release/**`
  branches.
- Any terse/ambiguous instruction ("merge PR N", "ship it") where the literal action
  has irreversible or production-visible side effects.

## Examples

**Careless flow (what happened):**
```bash
# User: "merge pr 24"
gh pr merge 24 --merge --admin
# → main advances; release.yml cuts stable v0.1.0; release/0.1.0 branch deleted
# User: "why are you merging to main? no it merges to release"
```

**Confirm-base-branch flow (what to do):**
```bash
# User: "merge pr 24" — first, inspect the base
gh pr view 24 --json number,baseRefName,headRefName,title
# baseRefName == "main"  →  STOP and confirm:
#   "PR #24 is release/0.1.0 → main. Merging promotes to STABLE:
#    release.yml will tag v0.1.0 and cut a non-prerelease, and
#    delete_branch_on_merge will delete release/0.1.0. Proceed?"
# Only merge after explicit confirmation. If the intended target was the
# release line, the PR base is wrong — fix the base, don't merge.
```

**Recovery sequence (verified undo of the bad promotion):**
```bash
# 1. Delete the stable release and its tag
gh release delete v0.1.0 --yes --cleanup-tag

# 2. Temporarily disable main's ruleset (non_fast_forward blocks the reset).
#    GET the ruleset JSON, PUT it back with enforcement disabled:
gh api repos/<org>/<repo>/rulesets/<id> \
  | jq '{name,target,enforcement:"disabled",conditions,bypass_actors,rules}' > rs.json
gh api -X PUT repos/<org>/<repo>/rulesets/<id> --input rs.json

# 3. Reset main to the pre-merge commit (force-push past non_fast_forward)
git push --force origin <pre-merge-sha>:refs/heads/main

# 4. Re-arm the ruleset (enforcement -> "active")
gh api repos/<org>/<repo>/rulesets/<id> \
  | jq '{name,target,enforcement:"active",conditions,bypass_actors,rules}' > rs.json
gh api -X PUT repos/<org>/<repo>/rulesets/<id> --input rs.json

# 5. Recreate the deleted release branch — FULL 40-char SHA required
#    (an abbreviated SHA returns HTTP 422 "At least 40 characters are required")
gh api -X POST repos/<org>/<repo>/git/refs \
  -f ref=refs/heads/release/0.1.0 \
  -f sha=<FULL-40-CHAR-SHA>
```

Recovery gotchas:
- The force-push to `main` only succeeds with the ruleset's `non_fast_forward` rule
  lifted — disable enforcement, reset, then re-enable.
- `git/refs` POST rejects abbreviated SHAs with HTTP 422; pass the full 40-char SHA.
- The deleted branch's commit was recoverable only because a tag still pinned it.
  Treat branch deletion as a real data-loss risk, not a reversible click.

## Related

- `docs/solutions/workflow-issues/branch-target-resolution-before-pr-merge-2026-06-25.md`
  — the sibling guard: how to pick the right target for **incoming** fixes (land on the
  active `release/*`, not dormant `development`). This doc covers the **outgoing** side:
  merging that release line into `main` is a stable promotion to confirm, not a default.
- `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md` — the release
  ladder and `release.yml` (push-to-main → stable tag + copy beta binary).
- `docs/ci/rulesets.md` — the branch-protection / ruleset layer this recovery had to
  temporarily lift to reset `main`.
- `CLAUDE.md` / `AGENTS.md` → "Branch + commit + tag rules" / "Release lifecycle".
