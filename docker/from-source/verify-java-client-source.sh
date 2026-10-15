#!/usr/bin/env bash
# Compare cached inputs with pinned source and its exact image-build transformation.
set -euo pipefail
# Diff must not refresh the real index as a side effect of validation.
export GIT_OPTIONAL_LOCKS=0
source_root=$1
revision=$2
patch_output=$3
fail() { echo "Java client source validation: $*" >&2; exit 1; }
inputs=(engine/codemaker engine/unoidl engine/sal engine/salhelper engine/registry
    engine/store engine/include engine/solenv engine/external engine/ridljar
    engine/udkapi engine/offapi engine/Makefile.gbuild engine/RepositoryModule_host.mk
    engine/RepositoryExternal.mk engine/Repository.mk engine/RepositoryFixes.mk engine/configure.ac)
[[ $(git -C "$source_root" rev-parse HEAD) == "$revision" ]] || fail 'cache revision differs from selected source'
# Inspect both staged and unstaged differences against the pinned commit.
changes=$(mktemp)
expected=$(mktemp -d)
trap 'rm -f "$changes"; rm -rf "$expected"' EXIT
# Some Git versions refresh index extensions even for a read-only diff. Work on
# a copy throughout, preserving staged entries without touching the cache index.
cache_index=$(git -C "$source_root" rev-parse --path-format=absolute --git-path index)
cp "$cache_index" "$expected/cache-index"
export GIT_INDEX_FILE="$expected/cache-index"
git -C "$source_root" diff --name-only -z "$revision" -- "${inputs[@]}" > "$changes"
mapfile -d '' -t paths < "$changes"
: > "$patch_output"
if [[ ${#paths[@]} == 0 ]]; then exit 0; fi
for path in "${paths[@]}"; do
    mode=$(git -C "$source_root" ls-tree "$revision" -- "$path" | cut -d ' ' -f1)
    case "$mode" in
        100644) [[ -f $source_root/$path && ! -L $source_root/$path && ! -x $source_root/$path ]] || fail "unexpected file type or mode: $path" ;;
        100755) [[ -f $source_root/$path && ! -L $source_root/$path && -x $source_root/$path ]] || fail "unexpected file type or mode: $path" ;;
        *) fail "added, deleted or unsupported source path: $path" ;;
    esac
done
# The temporary tree and index contain only changed inputs and the existing
# transform's required favicon. Neither the real index nor cache files are edited.
paths+=(docker/from-source/debrand.py engine/static/emscripten/favicon.ico)
git -C "$source_root" archive "$revision" -- "${paths[@]}" | tar -xf - -C "$expected"
git_dir=$(git -C "$source_root" rev-parse --absolute-git-dir)
(
    export GIT_DIR="$git_dir" GIT_INDEX_FILE="$expected/index" GIT_WORK_TREE="$expected"
    git read-tree --empty
    git -C "$source_root" ls-tree -rz "$revision" -- "${paths[@]}" | git update-index -z --index-info
    # Run the existing pinned build helper, never a rewritten approximation.
    python3 "$expected/docker/from-source/debrand.py" source "$expected"
)
invalid=0
while IFS= read -r -d '' path; do
    if ! cmp -s "$expected/$path" "$source_root/$path"; then
        printf 'Unexpected cached generator source: %s\n' "$path" >&2
        invalid=1
    fi
done < "$changes"
[[ $invalid == 0 ]] || fail 'inputs differ from both pinned source and its exact debranding transformation; cache left unchanged'
git -C "$source_root" diff --binary "$revision" -- "${inputs[@]}" > "$patch_output"
printf 'Verified %s transformed generator source paths against pinned debranding.\n' "$(tr -cd '\0' < "$changes" | wc -c)"
