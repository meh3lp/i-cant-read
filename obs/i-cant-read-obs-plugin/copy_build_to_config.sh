#!/usr/bin/env bash
set -euo pipefail

PLUGIN=i-cant-read-ocr-gates
RUNDIR=./build_x86_64/rundir/RelWithDebInfo
DEST=~/.config/obs-studio/plugins/$PLUGIN

mkdir -p "$DEST/bin/64bit" "$DEST/data"

cp "$RUNDIR/$PLUGIN.so" "$DEST/bin/64bit/"
cp -r "$RUNDIR/$PLUGIN/." "$DEST/data/"

echo "deployed $PLUGIN to $DEST"