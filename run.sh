#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "$#" -eq 0 ]; then
    recording="$script_dir/data/sessions/session-$(date -u +%Y%m%dT%H%M%SZ)-$$"
    printf 'Recording to %s\nPress Ctrl-C to stop.\n' "$recording"
    set -- capture --session "$recording"
fi

exec "$script_dir/build/arbitrage_engine" "$@"
