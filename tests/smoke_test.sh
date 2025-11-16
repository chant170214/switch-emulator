#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <build_dir>" >&2
  exit 1
fi

BUILD_DIR="$1"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$BUILD_DIR/switch-emulator"
PROGRAM="$REPO_ROOT/examples/hello_homebrew.asm"
ARTIFACT_DIR="$BUILD_DIR/smoke_artifacts"

if [[ ! -x "$BINARY" ]]; then
  echo "Expected emulator binary at $BINARY" >&2
  exit 1
fi

mkdir -p "$ARTIFACT_DIR"
FRAMEBUFFER="$ARTIFACT_DIR/hello.ppm"
ANSI_DUMP="$ARTIFACT_DIR/hello_ansi.txt"
STDIN_ANSI="$ARTIFACT_DIR/stdin_ansi.txt"
PACKAGE="$ARTIFACT_DIR/hello_homebrew.nro"
ROUNDTRIP_FRAMEBUFFER="$ARTIFACT_DIR/hello_from_nro.ppm"

# Run the assembler-driven workload, dump a framebuffer, and save the ANSI preview.
"$BINARY" "$PROGRAM" --dump "$FRAMEBUFFER" --show-ansi --show-ansi-width 48 > "$ANSI_DUMP"

test -s "$FRAMEBUFFER"
test -s "$ANSI_DUMP"

# Ensure stdin assembly loading works for quick iterations.
cat "$PROGRAM" | "$BINARY" --stdin --show-ansi-mono --show-ansi-width 32 > "$STDIN_ANSI"
test -s "$STDIN_ANSI"

# Package the workload as a Toy NRO and ensure it can be executed as well.
"$BINARY" "$PROGRAM" --emit-nro "$PACKAGE"

"$BINARY" "$PACKAGE" --dump "$ROUNDTRIP_FRAMEBUFFER"

test -s "$PACKAGE"
test -s "$ROUNDTRIP_FRAMEBUFFER"
