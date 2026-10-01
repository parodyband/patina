#!/usr/bin/env bash
# Set up a fresh workspace for the reference-match barrel benchmark.
# usage: bench/barrel_ref/prepare.sh <workspace_dir>
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
ws=$1
mkdir -p "$ws/skills"
cp "$here/TASK.md" "$here/reference.png" "$ws/"
cp -r "$repo/.claude/skills/stylized-environment-art" "$ws/skills/"
