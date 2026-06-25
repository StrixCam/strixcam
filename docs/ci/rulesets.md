# Branch & tag rulesets — sst-cam-firmware

> **APPLIED 2026-06-18.** These rulesets are **live** on the repo. Four rulesets
> are active — **Release Tags** (immutable `v*` tags), **development**, **main**, and
> **release-branches** — with an **OrgAdmin** bypass actor on the branch rulesets.
> The **development** ruleset requires the green checks `ci-scripts` / `format` /
> `tidy` / `test`. The **main** ruleset's `required_status_checks` is **deferred**
> (see the OPEN caveat below); `main` is currently PR + no-direct-push + admin
> bypass. The `gh api` commands below are kept for reference and re-apply.

> **MAINTAINER RUNBOOK (admin only).** These `gh api` calls apply the GitHub
> repository rulesets that enforce the SST branch model. They are NOT executed by
> CI or by the implementing change — an admin applies them once, **after** U0
> (bootstrap `development` + default-branch flip) and **after** the first `development`
> CI run has emitted the `ci-scripts` / `format` / `tidy` / `test` check runs so
> their exact names can be wired as required status checks. Strict order:
> bootstrap → first CI run (capture check names) → apply these rulesets.
> The PR gate checks now live **inside** `release-alpha.yml` (on `development` PRs)
> and `release-beta.yml` (on `release/**` PRs); there is no standalone `ci.yml`.

## Intent

The branch model is `feat/* → development → release/X.Y.Z → main` with the maturity
ladder `alpha` (development) → `beta` (release/*) → `stable` (main). The rulesets
enforce:

| Branch          | Rule                                                                                                  |
| --------------- | ----------------------------------------------------------------------------------------------------- |
| `development`       | PR required; green `ci-scripts` / `format` / `tidy` / `test` (from `release-alpha.yml` PR run); no force-push / delete. |
| `release/**`    | Same green `ci-scripts` / `format` / `tidy` / `test` (from `release-beta.yml` PR run); no force-push / delete. |
| `main`          | PR required; **no direct push**; no force-push / delete; admin/hotfix bypass. Required checks **deferred** (see OPEN caveat). |
| tags `v*`       | Immutable — existing **"Release Tags"** ruleset; no delete / move / force. Keep it; do not weaken.     |

`main` deliberately does **not** re-run the heavy aarch64 cross-build. A
`release/X.Y.Z → main` PR's head SHA *is* the release-branch tip, which already
carries green `ci-scripts` / `format` / `tidy` / `test` from its `release/*` PR
run; GitHub surfaces those runs on the PR, so the `main` ruleset could require the
same check names with no devcontainer build re-running on `main` (the R3/AE5
mechanism). The PR gate checks live **inside** `release-alpha.yml` (development PRs)
and `release-beta.yml` (release/* PRs), `pull_request`-gated — there is no
standalone `ci.yml`. `main`'s required-status-checks rule stays **deferred** (see
the OPEN caveat below).

## OPEN caveat — verify before relying on `main`'s required check

> **(OPEN — do not resolve here, flag for the maintainer applying U7):** verify
> GitHub surfaces the release-head SHA's *push* check-run as a *status on the
> main PR* — no product workflow runs the gate checks on `main` PRs (the
> cross-build is heavy; `release.yml` on `main` only promotes). If GitHub does
> NOT surface the push-event check run as a PR status on
> the same SHA, the `main` ruleset's `required_status_checks` will never be
> satisfiable (the `release/X.Y.Z → main` PR would have no checks, AE4/AE5
> unrealizable). In that case add a lightweight `pull_request: [main]` gate that
> only asserts the `vX.Y.Z-beta.N` Release/asset exists (a NO-build assertion,
> not the cross-build) and require *that* check instead. Resolve this before
> wiring the `main` ruleset's `required_status_checks` below.

## Required-check names

Captured from the first `development` CI run (U0). They are the **job names** of the
`pull_request`-gated check jobs inside `release-alpha.yml` (and the identical set
inside `release-beta.yml`), kept byte-identical across the retarget:

- `ci-scripts`
- `format`
- `tidy`
- `test`

The `context` value in `required_status_checks` is the job's `name:`. Confirm the
exact rendered context string on the first run (GitHub may prefix it with the
workflow name in some configurations) and substitute below if so.

## Apply the rulesets

Replace `:owner`/`:repo` via the authenticated `gh` context (it expands them).

### development — PR + green checks

```bash
gh api -X POST repos/:owner/:repo/rulesets \
  -H "Accept: application/vnd.github+json" \
  --input - <<'JSON'
{
  "name": "development",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": { "include": ["refs/heads/development"], "exclude": [] }
  },
  "rules": [
    { "type": "pull_request",
      "parameters": {
        "required_approving_review_count": 0,
        "dismiss_stale_reviews_on_push": true,
        "require_code_owner_review": false,
        "require_last_push_approval": false,
        "required_review_thread_resolution": false
      }
    },
    { "type": "required_status_checks",
      "parameters": {
        "strict_required_status_checks_policy": true,
        "required_status_checks": [
          { "context": "ci-scripts" },
          { "context": "format" },
          { "context": "tidy" },
          { "context": "test" }
        ]
      }
    },
    { "type": "non_fast_forward" },
    { "type": "deletion" }
  ]
}
JSON
```

### release/** — green checks, no force-push/delete

```bash
gh api -X POST repos/:owner/:repo/rulesets \
  -H "Accept: application/vnd.github+json" \
  --input - <<'JSON'
{
  "name": "release-branches",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": { "include": ["refs/heads/release/**"], "exclude": [] }
  },
  "rules": [
    { "type": "required_status_checks",
      "parameters": {
        "strict_required_status_checks_policy": true,
        "required_status_checks": [
          { "context": "ci-scripts" },
          { "context": "format" },
          { "context": "tidy" },
          { "context": "test" }
        ]
      }
    },
    { "type": "non_fast_forward" },
    { "type": "deletion" }
  ]
}
JSON
```

### main — PR + no direct push, admin bypass (required-checks deferred)

`bypass_actors` grants an `OrgAdmin` bypass actor `always` bypass for
hotfix/version-reset operations (live state; the JSON below shows the
`RepositoryRole` id `5` form — confirm the role/org-admin id for this org via
`gh api repos/:owner/:repo/rulesets/<id>` or the Roles UI).

> **DEFERRED — not in the live `main` ruleset.** The `required_status_checks`
> rule below is **not** currently applied; `main` is live as PR + no-direct-push
> + admin bypass only. Wire `required_status_checks` here only **after**
> confirming the OPEN caveat above — if the push check-run is not surfaced as a
> PR status, swap in the no-build assertion gate's context instead of
> `ci-scripts`/`format`/`tidy`/`test`.

```bash
gh api -X POST repos/:owner/:repo/rulesets \
  -H "Accept: application/vnd.github+json" \
  --input - <<'JSON'
{
  "name": "main",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": { "include": ["refs/heads/main"], "exclude": [] }
  },
  "bypass_actors": [
    { "actor_type": "RepositoryRole", "actor_id": 5, "bypass_mode": "always" }
  ],
  "rules": [
    { "type": "pull_request",
      "parameters": {
        "required_approving_review_count": 1,
        "dismiss_stale_reviews_on_push": true,
        "require_code_owner_review": false,
        "require_last_push_approval": false,
        "required_review_thread_resolution": false
      }
    },
    { "type": "required_status_checks",
      "parameters": {
        "strict_required_status_checks_policy": true,
        "required_status_checks": [
          { "context": "ci-scripts" },
          { "context": "format" },
          { "context": "tidy" },
          { "context": "test" }
        ]
      }
    },
    { "type": "non_fast_forward" },
    { "type": "deletion" }
  ]
}
JSON
```

> The `main` ruleset pins the literal ref `refs/heads/main` (not the
> `~DEFAULT_BRANCH` alias): U0 flips the default branch to `development`, so
> `~DEFAULT_BRANCH` would resolve to `development` here — wrong target.

## Keep the immutable Release-Tags ruleset

The existing **"Release Tags"** tag ruleset (immutable `v*`, no delete/move/
force; `github-actions[bot]` may *create* compliant tags) stays as-is. Confirm
its `ref_name` include pattern (`refs/tags/v*`) admits the ladder names:

- `v0.1.0-alpha.1`, `v0.1.0-alpha.2`, … (development / release-alpha.yml)
- `v0.1.0-beta.1`, `v0.1.0-beta.2`, … (release/* / release-beta.yml)
- `v0.1.0`, `v1.0.0`, … (main / release.yml)

`refs/tags/v*` matches all three. The **only** sanctioned exception is the
one-time version reset (see `version-reset-runbook.md`), where the ruleset is
temporarily bypassed to delete the bogus `v0.1.0` / `v0.1.1` tags and re-enabled
immediately.

## Related

- **[release-commit-signing.md](release-commit-signing.md)** — why a release's
  green **"Verified"** badge depends on the target commit being PR-merged (web-flow
  signed) and not a local direct push; the proper flow to keep `-alpha.N`/`-beta.N`
  tags Verified.

## Verify

- Direct push to `main` is rejected.
- A `release/* → main` PR with red checks is blocked (AE4).
- `development` is the repository default branch; a PR into it triggers
  `ci-scripts` / `format` / `tidy` / `test` (from `release-alpha.yml`).
- `git push --delete origin vX.Y.Z` on any released tag is rejected.
