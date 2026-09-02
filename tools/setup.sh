#!/bin/sh
# Creates tools/.venv with Pillow. Run once: sh tools/setup.sh
set -e
cd "$(dirname "$0")"
python3 -m venv .venv
./.venv/bin/pip install --quiet --upgrade pip
./.venv/bin/pip install --quiet pillow
echo "ok: use tools/.venv/bin/python"
