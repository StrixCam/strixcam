# Branch & tag rulesets — sst-cam-firmware

> **MAINTAINER RUNBOOK (admin only).** These `gh api` calls apply the GitHub
> repository rulesets that enforce the SST branch model. They are NOT executed by
> CI or by the implementing change — an admin applies them once, **after** U0
> (bootstrap `develop` + default-branch flip) and **after** the first `develop`
> CI run has emitted the `format` / `tidy` / `test` check runs so their exact
> names can be wired as required status checks. Strict order: bootstrap → first
> CI run (capture check names) → apply these rulesets (this is the last step).

## Intent

The branch model is `feat/* → develop → release/X.Y.Z → main` with the maturity
ladder `alpha` (develop) → `beta` (release/*) → `stable` (main). The rulesets
enforce:

| Branch          | Rule                                                                                                  |
| --------------- | ----------------------------------------------------------------------------------------------------- |
| `develop`       | PR required; green `format` / `tidy` / `test`; no force-push / delete.                                 |
| `release/**`    | Same green `format` / `tidy` / `test` (reported on `release/*` pushes via ci.yml); no force-push / delete. |
| `main`          | PR required; green `format` / `tidy` / `test`; **no direct push**; no force-push / delete; admin/hotfix bypass. |
| tags `v*`       | Immutable — existing **"Release Tags"** ruleset; no delete / move / force. Keep it; do not weaken.     |

`main` deliberately does **not** re-run the heavy aarch64 cross-build. A
`release/X.Y.Z → main` PR's head SHA *is* the release-branch tip, which already
carries green `format` / `tidy` / `test` from its `release/*` push; GitHub
surfaces those runs on the PR, so the `main` ruleset requires the same check
names with no devcontainer build re-running on `main` (the R3/AE5 mechanism).
`ci.yml` is deliberately NOT on `main` PRs (see the OPEN caveat below).

## OPEN caveat — verify before relying on `main`'s required check

> **(OPEN — do not resolve here, flag for the maintainer applying U7):** verify
> GitHub surfaces the release-head SHA's *push* check-run as a *status on the
> main PR* — `ci.yml` is deliberately NOT on `main` PRs (the cross-build is
> heavy). If GitHub does NOT surface the push-event check run as a PR status on
> the same SHA, the `main` ruleset's `required_status_checks` will never be
> satisfiable (the `release/X.Y.Z → main` PR would have no checks, AE4/AE5
> unrealizable). In that case add a lightweight `pull_request: [main]` gate that
> only asserts the `vX.Y.Z-beta.N` Release/asset exists (a NO-build assertion,
> not the cross-build) and require *that* check instead. Resolve this before
> wiring the `main` ruleset's `required_status_checks` below.

## Required-check names

Captured from the first `develop` CI run (U0). They are the **job names** in
`ci.yml`, kept byte-identical across the retarget:

- `format`
- `tidy`
- `test`

The `context` value in `required_status_checks` is the job's `name:`. Confirm the
exact rendered context string on the first run (GitHub may prefix it with the
workflow name in some configurations) and substitute below if so.

## Apply the rulesets

Replace `:owner`/`:repo` via the authenticated `gh` context (it expands them).

### develop — PR + green checks

```bash
gh api -X POST repos/:owner/:repo/rulesets \
  -H "Accept: application/vnd.github+json" \
  --input - <<'JSON'
{
  "name": "develop",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": { "include": ["refs/heads/develop"], "exclude": [] }
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

### main — PR + green checks + no direct push, admin bypass

`bypass_actors` grants the `RepositoryRole` id `5` (admin/maintainer — confirm
the role id for this org via `gh api repos/:owner/:repo/rulesets/<id>` or the
Roles UI) `always` bypass for hotfix/version-reset operations.

> Wire `required_status_checks` here only **after** confirming the OPEN caveat
> above — if the push check-run is not surfaced as a PR status, swap in the
> no-build assertion gate's context instead of `format`/`tidy`/`test`.

```bash
gh api -X POST repos/:owner/:repo/rulesets \
  -H "Accept: application/vnd.github+json" \
  --input - <<'JSON'
{
  "name": "main",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": { "include": ["~DEFAULT_BRANCH"], "exclude": [] }
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

> `~DEFAULT_BRANCH` keys the `main` ruleset to whatever GitHub considers the
> default. **After U0 flips the default to `develop`**, `~DEFAULT_BRANCH` resolves
> to `develop`, not `main` — so for the `main` ruleset, pin the literal ref
> instead: replace the `ref_name.include` value with `["refs/heads/main"]`.

## Keep the immutable Release-Tags ruleset

The existing **"Release Tags"** tag ruleset (immutable `v*`, no delete/move/
force; `github-actions[bot]` may *create* compliant tags) stays as-is. Confirm
its `ref_name` include pattern (`refs/tags/v*`) admits the ladder names:

- `v0.1.0-alpha.1`, `v0.1.0-alpha.2`, … (develop / alpha.yml)
- `v0.1.0-beta.1`, `v0.1.0-beta.2`, … (release/* / release-beta.yml)
- `v0.1.0`, `v1.0.0`, … (main / promote.yml)

`refs/tags/v*` matches all three. The **only** sanctioned exception is the
one-time version reset (see `version-reset-runbook.md`), where the ruleset is
temporarily bypassed to delete the bogus `v0.1.0` / `v0.1.1` tags and re-enabled
immediately.

## Verify

- Direct push to `main` is rejected.
- A `release/* → main` PR with red checks is blocked (AE4).
- `develop` is the repository default branch; a PR into it triggers
  `format` / `tidy` / `test`.
- `git push --delete origin vX.Y.Z` on any released tag is rejected.
