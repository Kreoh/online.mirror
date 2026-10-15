#!/usr/bin/env bash
# Read an existing stopped image container; never restore archive permissions.
set -euo pipefail
container=$1
destination=$2
fail() { echo "Java client library extraction: $*" >&2; exit 1; }
# A fresh flat directory prevents an existing symlink or file from redirecting output.
umask 077
mkdir "$destination"
inventory=$(mktemp)
trap 'rm -f "$inventory"' EXIT
# Stream directory metadata only. Do not extract its read-only directories/assets.
docker cp "$container:/opt/online-office/program/." - | tar -tf - |
    LC_ALL=C awk '/^\.\/[A-Za-z0-9_+.-]+\.so(\.[0-9]+)*$/ {sub(/^\.\//, ""); print}' > "$inventory"
[[ -s $inventory ]] || fail 'no top-level shared libraries found in image'
[[ $(sort -u "$inventory" | wc -l) == $(wc -l < "$inventory") ]] || fail 'duplicate image library paths'
extracted=0
placeholders=0
while IFS= read -r name; do
    # Docker resolves source aliases; tar writes bytes to stdout, never paths,
    # ownership, modes or links. Redirection creates a caller-owned regular file.
    (set -o noclobber
        docker cp -L "$container:/opt/online-office/program/$name" - |
            tar -xOf - -- "$name" > "$destination/$name")
    magic=$(od -An -tx1 -N4 "$destination/$name" | tr -d ' \n')
    if [[ $magic != 7f454c46 ]]; then
        # gbuild deliberately emits this marker for libraries folded into merged.
        # It is not an ELF input and must not be supplied to the library verifier.
        if printf 'invalid - merged lib\n' | cmp -s - "$destination/$name"; then
            rm "$destination/$name"
            placeholders=$((placeholders + 1))
            continue
        fi
        fail "unexpected non-ELF shared-library entry: $name"
    fi
    extracted=$((extracted + 1))
done < "$inventory"
for name in libmergedlo.so libsallo.so; do
    [[ -f $destination/$name ]] || fail "required image library is missing: $name"
done
printf 'Extracted %s image ELF libraries as caller-owned files; skipped %s gbuild merged-library markers.\n' "$extracted" "$placeholders"
