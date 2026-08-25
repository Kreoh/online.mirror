#! /bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# -- Available env vars --
# * DOCKER_HUB_REPO - which Docker Hub repo to use
# * DOCKER_HUB_TAG  - exact revision-specific Docker Hub tag to create
# * ONLINE_OFFICE_SOURCE_REPOSITORY - which Git repository to clone
# * ONLINE_OFFICE_SOURCE_BRANCH - which branch to build
# * ONLINE_OFFICE_SOURCE_REVISION - exact full Kreoh source revision to build
# * ONLINE_OFFICE_SOURCE_BUILD_HOST_OS - explicit runtime Dockerfile route, Debian or Ubuntu
# * ONLINE_OFFICE_SOURCE_CACHE_DIR - persistent build cache root
# * ONLINE_OFFICE_FINAL_DOCKER_NO_CACHE - if true, build the final Docker image without cache
# * ENGINE_BUILD_TARGET - which make target to run for the engine (when building from source)
# * ONLINE_EXTRA_BUILD_OPTIONS - extra build options for online
# * NO_DOCKER_IMAGE - if set, don't build the docker image itself, just do all the preps

# Check env variables
if [ -z "$DOCKER_HUB_REPO" ]; then
  DOCKER_HUB_REPO="mydomain/online-office"
fi;

if [[ ! "${ONLINE_OFFICE_SOURCE_REVISION:-}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "ONLINE_OFFICE_SOURCE_REVISION must be an explicit full lower-case Git revision." >&2
  exit 1
fi

EXPECTED_DOCKER_HUB_TAG="26.04.4.0-agent-save-${ONLINE_OFFICE_SOURCE_REVISION:0:12}"
if [ -z "${DOCKER_HUB_TAG:-}" ]; then
  DOCKER_HUB_TAG="$EXPECTED_DOCKER_HUB_TAG"
fi;
if [ "$DOCKER_HUB_TAG" != "$EXPECTED_DOCKER_HUB_TAG" ]; then
  echo "DOCKER_HUB_TAG must be $EXPECTED_DOCKER_HUB_TAG" >&2
  exit 1
fi
echo "Using Docker Hub Repository: '$DOCKER_HUB_REPO' with tag '$DOCKER_HUB_TAG'."

if [ -n "${ONLINE_OFFICE_SOURCE_REPOSITORY:-}" ] && [ "$ONLINE_OFFICE_SOURCE_REPOSITORY" != "https://github.com/Kreoh/online.mirror.git" ]; then
  echo "ONLINE_OFFICE_SOURCE_REPOSITORY must be https://github.com/Kreoh/online.mirror.git" >&2
  exit 1
fi
if [ -z "${ONLINE_OFFICE_SOURCE_REPOSITORY:-}" ]; then
  ONLINE_OFFICE_SOURCE_REPOSITORY="https://github.com/Kreoh/online.mirror.git"
fi;
if [ -n "${ONLINE_OFFICE_SOURCE_BRANCH:-}" ] && [ "$ONLINE_OFFICE_SOURCE_BRANCH" != "kreoh-main-agent-port" ]; then
  echo "ONLINE_OFFICE_SOURCE_BRANCH must be kreoh-main-agent-port" >&2
  exit 1
fi
if [ -z "${ONLINE_OFFICE_SOURCE_BRANCH:-}" ]; then
  ONLINE_OFFICE_SOURCE_BRANCH="kreoh-main-agent-port"
fi;
echo "Building exact revision '$ONLINE_OFFICE_SOURCE_REVISION' from '$ONLINE_OFFICE_SOURCE_REPOSITORY'"

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
CACHE_ROOT="${ONLINE_OFFICE_SOURCE_CACHE_DIR:-${HOME:-$SRCDIR}/.cache/online-office-source-builder}"

if [ -n "${ONLINE_OFFICE_SOURCE_BUILD_HOST_OS:-}" ]; then
  HOST_OS="$ONLINE_OFFICE_SOURCE_BUILD_HOST_OS"
else
  HOST_OS=$(lsb_release -si 2>/dev/null || true)
fi
if [ "$HOST_OS" != "Debian" ] && [ "$HOST_OS" != "Ubuntu" ]; then
  echo "Unsupported source-build host '$HOST_OS': set ONLINE_OFFICE_SOURCE_BUILD_HOST_OS to Debian or Ubuntu." >&2
  exit 1
fi
BUILDDIR="$CACHE_ROOT/builddir"
if [ -z "${CCACHE_DIR:-}" ]; then
  export CCACHE_DIR="$CACHE_ROOT/ccache"
fi

mkdir -p "$BUILDDIR" "$CCACHE_DIR"
cd "$BUILDDIR"

rm -rf "$INSTDIR" || true
mkdir -p "$INSTDIR"

# POCO is built as part of the engine (engine/external/poco) and picked up from
# its workdir automatically by the cool configure below; no separate build.


##### cloning & updating #####

# Clone the online monorepo (engine/ contains the rendering engine)
if test ! -d online ; then
  git clone --depth=1 --branch "$ONLINE_OFFICE_SOURCE_BRANCH" "$ONLINE_OFFICE_SOURCE_REPOSITORY" online || exit 1
fi

(
  cd online &&
  test "$(git config --get remote.origin.url)" = "$ONLINE_OFFICE_SOURCE_REPOSITORY" &&
  git fetch --depth=1 --force origin \
    "+refs/heads/$ONLINE_OFFICE_SOURCE_BRANCH:refs/remotes/origin/$ONLINE_OFFICE_SOURCE_BRANCH" &&
  git cat-file -e "$ONLINE_OFFICE_SOURCE_REVISION^{commit}" &&
  git merge-base --is-ancestor "$ONLINE_OFFICE_SOURCE_REVISION" \
    "refs/remotes/origin/$ONLINE_OFFICE_SOURCE_BRANCH" &&
  git checkout --detach -f "$ONLINE_OFFICE_SOURCE_REVISION" &&
  git clean -f -d &&
  test "$(git rev-parse HEAD)" = "$ONLINE_OFFICE_SOURCE_REVISION"
) || exit 1

python3 "$SRCDIR/debrand.py" source "$BUILDDIR/online" || exit 1
python3 "$SRCDIR/debrand.py" scan-source "$BUILDDIR/online" || exit 1

bash "$SRCDIR/check-builder.sh" "$BUILDDIR/online/configure.ac" || exit 1
( cd online && ./autogen.sh ) || exit 1

##### engine #####

( cd online/engine && ./autogen.sh --with-distro=OnlineLinux-LOKit --disable-epm --without-package-format --disable-symbols ) || exit 1
( cd online/engine && make $ENGINE_BUILD_TARGET ) || exit 1

# copy stuff
mkdir -p "$INSTDIR"/opt/
cp -a online/engine/instdir "$INSTDIR"/opt/online-office

##### coolwsd & cool #####

# build
( cd online && ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-tests --with-lokit-path="$BUILDDIR"/online/engine/include --with-lo-builddir="$BUILDDIR"/online/engine --with-lo-path=/opt/online-office --without-app-branding --with-app-name='Online Office' --with-app-package-name=org.onlineoffice.app --without-vendor --with-info-url=about:blank $ONLINE_EXTRA_BUILD_OPTIONS) || exit 1
( cd online && make -j $(nproc)) || exit 1

# copy stuff
( cd online && DESTDIR="$INSTDIR" make install ) || exit 1

python3 "$SRCDIR/debrand.py" rootfs "$INSTDIR" "$BUILDDIR/online" || exit 1

# Create new docker image
if [ -z "$NO_DOCKER_IMAGE" ]; then
  cd "$SRCDIR"
  docker_build_args=(
    --build-arg "ONLINE_OFFICE_SOURCE_REVISION=$ONLINE_OFFICE_SOURCE_REVISION" \
    -t "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" -f "$HOST_OS"
  )
  case "${ONLINE_OFFICE_FINAL_DOCKER_NO_CACHE:-}" in
    1|true|TRUE|yes|YES) docker_build_args=(--no-cache "${docker_build_args[@]}") ;;
  esac
  docker build "${docker_build_args[@]}" . || exit 1
  python3 "$SRCDIR/debrand.py" scan-image "$DOCKER_HUB_REPO:$DOCKER_HUB_TAG" || exit 1
else
  echo "Skipping docker image build"
fi;
