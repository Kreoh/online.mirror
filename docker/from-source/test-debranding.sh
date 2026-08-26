#!/usr/bin/env bash
set -eu

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"

python3 "$repo_root/docker/from-source/debrand.py" self-test
grep -Fqx -- '--with-product-name=Online Office' \
  "$repo_root/engine/distro-configs/OnlineLinuxCommon.conf"
grep -Fq 'online-office-sbom.spdx.json' "$repo_root/Makefile.am"
grep -Fq 'dist_doc_DATA = COPYING' "$repo_root/Makefile.am"
grep -Fq '"name": "Online Office"' "$repo_root/cool-sbom-template.spdx.json"
grep -Fq "IMAGE_BASE_NAME = 'ghcr.io/kreoh/online-office'" "$repo_root/Jenkinsfile"
grep -Fq 'ONLINE_OFFICE_SOURCE_REVISION' "$repo_root/Jenkinsfile"
grep -Fq 'ONLINE_OFFICE_SOURCE_REVISION' "$repo_root/docker/from-source/build.sh"
grep -Fq 'ARG ONLINE_OFFICE_SOURCE_REVISION' "$repo_root/docker/from-source/Distroless"

po_validation_line="$(grep -nF 'debrand.py" validate-po' \
  "$repo_root/docker/from-source/build.sh" | cut -d: -f1)"
source_scan_line="$(grep -nF 'debrand.py" scan-source' \
  "$repo_root/docker/from-source/build.sh" | cut -d: -f1)"
engine_autogen_line="$(grep -nF 'cd online/engine && ./autogen.sh' \
  "$repo_root/docker/from-source/build.sh" | cut -d: -f1)"
test "$source_scan_line" -lt "$engine_autogen_line"
test "$po_validation_line" -lt "$engine_autogen_line"

echo "Online Office pipeline debranding checks passed."
