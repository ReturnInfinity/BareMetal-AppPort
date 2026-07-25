#!/bin/bash
set -e

# Fetches and patches musl 1.2.6, lwIP 2.2.0, and mbedTLS 3.6.6 into this
# directory. Run once before ./build-app.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$SCRIPT_DIR/scripts/get-musl.sh"
"$SCRIPT_DIR/scripts/get-lwip.sh"
"$SCRIPT_DIR/scripts/get-mbedtls.sh"
