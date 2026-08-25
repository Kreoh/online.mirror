#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Assemble a source-installed Online Office tree for the distroless image.
# The exact source build is available at /source-instdir. This script creates
# the systemplate, copies that runtime payload once, and adds the recursive ELF
# dependency closure which the target image does not provide.

set -eu

ROOTFS=${ROOTFS:-/rootfs}
TARGET_ROOT=${TARGET_ROOT:-/target-base}
SYSTEMPLATE_BUILD=/tmp/online-office-systemplate
SOURCE_ROOT=${SOURCE_ROOT:-/source-instdir}

fail()
{
    printf 'assemble-source-rootfs: %s\n' "$*" >&2
    exit 1
}

test -d "$SOURCE_ROOT/opt/online-office" || fail 'source-built engine is absent'
test -x "$SOURCE_ROOT/usr/bin/coolwsd" || fail 'source-built coolwsd is absent'
test -x "$SOURCE_ROOT/usr/bin/coolforkit-caps" || fail 'coolforkit-caps is absent'
test -x "$SOURCE_ROOT/usr/bin/coolmount" || fail 'coolmount is absent'
test -d "$TARGET_ROOT" || fail 'target base inspection tree is absent'

# The source-built engine supplies document fonts, including Noto CJK. The
# assembler therefore installs no Debian font packs. The empty directory lets
# the upstream systemplate helper run when the builder has no system fonts.
mkdir -p /usr/share/fonts
rm -rf "$SYSTEMPLATE_BUILD" "$ROOTFS"
mkdir -p "$SYSTEMPLATE_BUILD" "$ROOTFS"
# The upstream helper needs the source installation at its final absolute path.
tar -C "$SOURCE_ROOT" -cf - . | tar -C / -xf -
/usr/bin/coolwsd-systemplate-setup \
    "$SYSTEMPLATE_BUILD" /opt/online-office >/dev/null

# Inventory everything installed by the source build rather than inferring it
# from Debian packages or path prefixes. Then remove material with no role in a
# directly executed distroless container.
tar -C "$SOURCE_ROOT" -cf - --xattrs --xattrs-include='*' . |
    tar -C "$ROOTFS" -xf - --xattrs --xattrs-include='*'
rm -rf \
    "$ROOTFS/usr/share/man" \
    "$ROOTFS/usr/share/info" \
    "$ROOTFS/usr/share/lintian" \
    "$ROOTFS/usr/share/bug" \
    "$ROOTFS/lib/systemd" \
    "$ROOTFS/usr/lib/systemd" \
    "$ROOTFS/etc/apparmor.d" \
    "$ROOTFS/etc/nginx" \
    "$ROOTFS/etc/apache2"
rm -f \
    "$ROOTFS/usr/bin/loolwsd" \
    "$ROOTFS/usr/bin/loolconfig" \
    "$ROOTFS/usr/bin/loolwsd-systemplate-setup" \
    "$ROOTFS/usr/bin/coolwsd-systemplate-setup" \
    "$ROOTFS/usr/bin/coolconfig" \
    "$ROOTFS/usr/bin/coolconvert" \
    "$ROOTFS/usr/bin/coolstress"

# Python and PyUNO are disabled in the engine configuration. Some dictionary
# extensions still install Python scripts as inert data, so remove those and
# any residual Python loader payload before calculating the runtime closure.
find "$ROOTFS" -depth -type d \
    \( -iname '*python*' -o -iname '*pyuno*' \) \
    -exec rm -rf -- {} +
find "$ROOTFS" -type f \
    \( -iname '*python*' -o -iname '*pyuno*' -o -name '*.py' \
       -o -name '*.pyc' -o -name '*.pyo' \) \
    -delete

find "$ROOTFS/usr/share/doc/coolwsd" -type f \
    ! -name 'COPYING*' \
    ! -name 'LICENSE*' \
    ! -name 'NOTICE*' \
    ! -name 'THIRDPARTY*' \
    ! -name '*copyright*' \
    ! -name 'online-office-sbom.spdx.json' \
    -delete

mkdir -p "$ROOTFS/opt/cool"
cp -a "$SYSTEMPLATE_BUILD" "$ROOTFS/opt/cool/systemplate"
# The engine bundles its document fonts, including Noto CJK. Keep fontconfig's
# configuration, but discard Debian fonts and caches copied by the helper.
rm -rf \
    "$ROOTFS/opt/cool/systemplate/usr/share/fonts" \
    "$ROOTFS/opt/cool/systemplate/var/cache/fontconfig"
mkdir -p "$ROOTFS/opt/cool/systemplate/usr/share/fonts"

# Resolve absolute symlinks as if TARGET_ROOT were /. This is needed for the
# Nix-backed ZenDiS image, whose FHS library links point into /nix/store.
resolve_in_target()
{
    target_path=$1
    count=0
    while test -L "$TARGET_ROOT$target_path" && test "$count" -lt 40; do
        link=$(readlink "$TARGET_ROOT$target_path")
        case "$link" in
            /*) target_path=$link ;;
            *) target_path=$(dirname "$target_path")/$link ;;
        esac
        count=$((count + 1))
    done
    test -f "$TARGET_ROOT$target_path" && printf '%s\n' "$TARGET_ROOT$target_path"
    return 0
}

target_object()
{
    object_name=$1
    for directory in \
        /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu \
        /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu \
        /usr/lib /lib /usr/lib64 /lib64
    do
        resolved=$(resolve_in_target "$directory/$object_name")
        if test -n "$resolved"; then
            printf '%s\n' "$resolved"
            return
        fi
    done
}

echo '=== Closing source runtime ELF dependencies ==='
python3 /usr/local/bin/verify_elf_closure.py \
    assemble "$ROOTFS" "$TARGET_ROOT" \
    /usr/local/share/online-office/runtime-dynamic-dependencies.conf /

# The in-jail process shares the target image's libc. Replace glibc loader,
# NSS, resolver and CA material copied from Debian with exact target objects.
echo '=== Overlaying systemplate glibc and CA material from target base ==='
find "$ROOTFS/opt/cool/systemplate" -type f \
    \( -name 'ld-*' -o -name 'ld64.so*' -o -name 'libnss_*.so*' \
       -o -name 'libresolv.so*' \) -print |
    while IFS= read -r destination; do
        source=$(target_object "$(basename "$destination")")
        test -n "$source" || fail "target base lacks $(basename "$destination")"
        cp -f --preserve=mode,timestamps "$source" "$destination"
    done

target_ca=$(resolve_in_target /etc/ssl/certs/ca-certificates.crt)
test -n "$target_ca" || fail 'target base CA trust bundle is absent'
install -D -m 0644 "$target_ca" \
    "$ROOTFS/etc/ssl/certs/ca-certificates.crt"
install -D -m 0644 "$target_ca" \
    "$ROOTFS/opt/cool/systemplate/etc/ssl/certs/ca-certificates.crt"

# A compact UTF-8 locale is required for non-ASCII document names.
rm -f /usr/lib/locale/locale-archive
localedef -i C -c -f UTF-8 C.UTF-8
install -D -m 0644 /usr/lib/locale/locale-archive \
    "$ROOTFS/usr/lib/locale/locale-archive"

# Create a stable UID 100 identity without carrying adduser or its dependencies.
install -d -m 0755 "$ROOTFS/etc"
printf '%s\n' \
    'root:x:0:0:root:/root:/usr/sbin/nologin' \
    'cool:x:100:100:Online Office:/opt/cool:/usr/sbin/nologin' \
    > "$ROOTFS/etc/passwd"
printf '%s\n' 'root:x:0:' 'cool:x:100:' > "$ROOTFS/etc/group"
chmod 0664 "$ROOTFS/etc/passwd"
chmod 0644 "$ROOTFS/etc/group"

# Installed application content stays immutable and root-owned. Only explicit
# daemon state paths are owned by the runtime identity.
chown -R 0:0 "$ROOTFS/opt/online-office" \
    "$ROOTFS/opt/cool/systemplate" \
    "$ROOTFS/usr/share/coolwsd" \
    "$ROOTFS/usr/share/doc/coolwsd" \
    "$ROOTFS/etc/coolwsd"
chmod -R a-w "$ROOTFS/opt/online-office" \
    "$ROOTFS/opt/cool/systemplate" \
    "$ROOTFS/usr/share/coolwsd" \
    "$ROOTFS/usr/share/doc/coolwsd" \
    "$ROOTFS/etc/coolwsd"
install -d -o 100 -g 100 -m 0750 \
    "$ROOTFS/opt/cool/child-roots" "$ROOTFS/opt/cool/cache"
install -d -m 1777 "$ROOTFS/tmp"
install -d -o 100 -g 100 -m 0755 "$ROOTFS/var/log"
install -o 100 -g 100 -m 0644 /dev/null "$ROOTFS/var/log/coolwsd.log"

setcap cap_chown,cap_fowner,cap_sys_chroot=ep \
    "$ROOTFS/usr/bin/coolforkit-caps"
setcap cap_sys_admin=ep "$ROOTFS/usr/bin/coolmount"

ROOTFS="$ROOTFS" TARGET_ROOT="$TARGET_ROOT" \
    /usr/local/bin/verify-source-rootfs --staged

du -sb "$ROOTFS" | awk '{ print "assembled source rootfs bytes: " $1 }'
echo "assembled source rootfs files: $(find "$ROOTFS" -type f | wc -l)"
