#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
exec make -C "$project_dir" test "$@"
