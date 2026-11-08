#!/bin/bash
# Bakes the shader bundle of a release build on lavapipe (no GPU needed). The
# release workflow runs this file in the "Shader bundle" step of its bake job
# (release-package.yml); bake-shaders-ci.py
# next to it does the work and documents the options (python -I bake-shaders-ci.py --help).
#
#   bash bake-shaders-ci.sh --build-dir build-vc170-64-release [options]
#
# Exit status: 0 with the bundle in place, 1 when a step failed, 2 for wrong
# arguments, 3 when what the release is made from changed while Mesa ran (no
# bundle is put in place unless it is 0).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
python=$(command -v python || command -v python3 || command -v py) || { echo "bake-shaders-ci: no python" >&2; exit 1; }
exec "$python" -I "$here/bake-shaders-ci.py" "$@"
