#!/bin/sh
# Publishes a release to the public GitHub repository (called by CI on "release: published").
#
# The public repo does not mirror the private history: every release becomes ONE commit whose
# tree is this release, on top of the previous public release. Before anything leaves the
# runner the whole tree (and the release text) is checked against PRIVACY_PATTERNS.
#
# env: GH_TOKEN (fine-grained token, Contents: read and write, only this repo), GH_REPO (owner/name),
#      PATTERNS (= PRIVACY_PATTERNS), TAG, PRERELEASE (true|false), RELEASE_TITLE, RELEASE_BODY,
#      OUT_DIR (firmware of every board <tag>-<board>[-merged].bin and the signed manifest*.json/.sig)
set -eu

if [ -z "${GH_TOKEN:-}" ] || [ -z "${GH_REPO:-}" ]; then
  echo "::warning::GH_TOKEN or GH_REPO is not set: not published to GitHub"
  exit 0
fi
if [ -z "${PATTERNS:-}" ]; then
  echo "::error::PRIVACY_PATTERNS is not set: refusing to publish"
  exit 1
fi

# 1. nothing private in the tree or in the release text (only file:line is printed)
hits=$(git grep -nIiE -- "$PATTERNS" HEAD | cut -d: -f2,3 || true)
if [ -n "$hits" ]; then
  echo "Private data found, nothing published:"
  echo "$hits"
  exit 1
fi
if printf '%s\n%s\n' "${RELEASE_TITLE:-}" "${RELEASE_BODY:-}" | grep -qiE -- "$PATTERNS"; then
  echo "Private data in the release title or text, nothing published"
  exit 1
fi

auth="AUTHORIZATION: basic $(printf 'x-access-token:%s' "$GH_TOKEN" | base64 -w0)"
remote="https://github.com/${GH_REPO}.git"
gitgh() { git -c http.extraheader="$auth" "$@"; }

# 2. one commit for this release on top of the previous public one (none for the first)
git config user.name "SenkoOS CI"
git config user.email "ci@senkoos.invalid"
parent=""
if gitgh ls-remote --exit-code "$remote" refs/heads/main >/dev/null 2>&1; then
  gitgh fetch --quiet --depth=1 "$remote" main
  parent="-p $(git rev-parse FETCH_HEAD)"
fi
# public tree = this release without .github/: the upstream GitHub workflows are not needed there
# (Actions are off, CI is Forgejo), and pushing workflow files would need a token with "workflow" scope
export GIT_INDEX_FILE="$(mktemp -u)"   # path only: git refuses an existing empty index file
git read-tree HEAD
git rm -r -q --cached --ignore-unmatch .github
# the public front page is diy/github/README.md; MeshCore's own README is kept as README.MeshCore.md
if [ -f diy/github/README.md ]; then
  git update-index --add --cacheinfo "100644,$(git rev-parse HEAD:README.md),README.MeshCore.md"
  git update-index --add --cacheinfo "100644,$(git hash-object -w diy/github/README.md),README.md"
fi
tree=$(git write-tree)
rm -f "$GIT_INDEX_FILE"; unset GIT_INDEX_FILE
msg=$(printf 'SenkoOS %s\n\nMeshCore repeater with a web panel: SenkoPhone (ESP32 WROOM-32 + E22-900M30S), Heltec WiFi LoRa 32 V3 / V4.\nhttps://github.com/meshcore-dev/MeshCore' "$TAG")
commit=$(echo "$msg" | git commit-tree "$tree" $parent)
gitgh push --quiet "$remote" "$commit:refs/heads/main" "$commit:refs/tags/$TAG"
echo "pushed $commit as main and $TAG"

# 3. GitHub release with the firmware files of every board and the update manifests
api="https://api.github.com/repos/${GH_REPO}"
hdr_auth="Authorization: Bearer $GH_TOKEN"
body=$(python3 -c 'import json,os; print(json.dumps({"tag_name": os.environ["TAG"], "name": os.environ.get("RELEASE_TITLE") or os.environ["TAG"], "body": os.environ.get("RELEASE_BODY",""), "prerelease": os.environ.get("PRERELEASE")=="true"}))')
rel=$(curl -fsS -X POST -H "$hdr_auth" -H "Accept: application/vnd.github+json" "$api/releases" -d "$body")
rel_id=$(printf '%s' "$rel" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
up="https://uploads.github.com/repos/${GH_REPO}/releases/${rel_id}/assets"
upload() {
  [ -f "$1" ] || { echo "skip $2 (no file)"; return 0; }
  curl -fsS -X POST -H "$hdr_auth" -H "Content-Type: application/octet-stream" --data-binary "@$1" "$up?name=$2" >/dev/null
  echo "uploaded $2"
}
for f in "$OUT_DIR"/*.bin "$OUT_DIR"/manifest*.json "$OUT_DIR"/manifest*.sig; do
  upload "$f" "${f##*/}"
done
echo "GitHub release $TAG done"

# 4. rolling pre-release "beta-channel": the manifests of the newest release of ANY kind, for boards
#    with "update beta on" (GitHub's latest/download skips pre-releases)
if [ -f "$OUT_DIR/manifest.json" ] && [ -f "$OUT_DIR/manifest.sig" ]; then
  if ch=$(curl -fsS -H "$hdr_auth" -H "Accept: application/vnd.github+json" "$api/releases/tags/beta-channel" 2>/dev/null); then :; else
    ch=$(curl -fsS -X POST -H "$hdr_auth" -H "Accept: application/vnd.github+json" "$api/releases"       -d '{"tag_name":"beta-channel","target_commitish":"main","name":"beta-channel (update manifest)","body":"Manifest of the newest release, beta included. Used by boards with: update beta on","prerelease":true}')
  fi
  ch_id=$(printf '%s' "$ch" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
  # replace the old manifest files (manifest.json/.sig and manifest-<board>.json/.sig)
  printf '%s' "$ch" | python3 -c 'import json,sys; [print(a["id"]) for a in json.load(sys.stdin).get("assets",[]) if a["name"].startswith("manifest")]' |
    while read -r aid; do curl -fsS -X DELETE -H "$hdr_auth" "$api/releases/assets/$aid"; done
  up="https://uploads.github.com/repos/${GH_REPO}/releases/${ch_id}/assets"
  for f in "$OUT_DIR"/manifest*.json "$OUT_DIR"/manifest*.sig; do
    upload "$f" "${f##*/}"
  done
  echo "beta-channel manifests -> $TAG"
fi
