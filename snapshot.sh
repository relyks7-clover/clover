#!/bin/bash
# snapshot.sh - archive the current source before a self-modification.
# evidence for guideline 1: never overwrite the only copy.
set -euo pipefail
cd "$(dirname "$0")"
stamp=$(date +%Y%m%d-%H%M%S)
dir="archives/snapshots/$stamp"
mkdir -p "$dir"
for f in main.cpp tool.h tool_registry.cpp sysprompts/clover.txt; do
    [ -e "$f" ] || continue
    dest="$dir/$(basename "$f")"
    cp -p "$f" "$dest"
done
if [ -d tools ]; then
    mkdir -p "$dir/tools"
    cp -p tools/*.cpp "$dir/tools/" 2>/dev/null || true
fi
echo "$dir"
