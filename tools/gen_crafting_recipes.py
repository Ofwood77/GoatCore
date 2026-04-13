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
        if not isinstance(item, str):
            continue
        if item.startswith("#"):
            out.extend(expand_tag(tags_dir, item_ids, item))
        else:
            item_id = item_ids.get(item)
            if item_id is not None:
                out.append(item_id)
    return sorted(set(out))


def add_ingredient(ingredients, ingredient_items, ids):
    offset = len(ingredient_items)
    ingredient_items.extend(ids)
    ingredients.append((offset, len(ids)))
    return len(ingredients) - 1


def parse_recipe(path, tags_dir, item_ids, ingredients, ingredient_items):
    data = json.load(open(path, "r", encoding="utf-8"))
    recipe_type = data.get("type")
    if recipe_type not in ("minecraft:crafting_shaped", "minecraft:crafting_shapeless"):
        return None

    result = data.get("result")
    if not isinstance(result, dict):
        return None
    if "components" in result:
        return None
    result_name = result.get("id")
    result_id = item_ids.get(result_name)
    if result_id is None:
        return None
    result_count = int(result.get("count", 1))
    if result_count <= 0 or result_count > 255:
        return None

    if recipe_type == "minecraft:crafting_shaped":
        pattern = data.get("pattern")
        key = data.get("key")
        if not isinstance(pattern, list) or not pattern or not isinstance(key, dict):
            return None
        if len(pattern) > 3:
            return None
        width = max(len(row) for row in pattern if isinstance(row, str))
        height = len(pattern)
        if width <= 0 or width > 3:
            return None

        char_ingredients = {}
        for char, value in key.items():
            if not isinstance(char, str) or len(char) != 1:
                return None
            ids = ingredient_ids(value, tags_dir, item_ids)
            if not ids:
                return None
            char_ingredients[char] = ids

        ingredient_refs = []
        for row in pattern:
            if not isinstance(row, str):
                return None
            padded = row.ljust(width)
            for char in padded:
                if char == " ":
                    ingredient_refs.append(add_ingredient(ingredients, ingredient_items, []))
                    continue
                ids = char_ingredients.get(char)
                if not ids:
                    return None
                ingredient_refs.append(add_ingredient(ingredients, ingredient_items, ids))

        return {
            "type": 1,
            "width": width,
            "height": height,
            "ingredient_count": len(ingredient_refs),
            "ingredient_offset": ingredient_refs[0],
            "result_item_id": result_id,
            "result_count": result_count,
            "name": path.stem,
        }

    raw_ingredients = data.get("ingredients")
    if not isinstance(raw_ingredients, list) or not raw_ingredients or len(raw_ingredients) > 9:
        return None
    ingredient_refs = []
    for value in raw_ingredients:
        ids = ingredient_ids(value, tags_dir, item_ids)
        if not ids:
            return None
        ingredient_refs.append(add_ingredient(ingredients, ingredient_items, ids))

    return {
        "type": 2,
        "width": 0,
        "height": 0,
        "ingredient_count": len(ingredient_refs),
        "ingredient_offset": ingredient_refs[0],
        "result_item_id": result_id,
        "result_count": result_count,
        "name": path.stem,
    }


def emit_header(out_h):
    out_h.write("""#ifndef GENERATED_CRAFTING_RECIPES_H
#define GENERATED_CRAFTING_RECIPES_H

#include <stddef.h>
#include <stdint.h>

enum {
    MC_CRAFTING_RECIPE_SHAPED = 1,
    MC_CRAFTING_RECIPE_SHAPELESS = 2
};

typedef struct {
    uint32_t item_offset;
    uint16_t item_count;
} mc_crafting_ingredient_t;

typedef struct {
    uint8_t type;
    uint8_t width;
    uint8_t height;
    uint8_t ingredient_count;
    uint32_t ingredient_offset;
    int32_t result_item_id;
    uint8_t result_count;
} mc_crafting_recipe_t;

extern const mc_crafting_recipe_t MC_CRAFTING_RECIPES[];
extern const size_t MC_CRAFTING_RECIPE_COUNT;
extern const mc_crafting_ingredient_t MC_CRAFTING_INGREDIENTS[];
extern const size_t MC_CRAFTING_INGREDIENT_COUNT;
extern const int32_t MC_CRAFTING_INGREDIENT_ITEMS[];
extern const size_t MC_CRAFTING_INGREDIENT_ITEM_COUNT;

#endif /* GENERATED_CRAFTING_RECIPES_H */
""")


def emit_source(out_c, recipes, ingredients, ingredient_items):
    out_c.write('#include "generated_crafting_recipes.h"\n\n')
    out_c.write("const int32_t MC_CRAFTING_INGREDIENT_ITEMS[] = {\n")
    for i in range(0, len(ingredient_items), 8):
        out_c.write("    " + ",     ".join(str(item_id) for item_id in ingredient_items[i:i + 8]) + ",\n")
    out_c.write("};\n")
    out_c.write("const size_t MC_CRAFTING_INGREDIENT_ITEM_COUNT = sizeof(MC_CRAFTING_INGREDIENT_ITEMS) / sizeof(MC_CRAFTING_INGREDIENT_ITEMS[0]);\n\n")

    out_c.write("const mc_crafting_ingredient_t MC_CRAFTING_INGREDIENTS[] = {\n")
    for offset, count in ingredients:
        out_c.write(f"    {{{offset}u, {count}u}},\n")
    out_c.write("};\n")
    out_c.write("const size_t MC_CRAFTING_INGREDIENT_COUNT = sizeof(MC_CRAFTING_INGREDIENTS) / sizeof(MC_CRAFTING_INGREDIENTS[0]);\n\n")

    out_c.write("const mc_crafting_recipe_t MC_CRAFTING_RECIPES[] = {\n")
    for recipe in recipes:
        out_c.write(
            "    {%du, %du, %du, %du, %du, %d, %du}, /* %s */\n"
            % (
                recipe["type"],
                recipe["width"],
                recipe["height"],
                recipe["ingredient_count"],
                recipe["ingredient_offset"],
                recipe["result_item_id"],
                recipe["result_count"],
                recipe["name"],
            )
        )
    out_c.write("};\n")
    out_c.write("const size_t MC_CRAFTING_RECIPE_COUNT = sizeof(MC_CRAFTING_RECIPES) / sizeof(MC_CRAFTING_RECIPES[0]);\n")


def main(argv):
    if len(argv) != 6:
        print("usage: gen_crafting_recipes.py minecraft_ids.json recipe_dir tags_item_dir out.c out.h", file=sys.stderr)
        return 2

    ids_path = Path(argv[1])
    recipe_dir = Path(argv[2])
    tags_dir = Path(argv[3])
    out_c_path = Path(argv[4])
    out_h_path = Path(argv[5])

    item_ids = load_item_ids(ids_path)
    ingredients = []
    ingredient_items = []
    recipes = []
    skipped = 0
    for path in sorted(recipe_dir.glob("*.json")):
        recipe = parse_recipe(path, tags_dir, item_ids, ingredients, ingredient_items)
        if recipe is None:
            skipped += 1
            continue
        recipes.append(recipe)

    out_c_path.parent.mkdir(parents=True, exist_ok=True)
    out_h_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_h_path, "w", encoding="utf-8") as out_h:
        emit_header(out_h)
    with open(out_c_path, "w", encoding="utf-8") as out_c:
        emit_source(out_c, recipes, ingredients, ingredient_items)

    print(
        f"generated crafting recipes: recipes={len(recipes)} ingredients={len(ingredients)} "
        f"ingredient_items={len(ingredient_items)} skipped={skipped}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
