#!/bin/bash
set -e

# Pinned nightly (not a rolling "nightly"): build-rust-app.sh's use of
# `-Z build-std`/`-Z json-target-spec` is unstable cargo/rustc surface
# that can and does shift between nightlies -- the whole Rust port
# (port/rust_port/x86_64-baremetal-firecracker.json,
# port/rust_port/unwind_stub.c) was verified against this exact one.
# Bump deliberately, the same way get-musl.sh pins VERSION="1.2.6".
RUST_NIGHTLY="nightly-2026-09-14"

if ! command -v rustup > /dev/null 2>&1; then
	echo "- Installing rustup"
	curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain none --profile minimal
	# shellcheck disable=SC1091
	source "$HOME/.cargo/env"
fi

echo "- Installing Rust $RUST_NIGHTLY + rust-src"
rustup toolchain install "$RUST_NIGHTLY" --profile minimal --component rust-src

echo "$RUST_NIGHTLY" > "$(dirname "${BASH_SOURCE[0]}")/../port/rust_port/PINNED_TOOLCHAIN"
