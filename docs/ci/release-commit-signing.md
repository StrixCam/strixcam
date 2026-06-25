# Signed commits → "Verified" release tags

> **Why this exists.** `v0.1.0-beta.8` shipped without GitHub's green
> **Verified** badge, while `beta.1`–`beta.5` had it. Nothing was wrong with the
> binary — the cause was the **commit the tag pointed at**, not the tag or the
> release job. This doc records the failure and the proper flow so it does not
> recur.

## What the "Verified" badge actually means

The green **Verified** badge on a release is GitHub showing the **signature of the
commit the tag points at** — not a property of the tag or the release. Our release
jobs create the tag with `softprops/action-gh-release`, which makes a **lightweight
tag** (a ref to a commit). A lightweight tag has no signature of its own, so the
badge mirrors the **target commit's** verification status.

A commit is `verified` when it carries a signature GitHub trusts. Two ways that
happens here:

1. **GitHub web-flow signature (the common one).** Any commit **created on
   github.com** — a PR **squash/merge**, a "Merge pull request" commit, or a web
   edit — is signed automatically by GitHub's web-flow GPG key
   (`B5690EEEBB952194`, "created on GitHub.com and signed with GitHub's verified
   signature"). No local key needed.
2. **Your own signed commit.** A commit you make locally with `git commit -S`
   (GPG or SSH signing), with the public key registered on your GitHub account.

A commit you author locally **without** `-S` and `git push` **straight to the
branch** is `unsigned` → its release shows **no badge**.

## The failure (beta.8)

`beta.8`'s tag pointed at `f1c2cf5`, committed locally and pushed **directly** to
`release/0.1.0` (a hotfix-style push, no PR). It was `unsigned`, so no badge. The
betas that *did* have the badge landed on **PR-merge commits**, which github.com
web-flow-signs automatically.

```
commit    landed via                         verified   →  beta badge
f1c2cf5   direct push to release/0.1.0        unsigned      ❌ (beta.8)
ab954d6   direct push to release/0.1.0        unsigned      ❌
85ff4ce   PR #23 merge (github.com)           valid         ✅
41eb8a9   PR #22 merge (github.com)           valid         ✅
9ae819a   PR #19 merge (github.com)           valid         ✅
```

Check any commit's status:

```bash
gh api repos/ScoutSportTechnology/sst-cam-firmware/commits/<sha> \
  --jq '.commit.verification | {verified, reason}'
```

## Proper flow — keep release commits signed

**Never push releasable commits directly to `development` or `release/**`.** Land
every change through a **PR merged on github.com** so the merge/squash commit is
web-flow-signed. This is already the intended branch model
(`feat/* → development → release/X.Y.Z → main`) — the badge is a free side effect
of following it.

- **Beta fixes** iterate on the release branch: open a PR `fix/... → release/X.Y.Z`
  and use the **Merge/Squash** button. Each merge commit is signed → each
  `-beta.N` cut from it is Verified.
- **Cutting the branch** (`git switch -c release/X.Y.Z development`) points the
  first beta at `development`'s tip, which is itself a signed PR-merge commit — so
  `-beta.1` is Verified.
- **If you must hotfix locally** (rare), sign the commit so it still verifies:

  ```bash
  git config commit.gpgsign true          # or per-commit: git commit -S
  # one-time: register the GPG/SSH public key at github.com → Settings → SSH and GPG keys
  ```

  An SSH signing key is the lightest path:

  ```bash
  git config gpg.format ssh
  git config user.signingkey ~/.ssh/id_ed25519.pub
  git config commit.gpgsign true
  ```

## Not a binary-integrity control

The Verified badge is **provenance of the commit**, independent of binary
integrity. Asset integrity rides on the `sha256: <hex>` hand-off recorded in the
beta release notes, which `release.yml` recomputes and verifies before promoting
to stable (see [rulesets.md](rulesets.md) and the workflow headers). `beta.8`'s
recorded digest matched its binary exactly — promotion was never at risk. Treat a
missing badge as a **process smell (a release commit bypassed PR review)**, not a
security failure.
