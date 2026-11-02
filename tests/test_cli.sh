#!/bin/sh
set -eu
binary=$1
temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM

"$binary" --help >"$temporary_dir/help"
if ! grep -q -- '--player' "$temporary_dir/help"; then
    printf '%s\n' 'FAIL: command-line help is missing --player' >&2
    exit 1
fi

expect_failure() {
    expected=$1
    shift
    if "$binary" "$@" >"$temporary_dir/out" 2>"$temporary_dir/error"; then
        printf 'FAIL: invalid arguments succeeded: %s\n' "$*" >&2
        exit 1
    fi
    if ! grep -q -- "$expected" "$temporary_dir/error"; then
        cat "$temporary_dir/error" >&2
        printf 'FAIL: expected error: %s\n' "$expected" >&2
        exit 1
    fi
}
expect_failure 'at least 0.1' --refresh 0.01
expect_failure 'finite number' --refresh nan
expect_failure 'finite number' --offset inf
expect_failure 'finite number' --offset 1abc
expect_failure 'requires a value' --player
expect_failure 'nonempty player' --player=
expect_failure 'Unknown option' --unknown
printf '%s\n' 'CLI tests passed.'
