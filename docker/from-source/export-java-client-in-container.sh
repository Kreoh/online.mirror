#!/usr/bin/env bash
set -euo pipefail
fail() { echo "Java client export: $*" >&2; exit 1; }
source_root=/workspace-cache/build/builddir/online
engine="$source_root/engine"
[[ $(git -C "$source_root" rev-parse HEAD) == "$CLIENT_SOURCE_REVISION" ]] || fail 'cache revision differs from selected source; restore matching cache'
# Branding edits in other modules are expected from the ordinary image build.
git -C "$source_root" diff --quiet HEAD -- engine/codemaker engine/unoidl engine/sal engine/salhelper engine/registry engine/store engine/include engine/solenv engine/external engine/ridljar engine/udkapi engine/offapi engine/Makefile.gbuild engine/RepositoryModule_host.mk engine/RepositoryExternal.mk engine/Repository.mk engine/RepositoryFixes.mk engine/configure.ac || fail 'generator source inputs differ from the selected revision'
[[ -f $engine/config_host.mk && -f $engine/config_host.h ]] || fail 'configured engine cache is incomplete'
for setting in "SRCDIR=$engine" "BUILDDIR=$engine" "WORKDIR=$engine/workdir" "INSTDIR=$engine/instdir"; do
    grep -Fxq "export $setting" "$engine/config_host.mk" || fail "configured cache root mismatch: $setting"
done
cmp /output/registries/types.rdb "$engine/instdir/program/types.rdb" || fail 'cached types.rdb differs from image'
cmp /output/registries/offapi.rdb "$engine/instdir/program/types/offapi.rdb" || fail 'cached offapi.rdb differs from image'
cd "$engine"
# Bypass the outer Makefile bootstrap, fetch and configuration. Force this target's
# prerequisite closure so an unverified cached executable or object is never trusted.
# Network is disabled by the caller. Missing prerequisite inputs stop this build.
unset MAKEFLAGS MFLAGS MAKEOVERRIDES
make -f Makefile.gbuild -B -j2 Executable_javamaker
export LD_LIBRARY_PATH="$engine/instdir/program:$engine/workdir/LinkTarget/Library"
tool="$engine/workdir/LinkTarget/Executable/javamaker"
[[ -x $tool ]] || fail 'target did not produce javamaker'
mkdir /output/generated /output/generator /output/licences
"$tool" -O/output/generated /output/registries/types.rdb /output/registries/offapi.rdb
[[ -f /output/generated/cpo/uno/XInterface.class && -f /output/generated/com/sun/star/frame/XDesktop.class ]] || fail 'generation omitted required API roots'
cp "$tool" /output/generator/javamaker
cp config_host.mk config_host.h /output/generator/
git -C "$source_root" archive "$CLIENT_SOURCE_REVISION" engine/codemaker engine/unoidl engine/sal engine/salhelper engine/registry engine/store engine/include engine/solenv engine/external engine/RepositoryExternal.mk engine/Makefile.gbuild > /output/generator/sources.tar
runtime=$(mktemp -d)
trap 'rm -rf "$runtime"' EXIT
mapfile -t sources < <(sed -n '/gb_Jar_add_sourcefiles[_a-z0-9]*,libreoffice,/,/^))/p' ridljar/Jar_libreoffice.mk | sed -n 's@^[[:space:]]*ridljar/\([^[:space:]]*\).*@\1.java@p')
[[ ${#sources[@]} -gt 0 ]] || fail 'maintained Java source list is empty'
for path in "${sources[@]}"; do
    mkdir -p "$runtime/$(dirname "$path")"
    git -C "$source_root" show "$CLIENT_SOURCE_REVISION:engine/ridljar/$path" > "$runtime/$path"
done
chmod -R u=rwX,go=rX "$runtime"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf /output/runtime-sources.tar -C "$runtime" .
cp ridljar/util/manifest /output/licences/runtime-manifest
for path in COPYING COPYING.MPL COPYING.LGPL COPYING.GPL NOTICE; do
    if [[ -f $path ]]; then cp "$path" /output/licences/; fi
done
cp "$source_root/COPYING" /output/licences/online-COPYING
cp "$source_root/THIRDPARTYLICENSES" /output/licences/online-THIRDPARTYLICENSES
core_tree=$(git -C "$source_root" rev-parse "$CLIENT_SOURCE_REVISION:engine")
cat > /output/PROVENANCE.properties <<PROVENANCE
format=1
source_revision=$CLIENT_SOURCE_REVISION
core_revision=$CLIENT_SOURCE_REVISION
core_tree=$core_tree
core_layout=bundled-monorepo-engine
pipeline_revision=$PIPELINE_REVISION
image=$CLIENT_IMAGE
image_id=$CLIENT_IMAGE_ID
builder_id=$CLIENT_BUILDER_ID
build_url=$BUILD_URL
generator_target=make -f Makefile.gbuild -B -j2 Executable_javamaker
generator_provenance=forced-target-and-prerequisites-network-disabled
runtime_source_count=${#sources[@]}
PROVENANCE
cd /output
find generated registries licences generator -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
sha256sum runtime-sources.tar PROVENANCE.properties >> SHA256SUMS
