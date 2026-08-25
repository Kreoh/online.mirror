#!/bin/sh
set -eu

test "$#" -eq 1 || {
    echo 'usage: report-image-size.sh IMAGE' >&2
    exit 1
}

image=$1
daemon_bytes=$(docker image inspect --format '{{.Size}}' "$image")
case "$daemon_bytes" in *[!0-9]*|'') echo 'Docker returned an invalid image size' >&2; exit 1 ;; esac

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
docker image save -o "$temporary/image.tar" "$image"
script_directory=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
compressed_bytes=$(python3 "$script_directory/measure_saved_image.py" \
    "$temporary/image.tar")
case "$compressed_bytes" in *[!0-9]*|'') echo 'Compressed size calculation failed' >&2; exit 1 ;; esac
uncompressed_bytes=$(python3 "$script_directory/measure_saved_image.py" \
    --uncompressed "$temporary/image.tar")
case "$uncompressed_bytes" in *[!0-9]*|'') echo 'Uncompressed size calculation failed' >&2; exit 1 ;; esac

echo "Docker daemon-reported image bytes: $daemon_bytes"
echo "Uncompressed saved-layer and config bytes: $uncompressed_bytes"
echo "Deterministic gzip-compressed layer and config bytes: $compressed_bytes"
echo 'Image history, newest layer first:'
docker image history --no-trunc --format '{{.Size}} bytes | {{.CreatedBy}}' "$image"

# Disabled by default to avoid failing after the source compilation. Operators
# can set a publication ceiling once representative distroless builds exist.
ceiling=${ONLINE_OFFICE_MAX_COMPRESSED_BYTES:-0}
case "$ceiling" in *[!0-9]*|'') echo 'ONLINE_OFFICE_MAX_COMPRESSED_BYTES must be a byte count' >&2; exit 1 ;; esac
if test "$ceiling" -gt 0 && test "$compressed_bytes" -gt "$ceiling"; then
    echo "Compressed image size $compressed_bytes exceeds configured ceiling $ceiling" >&2
    exit 1
fi
