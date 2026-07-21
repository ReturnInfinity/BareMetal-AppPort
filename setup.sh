#!/bin/bash
set -e

# Fetches and patches musl 1.2.6 and lwIP 2.2.0 into this directory.
# Run once before ./build-app.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$SCRIPT_DIR/scripts/get-musl.sh"
"$SCRIPT_DIR/scripts/get-lwip.sh"
