#!/usr/bin/env bash
# Jenkins only: prove reused engine libraries belong to the selected image.
set -euo pipefail
engine=$1
image_program=$2
output=$3
shift 3
fail() { echo "Java client library validation: $*" >&2; exit 1; }
export LD_LIBRARY_PATH="$engine/instdir/program:$engine/workdir/LinkTarget/Library"
# This export path supports the existing native, dynamically linked merged build.
for setting in 'MERGELIBS=TRUE' 'DISABLE_DYNLOADING='; do
    grep -Fxq "export $setting" "$engine/config_host.mk" || fail "unsupported cached configuration: expected $setting"
done
libraries=("$engine/instdir/program/libmergedlo.so" "$engine/instdir/program/libsallo.so")
for library in "${libraries[@]}"; do
    cmp -s "$library" "$image_program/$(basename "$library")" || fail "cached library differs from exact image or is missing: $library; restore the matching cache, no library rebuild fallback"
done
# Linker aliases must resolve to those same bytes, even if a failed build left a
# different workdir copy. No cached library is repaired or overwritten here.
for name in libmergedlo.so libsallo.so; do
    alias="$engine/workdir/LinkTarget/Library/$name"
    if [[ -e $alias || -L $alias ]]; then
        cmp -s "$alias" "$image_program/$name" || fail "cached linker alias differs from image: $alias"
    fi
done
ldd "${libraries[@]}" "$@" > "$output.loader"
! grep -Fq 'not found' "$output.loader" || fail "missing loader dependency; see $output.loader"
awk '$2 == "=>" && $3 ~ /^\// {print $3} $1 ~ /^\// && $2 ~ /^\(/ {print $1}' "$output.loader" | sort -u > "$output.paths"
: > "$output"
while IFS= read -r library; do
    case "$library" in
        "$engine"/*)
            name=$(basename "$library")
            cmp -s "$library" "$image_program/$name" || fail "cached transitive library differs from image or is absent: $library"
            (cd "$image_program" && sha256sum "$name") >> "$output"
            ;;
    esac
done < "$output.paths"
for library in "${libraries[@]}"; do
    (cd "$image_program" && sha256sum "$(basename "$library")") >> "$output"
done
sort -u -o "$output" "$output"
rm "$output.paths"
