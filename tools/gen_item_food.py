#!/usr/bin/env python3
import json
import sys
from pathlib import Path


FLAG_PRESENT = 1 << 0
FLAG_ALWAYS_EDIBLE = 1 << 1
FLAG_HAS_REMAINDER = 1 << 2
FLAG_IGNORES_EFFECTS = 1 << 3


def fail(message: str) -> None:
    print(f"[error] {message}", file=sys.stderr)
    raise SystemExit(2)


def load_item_map(path: Path) -> tuple[dict[str, int], int]:
    data = json.loads(path.read_text(encoding="utf-8"))
    items = data.get("items")
    if not isinstance(items, list):
        fail(f"{path} does not contain an items array")

    item_id_by_name: dict[str, int] = {}
    max_item_id = -1
    for item in items:
        if not isinstance(item, dict):
            continue
        name = item.get("name")
        item_id = item.get("id")
        if isinstance(name, str) and isinstance(item_id, int):
            item_id_by_name[name] = item_id
            if item_id > max_item_id:
                max_item_id = item_id
    if max_item_id < 0:
        fail(f"{path} does not define any item ids")
    return item_id_by_name, max_item_id


def entry_literal(entry: dict[str, object]) -> str:
    saturation = format(float(entry["saturation"]), ".7g")
    if "." not in saturation and "e" not in saturation and "E" not in saturation:
        saturation += ".0"
    return f"{{{int(entry['nutrition'])}, {saturation}f, {int(entry['remainder_item_id'])}, {int(entry['flags'])}}}"


def main() -> int:
    if len(sys.argv) != 5:
        print(
            "usage: gen_item_food.py <generated_minecraft_ids.json> <components_item_dir> <out.c> <out.h>",
            file=sys.stderr,
        )
        return 2

    ids_path = Path(sys.argv[1])
    components_dir = Path(sys.argv[2])
    out_c = Path(sys.argv[3])
    out_h = Path(sys.argv[4])

    item_id_by_name, max_item_id = load_item_map(ids_path)
    table_size = max_item_id + 1
    entries = [
        {"nutrition": 0, "saturation": 0.0, "remainder_item_id": -1, "flags": 0}
        for _ in range(table_size)
    ]

    supported = 0
    ignored = 0
    for path in sorted(components_dir.glob("*.json")):
        item_name = f"minecraft:{path.stem}"
        item_id = item_id_by_name.get(item_name)
        if item_id is None:
            ignored += 1
            continue

        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            ignored += 1
            continue

        components = data.get("components")
        if not isinstance(components, dict):
            ignored += 1
            continue

        food = components.get("minecraft:food")
        consumable = components.get("minecraft:consumable")
        if not isinstance(food, dict) or not isinstance(consumable, dict):
            ignored += 1
            continue

        nutrition = food.get("nutrition")
        saturation = food.get("saturation")
        if not isinstance(nutrition, (int, float)) or not isinstance(saturation, (int, float)):
            ignored += 1
            continue

        flags = FLAG_PRESENT
        if food.get("can_always_eat") is True:
            flags |= FLAG_ALWAYS_EDIBLE

        remainder_item_id = -1
        remainder = components.get("minecraft:use_remainder")
        if isinstance(remainder, dict):
            remainder_name = remainder.get("id")
            if isinstance(remainder_name, str):
                remainder_item_id = item_id_by_name.get(remainder_name, -1)
                if remainder_item_id >= 0:
                    flags |= FLAG_HAS_REMAINDER

        on_consume_effects = consumable.get("on_consume_effects")
        if isinstance(on_consume_effects, list) and on_consume_effects:
            flags |= FLAG_IGNORES_EFFECTS

        entries[item_id] = {
            "nutrition": int(nutrition),
            "saturation": float(saturation),
            "remainder_item_id": remainder_item_id,
            "flags": flags,
        }
        supported += 1

    out_h.write_text(
        "\n".join(
            [
                "#ifndef GENERATED_ITEM_FOOD_H",
                "#define GENERATED_ITEM_FOOD_H",
                "",
                "#include <stdint.h>",
                "",
                f"#define MC_ITEM_FOOD_TABLE_SIZE {table_size}",
                "",
                "enum {",
                "    MC_ITEM_FOOD_FLAG_PRESENT = 1u << 0,",
                "    MC_ITEM_FOOD_FLAG_ALWAYS_EDIBLE = 1u << 1,",
                "    MC_ITEM_FOOD_FLAG_HAS_REMAINDER = 1u << 2,",
                "    MC_ITEM_FOOD_FLAG_IGNORES_EFFECTS = 1u << 3,",
                "};",
                "",
                "typedef struct {",
                "    int16_t nutrition;",
                "    float saturation;",
                "    int32_t remainder_item_id;",
                "    uint16_t flags;",
                "} mc_item_food_entry_t;",
                "",
                "const mc_item_food_entry_t *mc_item_food_entry(int32_t item_id);",
                "",
                "#endif /* GENERATED_ITEM_FOOD_H */",
                "",
            ]
        ),
        encoding="utf-8",
    )

    lines = [
        '#include "generated_item_food.h"',
        "#include <stddef.h>",
        "",
        f"static const mc_item_food_entry_t mc_item_food_table[MC_ITEM_FOOD_TABLE_SIZE] = {{",
    ]
    for item_name, item_id in sorted(item_id_by_name.items(), key=lambda kv: kv[1]):
        entry = entries[item_id]
        lines.append(f"    [{item_id}] = {entry_literal(entry)}, /* {item_name} */")
    lines.extend(
        [
            "};",
            "",
            "const mc_item_food_entry_t *mc_item_food_entry(int32_t item_id) {",
            "    if (item_id < 0 || item_id >= MC_ITEM_FOOD_TABLE_SIZE) return NULL;",
            "    return &mc_item_food_table[item_id];",
            "}",
            "",
        ]
    )
    out_c.write_text("\n".join(lines), encoding="utf-8")

    print(f"[gen_item_food] generated {supported} edible items, ignored {ignored} component files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
