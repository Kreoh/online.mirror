#!/usr/bin/env bash
set -eu

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"

python3 "$repo_root/docker/from-source/debrand.py" self-test
grep -Fqx -- '--with-product-name=Online Office' \
  "$repo_root/engine/distro-configs/OnlineLinuxCommon.conf"
grep -Fq 'online-office-sbom.spdx.json' "$repo_root/Makefile.am"
grep -Fq '"name": "Online Office"' "$repo_root/cool-sbom-template.spdx.json"
grep -Fq "IMAGE_BASE_NAME = 'ghcr.io/kreoh/online-office'" "$repo_root/Jenkinsfile"
grep -Fq 'ONLINE_OFFICE_SOURCE_REVISION' "$repo_root/Jenkinsfile"
grep -Fq 'ONLINE_OFFICE_SOURCE_REVISION' "$repo_root/docker/from-source/build.sh"
grep -Fq 'ARG ONLINE_OFFICE_SOURCE_REVISION' "$repo_root/docker/from-source/Debian"

echo "Online Office pipeline debranding checks passed."
