#!/bin/bash
set -e

# Mozilla's CA root bundle, republished in PEM form by curl.se
# (https://curl.se/docs/caextract.html) -- this port's one trust store:
# port/tls_shim.c's mbedtls_x509_crt_parse_file() and curltest.c's
# CURLOPT_CAINFO both point at it once
# port/mbedtls_port/install-cacert.sh has written it onto disk.img.
#
# Unlike get-mbedtls.sh/get-curl.sh/etc, there's no version number to
# pin here -- curl.se republishes this bundle under the same URL as
# Mozilla's own root store changes, so there's no fixed release to ask
# for. The reproducibility those other scripts get from a version
# number, this gets from vendoring instead: the outer BareMetal-App
# repo's files/cacert.pem (copied into build/ by that repo's setup.sh
# before this script runs, same as its other files/* tarballs) is what
# a normal ./setup.sh run actually uses -- the download below only
# fires if that vendored copy is missing.
FILE="cacert.pem"
URL="https://curl.se/ca/cacert.pem"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -f "$FILE" ]; then
	echo "$FILE already exists - skipping download."
	exit 0
fi

echo "- Downloading ${URL}"
curl -s -L -o "${FILE}" "${URL}"

# echo "Done. CA bundle at: ${FILE}"
