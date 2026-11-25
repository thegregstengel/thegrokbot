#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -f "$ROOT/secrets.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "$ROOT/secrets.env"
  set +a
fi

if [[ -z "${XAI_API_KEY:-}" ]]; then
  echo "XAI_API_KEY is not set. Add it to $ROOT/secrets.env or export it."
  exit 1
fi

BIN="$ROOT/brain/target/release/grokbot-brain"
if [[ ! -x "$BIN" ]]; then
  if command -v cargo >/dev/null 2>&1; then
    (cd "$ROOT/brain" && cargo build --release)
  elif command -v mise >/dev/null 2>&1; then
    (cd "$ROOT/brain" && mise exec rust@latest -- cargo build --release)
  else
    echo "Rust cargo not found. Install Rust or mise before running."
    exit 1
  fi
fi

exec "$BIN" "$@"
