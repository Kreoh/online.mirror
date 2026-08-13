#! /bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# -- Available env vars --
# * COLLABORA_ONLINE_REPO - which git repo to clone the online monorepo from
# * COLLABORA_ONLINE_BRANCH - which branch to build
# * COLLABORA_ONLINE_REVISION - exact Kreoh source revision to build
# * ENGINE_BUILD_TARGET - which make target to run for the engine
# * ONLINE_EXTRA_BUILD_OPTIONS - extra build options for online
#
# Note: on Alpine the engine must be built from source -- prebuilt engine
# assets are linked against glibc and won't work on musl.

if [ -z "$COLLABORA_ONLINE_REPO" ]; then
  COLLABORA_ONLINE_REPO="https://github.com/Kreoh/online.mirror.git"
fi;
if [ -z "$COLLABORA_ONLINE_BRANCH" ]; then
  COLLABORA_ONLINE_BRANCH="main"
fi;
if [ -z "$COLLABORA_ONLINE_REVISION" ]; then
  COLLABORA_ONLINE_REVISION="9b56f5583ab8df2202fb0b8471dcbf622d7825f8"
fi;
echo "Building exact revision '$COLLABORA_ONLINE_REVISION' from '$COLLABORA_ONLINE_REPO'"

if [ -n "${ENGINE_ASSETS:-}" ]; then
  echo "ENGINE_ASSETS is prohibited: build the document engine from Kreoh source." >&2
  exit 1
fi

if [ -z "$ENGINE_BUILD_TARGET" ]; then
  ENGINE_BUILD_TARGET=""
fi;
echo "Engine build target: '$ENGINE_BUILD_TARGET'"

SRCDIR=$(realpath `dirname $0`)
INSTDIR="$SRCDIR/instdir"
BUILDDIR="$SRCDIR/builddir"

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

rm -rf "$INSTDIR" || true
mkdir -p "$INSTDIR"

# POCO is built as part of the engine (engine/external/poco) and picked up from
# its workdir automatically by the cool configure below; no separate build.


##### cloning & updating #####

# Clone the online monorepo (engine/ contains the rendering engine)
git clone --depth=1 --branch "$COLLABORA_ONLINE_BRANCH" "$COLLABORA_ONLINE_REPO" online || exit 1
(cd online && git fetch origin "$COLLABORA_ONLINE_REVISION" && git checkout --detach -f "$COLLABORA_ONLINE_REVISION" && test "$(git rev-parse HEAD)" = "$COLLABORA_ONLINE_REVISION") || exit 1

##### engine #####

# Build engine from source -- prebuilt assets are glibc and don't work on musl.
( cd online/engine && ./autogen.sh --with-distro=CPLinux-LOKit --without-package-format --disable-symbols --with-lang=en-US ) || exit 1
( cd online/engine && make $ENGINE_BUILD_TARGET ) || exit 1

# copy stuff
mkdir -p "$INSTDIR"/opt/
cp -a online/engine/instdir "$INSTDIR"/opt/collaboraoffice

##### coolwsd & cool #####

# build
( cd online && ./autogen.sh ) || exit 1
( cd online && ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-tests --with-lokit-path="$BUILDDIR"/online/engine/include --with-lo-path=/opt/collaboraoffice $ONLINE_EXTRA_BUILD_OPTIONS) || exit 1
( cd online && make -j $(nproc)) || exit 1

# copy stuff
( cd online && DESTDIR="$INSTDIR" make install ) || exit 1
