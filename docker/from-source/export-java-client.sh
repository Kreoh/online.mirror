#!/usr/bin/env bash
# Run by Jenkins only. The existing configured cache is a required input.
set -euo pipefail
fail() { echo "Java client export: $*" >&2; exit 1; }
[[ ${BUILD_URL:-} && ${PIPELINE_REVISION:-} ]] || fail 'Jenkins build identity is required'
[[ ${CLIENT_SOURCE_REVISION:-} =~ ^[0-9a-f]{40}$ ]] || fail 'supply a full target source revision'
[[ ${CLIENT_IMAGE:-} ]] || fail 'supply the target image'
if [[ ${BUILD_MODE:-} == client-only ]]; then
    [[ $CLIENT_IMAGE =~ ^ghcr.io/kreoh/online-office@sha256:[0-9a-f]{64}$ ]] || fail 'client-only requires an immutable image digest'
fi
image_id=$(docker image inspect --format '{{.Id}}' "$CLIENT_IMAGE")
revision=$(docker image inspect --format '{{ index .Config.Labels "org.opencontainers.image.revision" }}' "$CLIENT_IMAGE")
[[ $revision == "$CLIENT_SOURCE_REVISION" ]] || fail 'image revision differs from target source'
cache_root="${ONLINE_OFFICE_SOURCE_CACHE_ROOT:-$(dirname "$PWD")/.online-office-source-cache}"
cache="$cache_root/main"
[[ -f $cache/build/builddir/online/engine/config_host.mk ]] || fail "missing configured cache file: $cache/build/builddir/online/engine/config_host.mk; restore the matching cache on Jenkins, no full engine fallback is permitted"
output="$PWD/uno-client-export-$BUILD_NUMBER"
mkdir "$output"
mkdir "$output/registries"
container=$(docker create "$image_id")
trap 'docker rm "$container" >/dev/null' EXIT
docker cp "$container:/opt/online-office/program/types.rdb" "$output/registries/types.rdb"
docker cp "$container:/opt/online-office/program/types/offapi.rdb" "$output/registries/offapi.rdb"
mkdir "$output/image-program"
docker cp "$container:/opt/online-office/program/." "$output/image-program/"
docker rm "$container" >/dev/null
trap - EXIT
builder='ghcr.io/kreoh/online-office:source-builder'
builder_id=$(docker image inspect --format '{{.Id}}' "$builder")
export CLIENT_IMAGE_ID="$image_id" CLIENT_BUILDER_ID="$builder_id"
docker run --rm --pull never --network none --user "$(id -u):$(id -g)" \
    -v "$PWD:/pipeline:ro" -v "$cache:/workspace-cache" -v "$output:/output" \
    -e CLIENT_SOURCE_REVISION -e CLIENT_IMAGE -e CLIENT_IMAGE_ID -e CLIENT_BUILDER_ID \
    -e PIPELINE_REVISION -e BUILD_URL -e BUILD_NUMBER \
    -e HOME=/workspace-cache/home -e CCACHE_DISABLE=1 \
    --entrypoint bash "$builder_id" /pipeline/docker/from-source/export-java-client-in-container.sh
(cd "$output" && tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -cf uno-client.tar generated runtime-sources.tar registries licences generator PROVENANCE.properties SHA256SUMS)
(cd "$output" && sha256sum uno-client.tar > uno-client.tar.sha256)
