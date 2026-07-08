#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOT_DIR="$ROOT_DIR/tests/minecraft-bot"

if [ "${RUN_MINECRAFT_E2E_TESTS:-0}" != "1" ]; then
  echo "[WARN] Tests Minecraft E2E ignores: definir RUN_MINECRAFT_E2E_TESTS=1 pour se connecter a un serveur reel."
  exit 0
fi

if ! command -v node >/dev/null 2>&1; then
  echo "[FAIL] Node.js est requis pour le bot Mineflayer."
  exit 2
fi

if [ ! -d "$BOT_DIR/node_modules" ]; then
  echo "[FAIL] Dependances absentes. Executez:"
  echo "  cd tests/minecraft-bot && npm install"
  exit 2
fi

cd "$BOT_DIR"
exec npm test
