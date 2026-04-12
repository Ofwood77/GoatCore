#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

DATA_REPORTS_DIR=${DATA_REPORTS_DIR:-data/26.1.1/reports}
MC_IDS_SOURCE=${MC_IDS_SOURCE:-$DATA_REPORTS_DIR}
MC_BLOCK_LOOT_SOURCE=${MC_BLOCK_LOOT_SOURCE:-mc_vania_asset/client/data/minecraft/loot_table/blocks}
MC_RECIPE_SOURCE=${MC_RECIPE_SOURCE:-mc_vania_asset/client/data/minecraft/recipe}
MC_ITEM_TAG_SOURCE=${MC_ITEM_TAG_SOURCE:-mc_vania_asset/client/data/minecraft/tags/item}
COMPONENTS_ITEM_DIR=${COMPONENTS_ITEM_DIR:-$DATA_REPORTS_DIR/minecraft/components/item}
REQUIRE_RAW_DATA=${REQUIRE_RAW_DATA:-0}

generated_files="
src/world/block_registry.c
src/world/block_registry.h
src/generated/generated_minecraft_ids.c
src/generated/generated_minecraft_ids.h
src/generated/generated_minecraft_ids.json
src/generated/generated_registries.c
src/generated/generated_registries.h
src/generated/generated_block_loot.c
src/generated/generated_block_loot.h
src/generated/generated_item_food.c
src/generated/generated_item_food.h
src/generated/generated_crafting_recipes.c
src/generated/generated_crafting_recipes.h
src/generated/generated_cooking_recipes.c
src/generated/generated_cooking_recipes.h
src/generated/generated_item_place.c
src/generated/generated_item_place.h
"

missing_generated=0
for path in $generated_files; do
    if [ ! -f "$path" ]; then
        echo "[generated-check] missing generated file: $path" >&2
        missing_generated=1
    fi
done
if [ "$missing_generated" -ne 0 ]; then
    exit 1
fi

missing_raw=0
for path in \
    "$DATA_REPORTS_DIR/blocks.json" \
    "$MC_IDS_SOURCE" \
    "$MC_BLOCK_LOOT_SOURCE" \
    "$MC_RECIPE_SOURCE" \
    "$MC_ITEM_TAG_SOURCE" \
    "$COMPONENTS_ITEM_DIR"
do
    if [ ! -e "$path" ]; then
        echo "[generated-check] raw source unavailable: $path" >&2
        missing_raw=1
    fi
done

if [ "$missing_raw" -ne 0 ]; then
    if [ "$REQUIRE_RAW_DATA" = "1" ]; then
        echo "[generated-check] raw data is required; refusing to skip regeneration" >&2
        exit 1
    fi
    echo "[generated-check] raw data unavailable; validated generated files only"
    exit 0
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/mc_c_server_generated_check.XXXXXX")
cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$tmp_dir/src/world" "$tmp_dir/src/generated"

echo "[generated-check] regenerating into $tmp_dir"
python3 tools/generate_registry.py "$DATA_REPORTS_DIR/blocks.json" "$tmp_dir/src/world/block_registry.c" "$tmp_dir/src/world/block_registry.h"
python3 tools/gen_minecraft_ids.py "$MC_IDS_SOURCE" "$tmp_dir/src/generated/generated_minecraft_ids.c" "$tmp_dir/src/generated/generated_minecraft_ids.h" "$tmp_dir/src/generated/generated_minecraft_ids.json"
python3 tools/gen_registries.py "$tmp_dir/src/world/block_registry.h" "$tmp_dir/src/generated/generated_registries.c" "$tmp_dir/src/generated/generated_registries.h"
python3 tools/gen_block_loot.py "$tmp_dir/src/generated/generated_minecraft_ids.json" "$DATA_REPORTS_DIR/blocks.json" "$MC_BLOCK_LOOT_SOURCE" "$tmp_dir/src/generated/generated_block_loot.c" "$tmp_dir/src/generated/generated_block_loot.h"
python3 tools/gen_item_food.py "$tmp_dir/src/generated/generated_minecraft_ids.json" "$COMPONENTS_ITEM_DIR" "$tmp_dir/src/generated/generated_item_food.c" "$tmp_dir/src/generated/generated_item_food.h"
python3 tools/gen_crafting_recipes.py "$tmp_dir/src/generated/generated_minecraft_ids.json" "$MC_RECIPE_SOURCE" "$MC_ITEM_TAG_SOURCE" "$tmp_dir/src/generated/generated_crafting_recipes.c" "$tmp_dir/src/generated/generated_crafting_recipes.h"
python3 tools/gen_cooking_recipes.py "$tmp_dir/src/generated/generated_minecraft_ids.json" "$MC_RECIPE_SOURCE" "$MC_ITEM_TAG_SOURCE" "$tmp_dir/src/generated/generated_cooking_recipes.c" "$tmp_dir/src/generated/generated_cooking_recipes.h"
python3 tools/gen_item_place_map.py "$tmp_dir/src/generated/generated_minecraft_ids.json" "$DATA_REPORTS_DIR/blocks.json" "$tmp_dir/src/generated/generated_item_place.c" "$tmp_dir/src/generated/generated_item_place.h"

failed=0
for path in $generated_files; do
    if ! diff -u "$path" "$tmp_dir/$path"; then
        failed=1
    fi
done

if [ "$failed" -ne 0 ]; then
    echo "[generated-check] generated files differ; run make regenerate intentionally" >&2
    exit 1
fi

echo "[generated-check] generated files are up to date"
