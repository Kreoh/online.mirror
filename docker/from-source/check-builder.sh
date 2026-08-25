#!/usr/bin/env bash
set -eu

missing=0
configure_ac="${1:-configure.ac}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

for tool in \
  aclocal \
  autoconf \
  automake \
  bash \
  bison \
  brotli \
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
  rsync \
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

check_header() {
  header="$1"
  if ! printf '#include <%s>\nint main(void) { return 0; }\n' "$header" | \
      gcc -x c - -o "$tmpdir/check-header" >/dev/null 2>&1; then
    echo "Missing required C header: $header" >&2
    missing=1
  fi
}

check_pkg_config() {
  module="$1"
  if ! pkg-config --exists "$module"; then
    echo "Missing required pkg-config module: $module" >&2
    missing=1
  fi
}

for header in \
  linux/seccomp.h \
  security/pam_appl.h \
  sys/capability.h
do
  check_header "$header"
done

if ! printf '#include <sys/capability.h>\nint main(void) { cap_t cap = cap_get_proc(); if (cap) cap_free(cap); return 0; }\n' | \
    gcc -x c - -lcap -o "$tmpdir/check-libcap" >/dev/null 2>&1; then
  echo "Missing required libcap development library: cap_get_proc" >&2
  missing=1
fi

for module in \
  cppunit \
  libpcre2-8 \
  libpng \
  'libzstd >= 1.4.0' \
  openssl \
  zlib
do
  check_pkg_config "$module"
done

openssl_cflags="$(pkg-config --cflags openssl 2>/dev/null || true)"
if ! printf '#include <openssl/opensslv.h>\n#if OPENSSL_VERSION_NUMBER < 0x30000000\n#error OpenSSL 3.0.0 or newer is required\n#endif\nint main(void) { return 0; }\n' | \
    gcc -x c - $openssl_cflags -o "$tmpdir/check-openssl" >/dev/null 2>&1; then
  echo "OpenSSL headers must be version 3.0.0 or newer" >&2
  missing=1
fi

if [ ! -f "$configure_ac" ]; then
  echo "Missing configure.ac for builder preflight: $configure_ac" >&2
  missing=1
else
  configure_python_modules="$(sed -n 's/^[[:space:]]*for MODULE in \(.*\); do[[:space:]]*$/\1/p' "$configure_ac" | head -n 1)"
  if [ -z "$configure_python_modules" ]; then
    echo "Could not determine Python module prerequisites from $configure_ac" >&2
    missing=1
  else
    for module in $configure_python_modules
    do
      if ! python3 -c "import ${module}" >/dev/null 2>&1; then
        echo "Missing required Python module from $configure_ac: $module" >&2
        missing=1
      fi
    done
  fi
fi

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

echo "Online Office source builder preflight passed."
