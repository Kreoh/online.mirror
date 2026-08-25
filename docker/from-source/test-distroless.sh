#!/usr/bin/env bash
set -eu

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
dockerfile="$repo_root/docker/from-source/Distroless"

python3 - "$dockerfile" <<'PY'
import re
import sys
from pathlib import Path

dockerfile = Path(sys.argv[1])
text = dockerfile.read_text(encoding="utf-8")

digest = "sha256:95e6d88e56ea0d74cd2cd126429d764efb3981c2f0e8c7bb2ae801291f20c36f"
assert f"zendis/base@{digest}" in text
assert "AS target-base" in text
assert "AS verify-caps" in text
assert "verify-source-rootfs --staged" in text
assert "cap_chown,cap_fowner,cap_sys_chroot=ep" in text
assert "cap_sys_admin=ep" in text
assert "USER 100" in text
assert '["/usr/bin/coolwsd", "--probe", "--use-env-vars"]' in text
assert "fonts-noto-cjk" not in text
assert "fonts-wqy" not in text
# Python is build-time verification tooling in the assembler and is rejected
# from the assembled rootfs by verify-source-rootfs.
assert "python3" in text.lower()
assert "java" not in text.lower()
assert "chown -R" not in text

stages: list[tuple[str, list[str]]] = []
logical: list[str] = []
current = ""
for raw in text.splitlines():
    stripped = raw.strip()
    if not stripped or stripped.startswith("#"):
        continue
    current = f"{current} {stripped}".strip()
    if current.endswith("\\"):
        current = current[:-1].rstrip()
        continue
    logical.append(current)
    current = ""
assert not current

for instruction in logical:
    match = re.match(r"FROM\s+\S+(?:\s+AS\s+(\S+))?$", instruction, re.I)
    if match:
        stages.append((match.group(1) or "final", []))
    elif stages:
        stages[-1][1].append(instruction)

final_name, final_instructions = stages[-1]
assert final_name == "final"
filesystem = [
    item for item in final_instructions if re.match(r"(?:ADD|COPY|RUN)\s", item, re.I)
]
assert filesystem == ["COPY --from=verify-caps /verified-rootfs /"]
PY

grep -Fq 'FINAL_DOCKERFILE="${ONLINE_OFFICE_FINAL_DOCKERFILE:-Distroless}"' \
    "$repo_root/docker/from-source/build.sh"
grep -Fq 'ONLINE_OFFICE_FINAL_DOCKERFILE=Distroless' "$repo_root/Jenkinsfile"
grep -Fq 'DOCKER_BUILDKIT=1' "$repo_root/Jenkinsfile"
grep -Fq 'CapabilityProbe.Dockerfile' "$repo_root/Jenkinsfile"
! grep -Fq -- '--entrypoint /bin/sh' "$repo_root/Jenkinsfile"
grep -Fq 'io.kreoh.online-office.runtime' "$repo_root/Jenkinsfile"
grep -Fq 'scan-distroless-image' "$repo_root/docker/from-source/build.sh"
grep -Fq 'scan-image' "$repo_root/docker/from-source/build.sh"
grep -Fq 'RuntimeVerifier.Dockerfile' "$repo_root/docker/from-source/build.sh"
grep -Fq 'verify-running-image.sh' "$repo_root/docker/from-source/build.sh"
grep -Fq 'COPY --from=runtime / /merged-rootfs' \
    "$repo_root/docker/from-source/RuntimeVerifier.Dockerfile"
grep -Fq 'verify-source-rootfs --merged /merged-rootfs' \
    "$repo_root/docker/from-source/RuntimeVerifier.Dockerfile"
grep -Fq -- '--cap-drop ALL' \
    "$repo_root/docker/from-source/verify-running-image.sh"
grep -Fq -- '--cap-add SYS_ADMIN' \
    "$repo_root/docker/from-source/verify-running-image.sh"
grep -Fq 'running\|healthy' \
    "$repo_root/docker/from-source/verify-running-image.sh"
grep -Fqx '/usr/bin/coolwsd|libpam.so.0' \
    "$repo_root/docker/from-source/runtime-dynamic-dependencies.conf"
for helper in coolconfig coolconvert coolstress coolwsd-systemplate-setup
do
    grep -Fq "usr/bin/$helper" \
        "$repo_root/docker/from-source/verify-source-rootfs.sh"
done
grep -Fq 'external system font leaked' \
    "$repo_root/docker/from-source/verify-source-rootfs.sh"
grep -Fq 'Uncompressed saved-layer and config bytes' \
    "$repo_root/docker/from-source/report-image-size.sh"
grep -Fq 'Deterministic gzip-compressed layer and config bytes' \
    "$repo_root/docker/from-source/report-image-size.sh"
grep -Fqx -- '--disable-python' \
    "$repo_root/engine/distro-configs/OnlineLinux-LOKit.conf"
grep -Fqx -- '--without-java' \
    "$repo_root/engine/distro-configs/OnlineLinux-LOKit.conf"
! grep -Eq -- '--enable-python|--with-python' \
    "$repo_root/engine/distro-configs/OnlineLinux-LOKit.conf"

fixture="$(mktemp -d)"
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/root/opt/online-office/program" "$fixture/target/usr/lib"
: > "$fixture/registry"
cat > "$fixture/libfixture.c" <<'EOF'
int fixture_value(void) { return 42; }
EOF
cat > "$fixture/app.c" <<'EOF'
extern int fixture_value(void);
int main(void) { return fixture_value() != 42; }
EOF
gcc -fPIC -shared -Wl,-soname,libfixture.so.1 \
    -o "$fixture/root/opt/online-office/program/libfixture.so.1" \
    "$fixture/libfixture.c"
gcc -o "$fixture/root/opt/online-office/program/fixture-app" \
    "$fixture/app.c" \
    -L"$fixture/root/opt/online-office/program" \
    -Wl,-rpath,'$ORIGIN' -l:libfixture.so.1

for binary in \
    "$fixture/root/opt/online-office/program/fixture-app" \
    "$fixture/root/opt/online-office/program/libfixture.so.1"
do
    ldd "$binary" | awk '/=> \/.+ \(/ { print $3 } /^[[:space:]]*\/.+ \(/ { print $1 }' |
        while IFS= read -r library; do
            test "$(basename "$library")" = libfixture.so.1 && continue
            cp -L "$library" "$fixture/target/usr/lib/$(basename "$library")"
        done
done
interpreter="$(readelf -l "$fixture/root/opt/online-office/program/fixture-app" |
    sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p')"
mkdir -p "$fixture/target$(dirname "$interpreter")"
cp -L "$interpreter" "$fixture/target$interpreter"

sh "$repo_root/docker/from-source/verify-elf-closure.sh" \
    "$fixture/root" "$fixture/target" "$fixture/registry"

# Registry records describe libraries loaded dynamically rather than declaring
# another DT_NEEDED edge. The registered provider must still be reachable from
# the requester's loader search path and match its ELF architecture.
gcc -fPIC -shared -Wl,-soname,libfixture-dlopen.so.1 \
    -o "$fixture/root/opt/online-office/program/libfixture-dlopen.so.1" \
    "$fixture/libfixture.c"
printf '%s\n' \
    '/opt/online-office/program/fixture-app|libfixture-dlopen.so.1' \
    > "$fixture/registry"
! readelf -d "$fixture/root/opt/online-office/program/fixture-app" |
    grep -Fq 'libfixture-dlopen.so.1'
sh "$repo_root/docker/from-source/verify-elf-closure.sh" \
    "$fixture/root" "$fixture/target" "$fixture/registry"

mv "$fixture/root/opt/online-office/program/libfixture-dlopen.so.1" \
    "$fixture/libfixture-dlopen.so.1"
if sh "$repo_root/docker/from-source/verify-elf-closure.sh" \
    "$fixture/root" "$fixture/target" "$fixture/registry" \
    >"$fixture/registry-missing.log" 2>&1; then
    echo 'missing registered dlopen provider unexpectedly passed' >&2
    exit 1
fi
grep -Fq 'missing registered loader requirement libfixture-dlopen.so.1' \
    "$fixture/registry-missing.log"

mkdir -p \
    "$fixture/assembly-root/opt/online-office/program" \
    "$fixture/assembly-source/opt/online-office/program"
cp "$fixture/root/opt/online-office/program/fixture-app" \
    "$fixture/root/opt/online-office/program/libfixture.so.1" \
    "$fixture/assembly-root/opt/online-office/program/"
cp "$fixture/libfixture-dlopen.so.1" \
    "$fixture/assembly-source/opt/online-office/program/"
python3 "$repo_root/docker/from-source/verify_elf_closure.py" assemble \
    "$fixture/assembly-root" "$fixture/target" "$fixture/registry" \
    "$fixture/assembly-source"
test -f \
    "$fixture/assembly-root/opt/online-office/program/libfixture-dlopen.so.1"

mkdir -p "$fixture/target/nix/store/unlinked-dlopen/lib"
mv "$fixture/libfixture-dlopen.so.1" \
    "$fixture/target/nix/store/unlinked-dlopen/lib/libfixture-dlopen.so.1"
if sh "$repo_root/docker/from-source/verify-elf-closure.sh" \
    "$fixture/root" "$fixture/target" "$fixture/registry" \
    >"$fixture/registry-unreachable.log" 2>&1; then
    echo 'loader-unreachable registered dlopen provider unexpectedly passed' >&2
    exit 1
fi
grep -Fq 'missing registered loader requirement libfixture-dlopen.so.1' \
    "$fixture/registry-unreachable.log"

: > "$fixture/registry"
mkdir -p \
    "$fixture/root/opt/cool/systemplate/usr/lib" \
    "$fixture/target/nix/store/unlinked-fixture/lib"
cp "$fixture/root/opt/online-office/program/libfixture.so.1" \
    "$fixture/root/opt/cool/systemplate/usr/lib/libfixture.so.1"
cp "$fixture/root/opt/online-office/program/libfixture.so.1" \
    "$fixture/target/nix/store/unlinked-fixture/lib/libfixture.so.1"
rm "$fixture/root/opt/online-office/program/libfixture.so.1"
if sh "$repo_root/docker/from-source/verify-elf-closure.sh" \
    "$fixture/root" "$fixture/target" "$fixture/registry" \
    >"$fixture/negative.log" 2>&1; then
    echo 'loader-unreachable same-name fixture unexpectedly passed' >&2
    exit 1
fi
grep -Fq 'missing libfixture.so.1' "$fixture/negative.log"

cat > "$fixture/versioned.c" <<'EOF'
int versioned_value(void) { return 42; }
EOF
cat > "$fixture/versioned-app.c" <<'EOF'
extern int versioned_value(void);
int main(void) { return versioned_value() != 42; }
EOF
cat > "$fixture/version-1.map" <<'EOF'
FIXTURE_1.0 { global: versioned_value; };
EOF
cat > "$fixture/version-2.map" <<'EOF'
FIXTURE_2.0 { global: versioned_value; };
EOF
mkdir -p "$fixture/version-root/opt/online-office/program"
gcc -fPIC -shared -Wl,-soname,libversioned.so.1 \
    -Wl,--version-script="$fixture/version-2.map" \
    -o "$fixture/version-root/opt/online-office/program/libversioned.so.1" \
    "$fixture/versioned.c"
gcc -o "$fixture/version-root/opt/online-office/program/versioned-app" \
    "$fixture/versioned-app.c" \
    -L"$fixture/version-root/opt/online-office/program" \
    -Wl,-rpath,'$ORIGIN' -l:libversioned.so.1
sh "$repo_root/docker/from-source/verify-elf-closure.sh" \
    "$fixture/version-root" "$fixture/target" "$fixture/registry"
gcc -fPIC -shared -Wl,-soname,libversioned.so.1 \
    -Wl,--version-script="$fixture/version-1.map" \
    -o "$fixture/version-root/opt/online-office/program/libversioned.so.1" \
    "$fixture/versioned.c"
if sh "$repo_root/docker/from-source/verify-elf-closure.sh" \
    "$fixture/version-root" "$fixture/target" "$fixture/registry" \
    >"$fixture/version-negative.log" 2>&1; then
    echo 'incompatible symbol-version fixture unexpectedly passed' >&2
    exit 1
fi
grep -Fq 'missing symbol versions FIXTURE_2.0' \
    "$fixture/version-negative.log"

python3 - "$fixture/saved-image.tar" "$fixture/saved-oci-image.tar" <<'PY'
import gzip
import io
import json
import sys
import tarfile

def write_archive(path: str, layer: bytes) -> None:
    with tarfile.open(path, "w") as archive:
        files = {
            "config.json": b'{"architecture":"amd64","os":"linux"}\n',
            "layer.tar": layer,
        }
        manifest = json.dumps(
            [{"Config": "config.json", "RepoTags": ["fixture"], "Layers": ["layer.tar"]}]
        ).encode()
        files["manifest.json"] = manifest
        for name, content in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.mtime = 0
            archive.addfile(info, io.BytesIO(content))


layer = b"synthetic-runtime-layer\n" * 128
write_archive(sys.argv[1], layer)
write_archive(sys.argv[2], gzip.compress(layer, mtime=0))
PY
compressed_fixture=$(python3 \
    "$repo_root/docker/from-source/measure_saved_image.py" \
    "$fixture/saved-image.tar")
case "$compressed_fixture" in
    ''|*[!0-9]*) echo 'compressed-size fixture returned a non-numeric value' >&2; exit 1 ;;
esac
test "$compressed_fixture" -gt 0
uncompressed_fixture=$(python3 \
    "$repo_root/docker/from-source/measure_saved_image.py" \
    --uncompressed "$fixture/saved-image.tar")
test "$uncompressed_fixture" -gt "$compressed_fixture"
oci_compressed_fixture=$(python3 \
    "$repo_root/docker/from-source/measure_saved_image.py" \
    "$fixture/saved-oci-image.tar")
oci_uncompressed_fixture=$(python3 \
    "$repo_root/docker/from-source/measure_saved_image.py" \
    --uncompressed "$fixture/saved-oci-image.tar")
test "$oci_uncompressed_fixture" -eq "$uncompressed_fixture"
test "$oci_compressed_fixture" -gt 0
test "$oci_compressed_fixture" -lt "$oci_uncompressed_fixture"

echo 'Source distroless structural and closure tests passed.'
