#!/bin/sh
# Publish out/* as release $TAG on GitHub and Forgejo, creating the release
# if it doesn't exist yet, with the top debian/changelog entry as its notes.
# Needs curl and jq, GITHUB_TOKEN and FORGEJO_TOKEN.
# ponytail: an asset that's already on a release makes its upload fail; delete
# the release on that forge and re-run the tag build to replace it.
set -eu
: "${TAG:?}" "${GITHUB_TOKEN:?}" "${FORGEJO_TOKEN:?}"
GH=https://api.github.com/repos/yeold/gork
FJ=https://git.yeold.org/api/v1/repos/yeold/gork
DIR=${1:-out}

gh() { curl -fsS -H "Authorization: Bearer $GITHUB_TOKEN" -H 'Accept: application/vnd.github+json' "$@"; }
fj() { curl -fsS -H "Authorization: token $FORGEJO_TOKEN" "$@"; }

# Release notes: the top debian/changelog entry, bullets as Markdown.
notes=$(awk 'NR <= 2 { next } /^ -- / { exit } { print }' debian/changelog |
        sed 's/^  \* /- /; s/^    /  /')
create() { jq -nc --arg tag "$TAG" --arg body "$notes" \
    "{tag_name: \$tag, name: (\"gork \" + \$tag), body: \$body} $1"; }

# release_id gh|fj API CREATE_JSON: id of release $TAG, created if missing.
release_id() {
    r=$($1 "$2/releases/tags/$TAG" 2>/dev/null) ||
        r=$($1 -X POST -H 'Content-Type: application/json' "$2/releases" -d "$3")
    echo "$r" | jq -r .id
}

# The tag lives on GitHub, which Jenkins builds from.  Forgejo's history has
# different commit ids, so its release tags the head of master instead.
id=$(release_id gh "$GH" "$(create '')")
for f in "$DIR"/*; do
    echo "github: $(basename "$f")"
    gh -H 'Content-Type: application/octet-stream' --data-binary @"$f" \
        "https://uploads.github.com/repos/yeold/gork/releases/$id/assets?name=$(basename "$f")" >/dev/null
done

id=$(release_id fj "$FJ" "$(create '+ {target_commitish: "master"}')")
for f in "$DIR"/*; do
    echo "forgejo: $(basename "$f")"
    fj -F "attachment=@$f" "$FJ/releases/$id/assets?name=$(basename "$f")" >/dev/null
done

# Forgejo package registry: apt and dnf repos, plus the tarball as a generic
# package.  Built on debian:stable, so the debs go in the "stable" suite.
P=https://git.yeold.org/api/packages/yeold
up() { echo "registry: $(basename "$2")"; curl -fsS -u "yeold:$FORGEJO_TOKEN" -T "$2" "$1" >/dev/null; }
for f in "$DIR"/*.deb; do up "$P/debian/pool/stable/main/upload" "$f"; done
for f in "$DIR"/*.rpm; do case "$f" in *.src.rpm) ;; *) up "$P/rpm/upload" "$f";; esac; done
for f in "$DIR"/gork-*.tar.gz; do up "$P/generic/gork/${TAG#v}/$(basename "$f")" "$f"; done
