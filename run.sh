#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "$#" -eq 0 ]; then
    recording="$script_dir/data/session-$(date -u +%Y%m%dT%H%M%SZ)-$$.jsonl"
    printf 'Recording to %s\nPress Ctrl-C to stop.\n' "$recording"
    set -- --record "$recording"
fi

exec "$script_dir/build/arbitrage_engine" "$@"
