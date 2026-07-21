#!/bin/bash
# Remove all build artifacts (.o, .a, .app) from this directory.
set -e

cd "$(dirname "$0")"

find . -type f \( -name '*.o' -o -name '*.a' -o -name '*.app' \) -print -delete
