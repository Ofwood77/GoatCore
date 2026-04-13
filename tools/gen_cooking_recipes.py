#!/usr/bin/env python3
import json
import sys
from pathlib import Path


def load_item_ids(path):
    data = json.load(open(path, "r", encoding="utf-8"))
    return {entry["name"]: int(entry["id"]) for entry in data.get("items", [])}


def tag_path(tags_dir, tag_name):
    if tag_name.startswith("#"):
        tag_name = tag_name[1:]
    if tag_name.startswith("minecraft:"):
        tag_name = tag_name[len("minecraft:"):]
    return tags_dir / f"{tag_name}.json"


def expand_tag(tags_dir, item_ids, tag_name, seen=None):
    if seen is None:
        seen = set()
    key = tag_name[1:] if tag_name.startswith("#") else tag_name
    if key in seen:
        return []
    seen.add(key)

    path = tag_path(tags_dir, key)
    if not path.exists():
        return []
    data = json.load(open(path, "r", encoding="utf-8"))
    out = []
    for value in data.get("values", []):
        if isinstance(value, dict):
            value = value.get("id") if value.get("required", True) else value.get("id")
        if not isinstance(value, str):
            continue
        if value.startswith("#"):
            out.extend(expand_tag(tags_dir, item_ids, value, seen))
        else:
            item_id = item_ids.get(value)
            if item_id is not None:
                out.append(item_id)
    return out


def ingredient_ids(value, tags_dir, item_ids):
    values = value if isinstance(value, list) else [value]
    out = []
    for item in values:
        if isinstance(item, dict):
            item = item.get("item") or item.get("id") or item.get("tag")
            if item and not item.startswith("minecraft:") and not item.startswith("#"):
                item = f"#{item}"
        if not isinstance(item, str):
            continue
        if item.startswith("#"):
            out.extend(expand_tag(tags_dir, item_ids, item))
        else:
            item_id = item_ids.get(item)
            if item_id is not None:
                out.append(item_id)
    return sorted(set(out))


def parse_recipe(path, tags_dir, item_ids, ingredient_items):
    data = json.load(open(path, "r", encoding="utf-8"))
    recipe_type = data.get("type")
    type_id = {
        "minecraft:smelting": 1,
        "minecraft:smoking": 2,
        "minecraft:blasting": 3,
    }.get(recipe_type)
    if type_id is None:
        return None

    result = data.get("result")
    if not isinstance(result, dict) or "components" in result:
        return None
    result_name = result.get("id")
    result_id = item_ids.get(result_name)
    if result_id is None:
        return None
    result_count = int(result.get("count", 1))
    if result_count <= 0 or result_count > 255:
        return None

    ids = ingredient_ids(data.get("ingredient"), tags_dir, item_ids)
    if not ids:
        return None
    cook_time = int(data.get("cookingtime", 200))
    if cook_time <= 0 or cook_time > 32767:
        cook_time = 200

    offset = len(ingredient_items)
    ingredient_items.extend(ids)
    return {
        "type": type_id,
        "ingredient_offset": offset,
        "ingredient_count": len(ids),
        "result_item_id": result_id,
        "result_count": result_count,
        "cook_time": cook_time,
        "name": path.stem,
    }


def add_fuel(fuels, item_ids, name, burn_ticks, remainder=None):
    item_id = item_ids.get(name)
    if item_id is None:
        return
    remainder_id = item_ids.get(remainder, -1) if remainder else -1
    fuels[item_id] = (burn_ticks, remainder_id, name)


def add_fuel_tag(fuels, tags_dir, item_ids, tag_name, burn_ticks, exclude_tag=None):
    excluded = set(expand_tag(tags_dir, item_ids, exclude_tag)) if exclude_tag else set()
    for item_id in expand_tag(tags_dir, item_ids, tag_name):
        if item_id in excluded:
            continue
        fuels.setdefault(item_id, (burn_ticks, -1, tag_name))


def build_fuels(tags_dir, item_ids):
    fuels = {}

    add_fuel(fuels, item_ids, "minecraft:coal", 1600)
    add_fuel(fuels, item_ids, "minecraft:charcoal", 1600)
    add_fuel(fuels, item_ids, "minecraft:coal_block", 16000)
    add_fuel(fuels, item_ids, "minecraft:lava_bucket", 20000, "minecraft:bucket")
    add_fuel(fuels, item_ids, "minecraft:blaze_rod", 2400)
    add_fuel(fuels, item_ids, "minecraft:dried_kelp_block", 4000)
    add_fuel(fuels, item_ids, "minecraft:stick", 100)
    add_fuel(fuels, item_ids, "minecraft:bamboo", 50)
    add_fuel(fuels, item_ids, "minecraft:bowl", 100)
    add_fuel(fuels, item_ids, "minecraft:crafting_table", 300)
    add_fuel(fuels, item_ids, "minecraft:chest", 300)
    add_fuel(fuels, item_ids, "minecraft:barrel", 300)
    add_fuel(fuels, item_ids, "minecraft:ladder", 300)

    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:logs_that_burn", 300)
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:planks", 300, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_slabs", 150, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_stairs", 300, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_fences", 300, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_fence_gates", 300, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_doors", 200, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_trapdoors", 300, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_pressure_plates", 300, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:wooden_buttons", 100, "#minecraft:non_flammable_wood")
    add_fuel_tag(fuels, tags_dir, item_ids, "#minecraft:saplings", 100)
    return fuels


def emit_header(out_h):
    out_h.write("""#ifndef GENERATED_COOKING_RECIPES_H
#define GENERATED_COOKING_RECIPES_H

#include <stddef.h>
#include <stdint.h>

enum {
    MC_COOKING_RECIPE_SMELTING = 1,
    MC_COOKING_RECIPE_SMOKING = 2,
    MC_COOKING_RECIPE_BLASTING = 3
};

typedef struct {
    uint8_t type;
    uint32_t ingredient_offset;
    uint16_t ingredient_count;
    int32_t result_item_id;
    uint8_t result_count;
    int16_t cook_time;
} mc_cooking_recipe_t;

typedef struct {
    int32_t item_id;
    int32_t burn_ticks;
    int32_t remainder_item_id;
} mc_fuel_entry_t;

extern const mc_cooking_recipe_t MC_COOKING_RECIPES[];
extern const size_t MC_COOKING_RECIPE_COUNT;
extern const int32_t MC_COOKING_INGREDIENT_ITEMS[];
extern const size_t MC_COOKING_INGREDIENT_ITEM_COUNT;
extern const mc_fuel_entry_t MC_FUEL_ENTRIES[];
extern const size_t MC_FUEL_ENTRY_COUNT;

#endif /* GENERATED_COOKING_RECIPES_H */
""")


def emit_source(out_c, recipes, ingredient_items, fuels):
    out_c.write('#include "generated_cooking_recipes.h"\n\n')
    out_c.write("const int32_t MC_COOKING_INGREDIENT_ITEMS[] = {\n")
    for i in range(0, len(ingredient_items), 8):
        out_c.write("    " + ",     ".join(str(item_id) for item_id in ingredient_items[i:i + 8]) + ",\n")
    out_c.write("};\n")
    out_c.write("const size_t MC_COOKING_INGREDIENT_ITEM_COUNT = sizeof(MC_COOKING_INGREDIENT_ITEMS) / sizeof(MC_COOKING_INGREDIENT_ITEMS[0]);\n\n")

    out_c.write("const mc_cooking_recipe_t MC_COOKING_RECIPES[] = {\n")
    for recipe in recipes:
        out_c.write(
            "    {%du, %du, %du, %d, %du, %d}, /* %s */\n"
            % (
                recipe["type"],
                recipe["ingredient_offset"],
                recipe["ingredient_count"],
                recipe["result_item_id"],
                recipe["result_count"],
                recipe["cook_time"],
                recipe["name"],
            )
        )
    out_c.write("};\n")
    out_c.write("const size_t MC_COOKING_RECIPE_COUNT = sizeof(MC_COOKING_RECIPES) / sizeof(MC_COOKING_RECIPES[0]);\n\n")

    out_c.write("const mc_fuel_entry_t MC_FUEL_ENTRIES[] = {\n")
    for item_id, (burn_ticks, remainder_id, source) in sorted(fuels.items()):
        out_c.write(f"    {{{item_id}, {burn_ticks}, {remainder_id}}}, /* {source} */\n")
    out_c.write("};\n")
    out_c.write("const size_t MC_FUEL_ENTRY_COUNT = sizeof(MC_FUEL_ENTRIES) / sizeof(MC_FUEL_ENTRIES[0]);\n")


def main(argv):
    if len(argv) != 6:
        print("usage: gen_cooking_recipes.py minecraft_ids.json recipe_dir tags_item_dir out.c out.h", file=sys.stderr)
        return 2

    ids_path = Path(argv[1])
    recipe_dir = Path(argv[2])
    tags_dir = Path(argv[3])
    out_c_path = Path(argv[4])
    out_h_path = Path(argv[5])

    item_ids = load_item_ids(ids_path)
    ingredient_items = []
    recipes = []
    skipped = 0
    for path in sorted(recipe_dir.glob("*.json")):
        recipe = parse_recipe(path, tags_dir, item_ids, ingredient_items)
        if recipe is None:
            skipped += 1
            continue
        recipes.append(recipe)

    fuels = build_fuels(tags_dir, item_ids)

    out_c_path.parent.mkdir(parents=True, exist_ok=True)
    out_h_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_h_path, "w", encoding="utf-8") as out_h:
        emit_header(out_h)
    with open(out_c_path, "w", encoding="utf-8") as out_c:
        emit_source(out_c, recipes, ingredient_items, fuels)

    print(
        f"generated cooking recipes: recipes={len(recipes)} ingredient_items={len(ingredient_items)} "
        f"fuels={len(fuels)} skipped={skipped}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
