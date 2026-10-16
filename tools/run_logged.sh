#!/bin/sh
# Runs each argument as a shell command from the project root and appends "PS> <command>"
# followed by its output to the log named by the first argument, for tools/render_terminal.py.
# Usage: tools/run_logged.sh <log> "<command>" ["<command>" ...]
log="$1"; shift
: > "$log"
for cmd in "$@"; do
    printf 'PS> %s\n' "$cmd" >> "$log"
    sh -c "$cmd" >> "$log" 2>&1
done
