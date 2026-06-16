#!/usr/bin/env bash
# Floor-NOLINT guard — fails when a PR diff ADDS an unsanctioned per-site
# suppression of a floor check (bugprone-* / performance-*). The floor policy
# (.clang-tidy header) allows such a NOLINT only when the same line also carries
# the sanction marker `// floor-ok: <reason>`. This guard is what makes the
# "floor" promise enforceable instead of aspirational.
#
# DIFF-SCOPED: only lines ADDED relative to the base ref are inspected, so
# pre-existing sanctioned (or grandfathered) suppressions never re-trip the gate
# — only newly introduced ones are. Touching a line near an existing NOLINT
# without adding a new floor NOLINT passes.
#
# Usage:
#   scripts/check-floor-nolints.sh [BASE_REF]
# BASE_REF defaults to origin/main, then main, then the empty tree (whole repo
# treated as added — used when no base is resolvable, e.g. a first commit).
set -euo pipefail

base_ref="${1:-}"
if [ -z "$base_ref" ]; then
    if git rev-parse --verify --quiet origin/main >/dev/null; then
        base_ref="origin/main"
    elif git rev-parse --verify --quiet main >/dev/null; then
        base_ref="main"
    fi
fi

if [ -n "$base_ref" ]; then
    # Three-dot: compare against the merge-base, so unrelated changes already on
    # the base branch don't count as "added" by this PR.
    diff_range="${base_ref}...HEAD"
else
    # No base resolvable — diff against the empty tree (everything is "added").
    diff_range="$(git hash-object -t tree /dev/null)"
fi

# -U0: no context lines, so every `+` line is a genuine addition (not context).
# Restrict to C/C++ sources + headers; that is where floor NOLINTs live.
diff_out="$(git diff --unified=0 "$diff_range" -- \
    '*.cpp' '*.cc' '*.hpp' '*.h' 2>/dev/null || true)"

violations=0
cur_file=""
new_line=0

while IFS= read -r line; do
    case "$line" in
    '+++ b/'*)
        cur_file="${line#+++ b/}"
        ;;
    '@@ '*)
        # @@ -old,cnt +new,cnt @@ — extract the new-file start line.
        hunk="${line#*+}"
        hunk="${hunk%%[ ,]*}"
        new_line="$hunk"
        ;;
    '+'*)
        # An added line (not the +++ header, which is matched above).
        content="${line#+}"
        if printf '%s' "$content" \
            | grep -Eq 'NOLINT(BEGIN|NEXTLINE)?\((bugprone|performance)-'; then
            if ! printf '%s' "$content" | grep -q 'floor-ok:'; then
                printf 'UNSANCTIONED floor NOLINT: %s:%s\n' "$cur_file" "$new_line" >&2
                printf '    %s\n' "$content" >&2
                violations=$((violations + 1))
            fi
        fi
        new_line=$((new_line + 1))
        ;;
    ' '*)
        new_line=$((new_line + 1))
        ;;
    esac
done <<EOF
$diff_out
EOF

if [ "$violations" -gt 0 ]; then
    cat >&2 <<'MSG'

----------------------------------------------------------------------
Floor policy (.clang-tidy): a bugprone-* / performance-* NOLINT is a
FLOOR suppression. It is allowed ONLY with a per-site sanction marker:

    something();  // NOLINT(bugprone-easily-swappable-parameters) // floor-ok: external order

Either remove the suppression (fix the finding), or add a `// floor-ok:
<reason>` marker giving the per-site justification.
----------------------------------------------------------------------
MSG
    exit 1
fi

echo "check-floor-nolints: no new unsanctioned floor NOLINTs (base: ${base_ref:-<empty-tree>})"
