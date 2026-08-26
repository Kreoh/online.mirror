#!/usr/bin/env bash
set -eu

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"

python3 "$repo_root/docker/from-source/debrand.py" self-test
python3 - "$repo_root/docker/from-source/debrand.py" <<'PY'
import importlib.util
import inspect
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

module_path = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("debrand", module_path)
assert spec is not None and spec.loader is not None
debrand = importlib.util.module_from_spec(spec)
spec.loader.exec_module(debrand)

scan_image_source = inspect.getsource(debrand.scan_image)
expected_extraction_routes = """\
            if distroless:
                extract_container_rootfs(container, rootfs)
            else:
                subprocess.run(
                    ["docker", "cp", f"{container}:/.", str(rootfs)],
                    check=True,
                )
"""
assert expected_extraction_routes in scan_image_source

with tempfile.TemporaryDirectory() as directory:
    fixture = Path(directory)
    fake_docker = fixture / "docker"
    fake_docker.write_text(
        """#!/usr/bin/env python3
import io
import os
import sys
import tarfile

if sys.argv[1:] != ["cp", "fixture-container:/.", "-"]:
    raise SystemExit(64)
failure = os.environ.get("FAKE_DOCKER_FAILURE")

with tarfile.open(fileobj=sys.stdout.buffer, mode="w|") as archive:
    directory = tarfile.TarInfo("etc/coolwsd")
    directory.type = tarfile.DIRTYPE
    directory.mode = 0o555
    directory.uid = 12345
    directory.gid = 12345
    archive.addfile(directory)

    payload = b"neutral runtime configuration\\n"
    config = tarfile.TarInfo("etc/coolwsd/coolkitconfig.xcu")
    config.mode = 0o400
    config.uid = 12345
    config.gid = 12345
    config.size = len(payload)
    archive.addfile(config, io.BytesIO(payload))
if failure:
    raise SystemExit(23)
""",
        encoding="utf-8",
    )
    fake_docker.chmod(fake_docker.stat().st_mode | stat.S_IXUSR)
    os.environ["PATH"] = f"{fixture}{os.pathsep}{os.environ['PATH']}"

    rootfs = fixture / "rootfs"
    rootfs.mkdir()
    debrand.extract_container_rootfs("fixture-container", rootfs)
    config = rootfs / "etc/coolwsd/coolkitconfig.xcu"
    assert config.read_text(encoding="utf-8") == "neutral runtime configuration\n"
    assert config.stat().st_uid == os.getuid()
    assert (rootfs / "etc/coolwsd").stat().st_uid == os.getuid()

    os.environ["FAKE_DOCKER_FAILURE"] = "1"
    failed_rootfs = fixture / "failed-rootfs"
    failed_rootfs.mkdir()
    try:
        debrand.extract_container_rootfs("fixture-container", failed_rootfs)
    except subprocess.CalledProcessError as error:
        assert error.returncode == 23
        assert error.cmd[:2] == ["docker", "cp"]
    else:
        raise AssertionError("Docker archive failure was not propagated")

    del os.environ["FAKE_DOCKER_FAILURE"]
    fake_tar = fixture / "tar"
    fake_tar.write_text("#!/bin/sh\ncat >/dev/null\nexit 29\n", encoding="utf-8")
    fake_tar.chmod(fake_tar.stat().st_mode | stat.S_IXUSR)
    try:
        debrand.extract_container_rootfs("fixture-container", failed_rootfs)
    except subprocess.CalledProcessError as error:
        assert error.returncode == 29
        assert error.cmd[0] == "tar"
    else:
        raise AssertionError("tar extraction failure was not propagated")
PY
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
