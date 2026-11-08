#!/usr/bin/env bash
# List every <plus> hook in official viewer files, with the lines between the
# markers, so an upstream merge can be checked quickly.
#
#   C++ / CMake / Python:  <plus> ... </plus>   (in a comment: "// <plus>", "# <plus>",
#                          "/* <plus>"; a one-line hook may end "... </plus>")
#   XML:                   <!-- plus ... -->  ...  <!-- /plus -->
#
# Then checks that the translations of the XUI files we change still apply
# (xui-check.py, next to this script; needs python and upstream/develop).
#
# Usage: indra/newview/plus/plus-hooks.sh [--brief]
#   --brief   only file:line of each opening marker
# Exit status is 1 if a marker is left unclosed, opened inside another, closed
# without one open, no hook is found, or a translation no longer applies.

set -eu
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

brief=0
[ "${1:-}" = "--brief" ] && brief=1

# Our own files (indra/newview/plus/) carry no hooks; skip them in case a
# comment there mentions a marker.
files=$(git grep -l -E '<plus>|<!-- plus' -- ':!indra/newview/plus/' || true)
if [ -z "$files" ]; then
    echo "no <plus> hooks found"
    exit 1
fi

status=0
# shellcheck disable=SC2086
# A marker counts only right after the comment's start (so a comment that names
# "<plus>" in its text is not one). A hook may close on its own line
# ("// <plus> ... </plus>"); a marker opened while another is open is reported.
awk -v brief="$brief" '
    function unclosed() { if (open) { print prev ":" open ": marker is not closed"; bad = 1 } }
    FNR == 1 { unclosed(); open = 0; prev = FILENAME; nfiles++ }
    match($0, /(\/\/|#|\/\*|<!--)[ \t]*<plus>|<!-- plus/) {
        rest = substr($0, RSTART + RLENGTH)
        if (open) { print FILENAME ":" FNR ": opens inside the marker opened at line " open; bad = 1 }
        hooks++
        print FILENAME ":" FNR ": " $0
        if (index(rest, "</plus>") || index(rest, "<!-- /plus -->")) {
            open = 0            # closed on the same line
            if (!brief) print ""
        } else {
            open = FNR
        }
        next
    }
    /(\/\/|#|\/\*|<!--)[ \t]*<\/plus>|<!-- \/plus -->/ {
        if (!open) { print FILENAME ":" FNR ": closing marker without opening one"; bad = 1 }
        open = 0
        if (!brief) print ""
        next
    }
    open && !brief { print "    " $0 }
    END {
        unclosed()
        print "== " hooks " hook(s) in " nfiles " file(s)"
        exit bad
    }
' $files || status=1

if command -v python >/dev/null 2>&1; then
    python -I indra/newview/plus/xui-check.py || status=1
else
    echo "xui-check: no python; translations not checked"
fi
exit $status
