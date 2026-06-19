# Version reset runbook — seed the clean `0.1.0-alpha` line

> **ONE-TIME MAINTAINER RUNBOOK (admin only).** Run this once, by hand, to delete
> the two bogus tags `v0.1.0` and `v0.1.1` (auto-cut by the old
> push-to-main-auto-release flow; they correspond to no real, tested release) and
> seed the SST maturity ladder at `0.1.0-alpha`. Not executed by CI. Run it
> **after** U0 (bootstrap `development`) and the new workflows have landed, so the
> first `development` merge can mint `v0.1.0-alpha.1`.

## Why both tags must go

`v0.1.0` and `v0.1.1` were minted by the removed `release.yml` auto-cut step on
pushes to `main` — neither was ever flashed to a Jetson or hardware-tested.
Keeping them would corrupt the ladder math (`resolve-version.sh` bumps from the
latest *stable* tag) and imply releases that never happened. Both go; the line
restarts at `0.1.0-alpha`.

## Precondition

Confirm **no consumer superproject pins `v0.1.0` or `v0.1.1`** (no submodule or
release-asset reference elsewhere expects those tags) before deleting them.

```bash
git tag -l 'v*'   # expect to see v0.1.0 and v0.1.1 (the bogus pair), nothing else stable
```

## Steps

The immutable **"Release Tags"** ruleset blocks tag deletion, so the bypass is
**mandatory, not optional**:

1. **Bypass the tag ruleset** (GitHub UI → Settings → Rules → Rulesets →
   "Release Tags"): either temporarily set its enforcement to *Disabled*, or add
   yourself to its bypass list. Do this for the deletion only.

2. **Delete both bogus Releases + their tags:**

   ```bash
   gh release delete v0.1.0 --yes --cleanup-tag
   gh release delete v0.1.1 --yes --cleanup-tag
   ```

   `--cleanup-tag` removes the underlying git tag along with the Release. If a
   tag exists without a Release, delete it directly:

   ```bash
   git push origin --delete v0.1.0 v0.1.1   # only if the gh step left a tag behind
   ```

3. **Verify neither bogus tag remains** (and no other bogus stable tag):

   ```bash
   git fetch --tags --prune
   git tag -l 'v*'        # expect: empty (no v0.1.0, no v0.1.1, no other stable)
   gh release list        # expect: no v0.1.0 / v0.1.1 entries
   ```

4. **Re-enable the "Release Tags" ruleset immediately** (set enforcement back to
   *Active* / remove yourself from the bypass list). Do not leave it disabled.

## Seed the `0.1.0-alpha` line

With the bogus tags gone, the next `feat:` merge into `development` mints
`v0.1.0-alpha.1` automatically (`resolve-version.sh alpha` bumps from an implicit
`v0.0.0`, so a first `feat:` → base `0.1.0`).

To seed deterministically without waiting for a `feat:` merge, dispatch
`release-alpha.yml` with the version override:

```bash
gh workflow run release-alpha.yml -f version=v0.1.0     # mints v0.1.0-alpha.1
```

(`IN_VERSION=v0.1.0` forces the base; the alpha rung + counter still apply, so a
clean repo seeds to exactly `v0.1.0-alpha.1`.)

## Targets

- **`0.1.0-beta.1`** — the immediate target: the first `release/0.1.0` cut, whose
  beta binary is flashed to a real Jetson and tested **jointly with the app**
  (the firmware+app hardware sign-off). Coordinate the cut timing with the app
  plan.
- **`1.0.0`** — the eventual **first stable** release. Not cut by this work.

## Verify

```bash
git tag -l 'v*'   # no v0.1.0 / v0.1.1; first development alpha is v0.1.0-alpha.1
```
