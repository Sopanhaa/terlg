#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
if ! command -v make >/dev/null 2>&1; then
    printf '%s\n' 'make is missing. Install build-essential (Debian/Ubuntu); see README.md.' >&2
    exit 1
fi
exec make -C "$project_dir" all "$@"
