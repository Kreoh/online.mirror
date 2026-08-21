#!/usr/bin/env bash
set -eu

missing=0

for tool in \
  aclocal \
  autoconf \
  automake \
  bash \
  bison \
  bsdtar \
  ccache \
  diff \
  docker \
  flex \
  g++ \
  gcc \
  gperf \
  gzip \
  java \
  make \
  meson \
  nasm \
  ninja \
  node \
  npm \
  patch \
  perl \
  pkg-config \
  python3 \
  tar \
  touch \
  uniq \
  unzip \
  uuidgen \
  xmllint \
  xsltproc \
  zip
do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required builder tool: $tool" >&2
    missing=1
  fi
done

if [ "$missing" -ne 0 ]; then
  exit 1
fi

node_version="$(node --version | sed 's/^v//')"
npm_version="$(npm --version)"
case "$node_version" in
  2[0-9].*) ;;
  *) echo "Node 20 or newer is required, found $node_version" >&2; exit 1 ;;
esac
case "$npm_version" in
  9.*|[1-9][0-9].*) ;;
  *) echo "npm 9 or newer is required, found $npm_version" >&2; exit 1 ;;
esac

echo "Collabora source builder preflight passed."
