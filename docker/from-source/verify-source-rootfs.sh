#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set -eu

fail()
{
    printf 'verify-source-rootfs: %s\n' "$*" >&2
    exit 1
}

case ${1:-} in
    --staged)
        root=${ROOTFS:-/rootfs}
        target=${TARGET_ROOT:-/target-base}
        ;;
    --merged)
        test "$#" -eq 2 || fail 'usage: --merged ROOTFS'
        root=$2
        target=$2
        ;;
    *) fail 'usage: --staged or --merged ROOTFS' ;;
esac

test -d "$root" || fail "rootfs is absent: $root"
test -x "$root/usr/bin/coolwsd" || fail 'coolwsd is absent'
test -x "$root/usr/bin/coolforkit-caps" || fail 'coolforkit-caps is absent'
test -x "$root/usr/bin/coolmount" || fail 'coolmount is absent'
test -x "$root/opt/online-office/program/soffice" || fail 'engine is absent'
test -d "$root/opt/cool/systemplate/lo" || fail 'systemplate engine link is absent'
test -d "$root/opt/cool/child-roots" || fail 'child-roots is absent'
test -d "$root/opt/cool/cache" || fail 'cache is absent'
test -s "$root/usr/lib/locale/locale-archive" || fail 'C.UTF-8 locale is absent'
test -s "$root/etc/ssl/certs/ca-certificates.crt" ||
    test -s "$target/etc/ssl/certs/ca-certificates.crt" ||
    fail 'CA trust bundle is absent'
test -s "$root/opt/cool/systemplate/etc/ssl/certs/ca-certificates.crt" ||
    fail 'systemplate CA trust bundle is absent'
test -s "$root/usr/share/doc/coolwsd/online-office-sbom.spdx.json" ||
    fail 'Online Office SPDX SBOM is absent'
find "$root/usr/share/doc/coolwsd" -type f \
    \( -iname 'copying*' -o -iname 'licen[cs]e*' -o -iname '*copyright*' \
       -o -iname 'notice*' -o -iname 'thirdparty*' \) -print -quit |
    grep -q . || fail 'runtime licence notices are absent'

test "$(awk -F: '$1 == "cool" { print $3 ":" $4 }' "$root/etc/passwd")" = '100:100' ||
    fail 'cool user is not UID and GID 100'
test "$(awk -F: '$1 == "cool" { print $3 }' "$root/etc/group")" = '100' ||
    fail 'cool group is not GID 100'
test "$(stat -c '%u:%g' "$root/opt/cool/child-roots")" = '100:100' ||
    fail 'child-roots ownership is incorrect'
test "$(stat -c '%u:%g' "$root/opt/cool/cache")" = '100:100' ||
    fail 'cache ownership is incorrect'
find "$root" -xdev \( -uid 100 -o -gid 100 \) -print | LC_ALL=C sort |
    while IFS= read -r owned; do
        relative=${owned#"$root"}
        case "$relative" in
            /opt/cool/cache|/opt/cool/cache/*|/opt/cool/child-roots|/opt/cool/child-roots/*|/var/log|/var/log/coolwsd.log) ;;
            *) fail "unexpected UID or GID 100 ownership: $relative" ;;
        esac
    done
find "$root" -xdev \( -type f -o -type d \) -perm /022 -print | LC_ALL=C sort |
    while IFS= read -r writable; do
        relative=${writable#"$root"}
        case "$relative" in
            /etc/passwd|/tmp|/tmp/*|/opt/cool/cache|/opt/cool/cache/*|/opt/cool/child-roots|/opt/cool/child-roots/*|/var/log|/var/log/coolwsd.log) ;;
            *) fail "unexpected group or world-writable runtime path: $relative" ;;
        esac
    done
for immutable in \
    "$root/opt/online-office" "$root/opt/cool/systemplate" \
    "$root/usr/share/coolwsd" "$root/usr/share/doc/coolwsd" "$root/etc/coolwsd"
do
    test "$(stat -c '%u:%g' "$immutable")" = '0:0' ||
        fail "immutable tree is not root-owned: ${immutable#"$root"}"
    find "$immutable" -xdev \( -type f -o -type d \) -perm /0222 -print -quit |
        grep -q . && fail "immutable tree contains a writable path: ${immutable#"$root"}"
done

expected_forkit='cap_chown,cap_fowner,cap_sys_chroot=ep'
expected_mount='cap_sys_admin=ep'
forkit=$(getcap -n "$root/usr/bin/coolforkit-caps" | sed 's/^[^ ]* *//')
mount=$(getcap -n "$root/usr/bin/coolmount" | sed 's/^[^ ]* *//')
test "$forkit" = "$expected_forkit" || fail "coolforkit-caps capabilities differ: $forkit"
test "$mount" = "$expected_mount" || fail "coolmount capabilities differ: $mount"

for forbidden in \
    bin/sh bin/bash usr/bin/sh usr/bin/bash bin/busybox usr/bin/busybox \
    usr/bin/apt usr/bin/apt-get usr/bin/dpkg usr/bin/rpm usr/bin/apk \
    usr/bin/python usr/bin/python3 usr/bin/java usr/bin/javac usr/bin/ssh \
    usr/bin/gcc usr/bin/g++ usr/bin/cc usr/bin/make usr/bin/nano usr/bin/vi \
    usr/bin/coolconfig usr/bin/coolconvert usr/bin/coolstress \
    usr/bin/coolwsd-systemplate-setup
do
    test ! -e "$root/$forbidden" || fail "forbidden runtime path is present: /$forbidden"
done

if find "$root" -mindepth 1 \
    \( -iname '*pyuno*' -o -iname '*pythonloader*' -o -iname 'python*' \
       -o -name '*.py' -o -name '*.pyc' -o -name '*.pyo' -o -name '*.jar' \
       -o -name '*.class' -o -iname 'libjvm.so*' -o -iname 'javasettings*' \
       -o -iname 'javavendors*' -o -iname 'libjvmaccess*' \
       -o -iname 'libjvmfwk*' -o -iname 'sunjavaplugin*' \
       -o -iname 'javaldx*' \) -print -quit | grep -q .; then
    fail 'Python, PyUNO or Java content is present'
fi
if find "$root/opt/cool/systemplate/usr/share/fonts" "$root/usr/share/fonts" \
    -type f -print -quit 2>/dev/null | grep -q .; then
    fail 'an external system font leaked into the final runtime'
fi

script_directory=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
"$script_directory/verify-elf-closure.sh" "$root" "$target" \
    /usr/local/share/online-office/runtime-dynamic-dependencies.conf

if test "${1:-}" = --merged; then
    test ! -e "$root/opt/online-office/EULA_en-US.rtf" || fail 'commercial EULA remains'
    test ! -e "$root/usr/share/coolwsd/browser/dist/welcome" || fail 'branded welcome remains'
    test ! -e "$root/usr/share/coolwsd/browser/dist/images/collabora-office-white.svg" ||
        fail 'branded image remains'
fi

echo 'Source distroless rootfs verification passed.'
