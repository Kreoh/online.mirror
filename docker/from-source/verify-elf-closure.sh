#!/bin/sh
set -eu

test "$#" -eq 3 || {
    echo 'verify-elf-closure: usage: ROOTFS TARGET_BASE REGISTRY' >&2
    exit 1
}

script_directory=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
exec python3 "$script_directory/verify_elf_closure.py" verify "$1" "$2" "$3"
