# push-refs.sh — decisions over the ref list git feeds pre-push on stdin.
# Sourced by .githooks/pre-push; POSIX sh, like the hook itself. Kept in its
# own file so tests/fixtures/pre-push-deletion/ can exercise the decision
# without running the hook's build.
#
# git writes one line per ref:  <local ref> <local sha> <remote ref> <remote sha>
# A branch deletion (`git push origin --delete X`) has a local sha of all
# zeros: 40 in a SHA-1 repo, 64 in a SHA-256 one, so match "all zeros" rather
# than a fixed length.

# prepush_deletion_only — reads the ref list on stdin. Succeeds iff there is at
# least one ref and every ref's local sha is all zeros. An empty list fails, so
# a push git describes with no refs still runs every gate, as it did before
# this check existed. Blank lines are not refs. Always drains stdin.
prepush_deletion_only() {
    _pr_refs=0
    _pr_content=0
    while read -r _pr_lref _pr_lsha _pr_rref _pr_rsha || [ -n "${_pr_lref:-}" ]; do
        if [ -n "${_pr_lsha:-}" ]; then
            _pr_refs=$((_pr_refs + 1))
            case "$_pr_lsha" in
                *[!0]*) _pr_content=1 ;;
            esac
        fi
        _pr_lref=""
        _pr_lsha=""
    done
    [ "$_pr_refs" -gt 0 ] && [ "$_pr_content" -eq 0 ]
}
