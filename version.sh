#!/bin/sh
# The name of this build, for --version.  In a git checkout it is
# v<version>-beta-<commits>, with -dirty when tracked files are changed;
# but a clean checkout of the release's own tag (v<version>, or $TAG_NAME
# naming it, as Jenkins sets in a tag build, whose checkout may have no
# tags) is the release, and named by its version alone.  So is a build
# out of git: a dist tarball.  Run from the source tree:
#     sh version.sh
ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
base=${ver%%[~-]*}
if [ -e .git ] && git rev-parse --verify HEAD >/dev/null 2>&1; then
    dirty=
    git diff HEAD --quiet 2>/dev/null || dirty=-dirty
    tags=$(git tag --points-at HEAD 2>/dev/null)
    # $TAG_NAME counts only if this checkout has no such tag elsewhere
    if [ -n "${TAG_NAME:-}" ]; then
        t=$(git rev-parse -q --verify "refs/tags/$TAG_NAME^{commit}" 2>/dev/null)
        [ -z "$t" ] || [ "$t" = "$(git rev-parse HEAD)" ] && tags="$tags
$TAG_NAME"
    fi
    if [ -z "$dirty" ] && printf '%s\n' "$tags" | grep -q "^v$base\(-.*\)\{0,1\}$"; then
        echo "$ver"
    else
        echo "v$base-beta-$(git rev-list HEAD | wc -l | tr -d ' 	')$dirty"
    fi
else
    echo "$ver"
fi
