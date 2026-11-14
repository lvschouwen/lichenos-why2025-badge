#!/usr/bin/env bash
# Host test for tools/check_release_image.sh (#401): fake images with and without the debug-build marker.
set -euo pipefail
exec bash "$(dirname "$0")/../check_release_image.sh" --selftest
