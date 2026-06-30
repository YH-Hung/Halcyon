#!/usr/bin/env bash
# Build the full documentation site: Doxygen API reference under docs/api, then
# the MkDocs Material site into site/ (which includes docs/api as /api).
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> Doxygen API reference"
doxygen Doxyfile

echo "==> MkDocs site"
mkdocs build --strict

echo "==> Site built in ./site (open ./site/index.html)"
