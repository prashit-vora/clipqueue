#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
if [ ! -x build/clipqueue ]; then
    echo 'Build once with: make' >&2
    exit 1
fi
exec ./build/clipqueue start
