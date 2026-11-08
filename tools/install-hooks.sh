#!/usr/bin/env bash
# Links the tracked pre-push hook into the common git dir (#252). Only pre-push: core.hooksPath would hide the
# untracked graphify hooks that live there. Safe to run twice.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir)
mkdir -p "$common/hooks"
ln -sfn "$repo/tools/git-hooks/pre-push" "$common/hooks/pre-push"
echo "installed $common/hooks/pre-push"
