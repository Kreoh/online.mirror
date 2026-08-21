#! /bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# -- Available env vars --
# * DOCKER_HUB_REPO - which Docker Hub repo to use
# * DOCKER_HUB_TAG  - exact revision-specific Docker Hub tag to create
# * COLLABORA_ONLINE_REPO - which git repo to clone the online monorepo from
# * COLLABORA_ONLINE_BRANCH - which branch to build
# * COLLABORA_SOURCE_REVISION - exact full Kreoh source revision to build
# * COLLABORA_SOURCE_BUILD_HOST_OS - explicit runtime Dockerfile route, Debian or Ubuntu
# * ENGINE_BUILD_TARGET - which make target to run for the engine (when building from source)
# * ONLINE_EXTRA_BUILD_OPTIONS - extra build options for online
# * NO_DOCKER_IMAGE - if set, don't build the docker image itself, just do all the preps

# Check env variables
if [ -z "$DOCKER_HUB_REPO" ]; then
  DOCKER_HUB_REPO="mydomain/collaboraonline"
fi;

if [[ ! "${COLLABORA_SOURCE_REVISION:-}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "COLLABORA_SOURCE_REVISION must be an explicit full lower-case Git revision." >&2
  exit 1
fi
if [ -n "${COLLABORA_ONLINE_REVISION:-}" ]; then
  echo "COLLABORA_ONLINE_REVISION is unsupported: use COLLABORA_SOURCE_REVISION." >&2
  exit 1
fi

EXPECTED_DOCKER_HUB_TAG="26.04.3.1-agent-save-${COLLABORA_SOURCE_REVISION:0:12}"
if [ -z "${DOCKER_HUB_TAG:-}" ]; then
  DOCKER_HUB_TAG="$EXPECTED_DOCKER_HUB_TAG"
fi;
if [ "$DOCKER_HUB_TAG" != "$EXPECTED_DOCKER_HUB_TAG" ]; then
  echo "DOCKER_HUB_TAG must be $EXPECTED_DOCKER_HUB_TAG" >&2
  exit 1
fi
echo "Using Docker Hub Repository: '$DOCKER_HUB_REPO' with tag '$DOCKER_HUB_TAG'."

if [ -n "${COLLABORA_ONLINE_REPO:-}" ] && [ "$COLLABORA_ONLINE_REPO" != "https://github.com/Kreoh/online.mirror.git" ]; then
  echo "COLLABORA_ONLINE_REPO must be https://github.com/Kreoh/online.mirror.git" >&2
  exit 1
fi
if [ -z "$COLLABORA_ONLINE_REPO" ]; then
  COLLABORA_ONLINE_REPO="https://github.com/Kreoh/online.mirror.git"
fi;
if [ -n "${COLLABORA_ONLINE_BRANCH:-}" ] && [ "$COLLABORA_ONLINE_BRANCH" != "kreoh-co-26.04.3.1-agent" ]; then
  echo "COLLABORA_ONLINE_BRANCH must be kreoh-co-26.04.3.1-agent" >&2
  exit 1
fi
if [ -z "${COLLABORA_ONLINE_BRANCH:-}" ]; then
  COLLABORA_ONLINE_BRANCH="kreoh-co-26.04.3.1-agent"
fi;
echo "Building exact revision '$COLLABORA_SOURCE_REVISION' from '$COLLABORA_ONLINE_REPO'"

if [ -n "${ENGINE_ASSETS:-}" ]; then
  echo "ENGINE_ASSETS is prohibited: build the document engine from Kreoh source." >&2
  exit 1
fi;
echo "Building engine from source"

if [ -z "$ENGINE_BUILD_TARGET" ]; then
  ENGINE_BUILD_TARGET=""
fi;
echo "Engine build target: '$ENGINE_BUILD_TARGET'"


SRCDIR=$(realpath `dirname $0`)
INSTDIR="$SRCDIR/instdir"

if [ -n "${COLLABORA_SOURCE_BUILD_HOST_OS:-}" ]; then
  HOST_OS="$COLLABORA_SOURCE_BUILD_HOST_OS"
else
  HOST_OS=$(lsb_release -si 2>/dev/null || true)
fi
if [ "$HOST_OS" != "Debian" ] && [ "$HOST_OS" != "Ubuntu" ]; then
  echo "Unsupported source-build host '$HOST_OS': set COLLABORA_SOURCE_BUILD_HOST_OS to Debian or Ubuntu." >&2
  exit 1
fi
BUILDDIR="$SRCDIR/builddir"

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

rm -rf "$INSTDIR" || true
mkdir -p "$INSTDIR"

# POCO is built as part of the engine (engine/external/poco) and picked up from
# its workdir automatically by the cool configure below; no separate build.


##### cloning & updating #####

# Clone the online monorepo (engine/ contains the rendering engine)
if test ! -d online ; then
  git clone --depth=1 --branch "$COLLABORA_ONLINE_BRANCH" "$COLLABORA_ONLINE_REPO" online || exit 1
fi

(
  cd online &&
  test "$(git config --get remote.origin.url)" = "$COLLABORA_ONLINE_REPO" &&
  git fetch --depth=1 --force origin \
    "+refs/heads/$COLLABORA_ONLINE_BRANCH:refs/remotes/origin/$COLLABORA_ONLINE_BRANCH" &&
  git cat-file -e "$COLLABORA_SOURCE_REVISION^{commit}" &&
  git merge-base --is-ancestor "$COLLABORA_SOURCE_REVISION" \
    "refs/remotes/origin/$COLLABORA_ONLINE_BRANCH" &&
  git checkout --detach -f "$COLLABORA_SOURCE_REVISION" &&
  git clean -f -d &&
  test "$(git rev-parse HEAD)" = "$COLLABORA_SOURCE_REVISION"
) || exit 1

##### engine #####

( cd online/engine && ./autogen.sh --with-distro=CPLinux-LOKit --disable-epm --without-package-format --disable-symbols ) || exit 1
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


# Create new docker image
if [ -z "$NO_DOCKER_IMAGE" ]; then
  cd "$SRCDIR"
  docker build --no-cache \
    --build-arg "COLLABORA_SOURCE_REVISION=$COLLABORA_SOURCE_REVISION" \
    -t "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" -f "$HOST_OS" . || exit 1
else
  echo "Skipping docker image build"
fi;
