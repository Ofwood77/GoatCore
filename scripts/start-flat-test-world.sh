#!/bin/sh
set -eu

repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
server_bin="$repo_root/mc_server"

if [ ! -x "$server_bin" ]; then
    echo "Serveur absent : lancez 'make mc_server' depuis $repo_root" >&2
    exit 1
fi

# A fresh path is important: existing chunks and player positions are persistent
# and may come from an older void world. Never overwrite the user's world.
flat_world=$(mktemp -d "${TMPDIR:-/tmp}/goatcore-flat-XXXXXX")
export MC_WORLD_PATH="$flat_world"
export MC_BIND_PORT="${MC_BIND_PORT:-25565}"

echo "Monde plat neuf : $MC_WORLD_PATH"
echo "Sol : Y=63 ; apparition : Y=64 ; port : $MC_BIND_PORT"
echo "Le monde existant du depot n'est pas modifie. Ctrl-C arrete le serveur."

cd "$repo_root"
exec "$server_bin"
