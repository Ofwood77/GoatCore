#!/usr/bin/env python3
import io
import json
import sys
import zipfile
from pathlib import Path


CATEGORY_NONE = 0
CATEGORY_PICKAXE = 1
CATEGORY_AXE = 2
CATEGORY_SHOVEL = 3
CATEGORY_HOE = 4

HARVEST_NONE = 0
HARVEST_STONE = 1
HARVEST_IRON = 2
HARVEST_DIAMOND = 3

MATERIAL_NONE = 0
MATERIAL_WOOD = 1
MATERIAL_STONE = 2
MATERIAL_COPPER = 3
MATERIAL_IRON = 4
MATERIAL_GOLD = 5
MATERIAL_DIAMOND = 6
MATERIAL_NETHERITE = 7

SPEED_SCALE = 100
FLAG_PRESENT = 1 << 0
FLAG_REQUIRES_CORRECT_TOOL = 1 << 1


BLOCK_CATEGORY_TAGS = (
    (CATEGORY_PICKAXE, "MC_MINING_TOOL_CATEGORY_PICKAXE", "minecraft:mineable/pickaxe"),
    (CATEGORY_AXE, "MC_MINING_TOOL_CATEGORY_AXE", "minecraft:mineable/axe"),
    (CATEGORY_SHOVEL, "MC_MINING_TOOL_CATEGORY_SHOVEL", "minecraft:mineable/shovel"),
    (CATEGORY_HOE, "MC_MINING_TOOL_CATEGORY_HOE", "minecraft:mineable/hoe"),
)

REQUIRED_LEVEL_TAGS = (
    (HARVEST_STONE, "MC_MINING_HARVEST_LEVEL_STONE", "minecraft:needs_stone_tool"),
    (HARVEST_IRON, "MC_MINING_HARVEST_LEVEL_IRON", "minecraft:needs_iron_tool"),
    (HARVEST_DIAMOND, "MC_MINING_HARVEST_LEVEL_DIAMOND", "minecraft:needs_diamond_tool"),
)

ITEM_CATEGORY_TAGS = (
    (CATEGORY_PICKAXE, "minecraft:pickaxes"),
    (CATEGORY_AXE, "minecraft:axes"),
    (CATEGORY_SHOVEL, "minecraft:shovels"),
    (CATEGORY_HOE, "minecraft:hoes"),
)

PICKAXE_DROP_EXEMPT_TAGS = (
    "minecraft:buttons",
    "minecraft:pressure_plates",
    "minecraft:rails",
)

MATERIALS = {
    "wooden": ("MC_MINING_TOOL_MATERIAL_WOOD", MATERIAL_WOOD, HARVEST_NONE, 2.0, 59),
    "stone": ("MC_MINING_TOOL_MATERIAL_STONE", MATERIAL_STONE, HARVEST_STONE, 4.0, 131),
    "copper": ("MC_MINING_TOOL_MATERIAL_COPPER", MATERIAL_COPPER, HARVEST_STONE, 5.0, 190),
    "iron": ("MC_MINING_TOOL_MATERIAL_IRON", MATERIAL_IRON, HARVEST_IRON, 6.0, 250),
    "golden": ("MC_MINING_TOOL_MATERIAL_GOLD", MATERIAL_GOLD, HARVEST_NONE, 12.0, 32),
    "diamond": ("MC_MINING_TOOL_MATERIAL_DIAMOND", MATERIAL_DIAMOND, HARVEST_DIAMOND, 8.0, 1561),
    "netherite": ("MC_MINING_TOOL_MATERIAL_NETHERITE", MATERIAL_NETHERITE, HARVEST_DIAMOND, 9.0, 2031),
}


def fail(message: str) -> None:
    print(f"[error] {message}", file=sys.stderr)
    raise SystemExit(2)


def namespaced(name: str) -> str:
    return name if ":" in name else f"minecraft:{name}"


def load_blocks(path: Path) -> list[str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        fail(f"{path} must contain a JSON object")
    return sorted(name for name in data if isinstance(name, str))


def load_item_id_map(path: Path) -> dict[str, int]:
    data = json.loads(path.read_text(encoding="utf-8"))
    items = data.get("items")
    if not isinstance(items, list):
        fail(f"{path} does not contain an items array")
    result: dict[str, int] = {}
    for item in items:
        if not isinstance(item, dict):
            continue
        name = item.get("name")
        item_id = item.get("id")
        if isinstance(name, str) and isinstance(item_id, int):
            result[name] = item_id
    return result


def load_server_data_jar(path: Path) -> zipfile.ZipFile:
    try:
        outer = zipfile.ZipFile(path)
    except zipfile.BadZipFile as exc:
        fail(f"{path} is not a valid jar: {exc}")
    with outer:
        version_jars = [name for name in outer.namelist() if name.startswith("META-INF/versions/") and name.endswith(".jar")]
        server_jars = [name for name in version_jars if "/server-" in name]
        candidates = server_jars or version_jars
        if not candidates:
            fail(f"{path} does not contain a bundled server jar")
        candidates.sort()
        nested = candidates[-1]
        return zipfile.ZipFile(io.BytesIO(outer.read(nested)))


def tag_path(registry: str, tag_name: str) -> str:
    namespace, value = namespaced(tag_name).split(":", 1)
    return f"data/{namespace}/tags/{registry}/{value}.json"


def read_tag_values(z: zipfile.ZipFile, registry: str, tag_name: str, seen: set[str] | None = None) -> set[str]:
    if seen is None:
        seen = set()
    tag_name = namespaced(tag_name)
    key = f"{registry}:{tag_name}"
    if key in seen:
        return set()
    seen.add(key)

    path = tag_path(registry, tag_name)
    try:
        raw = z.read(path)
    except KeyError:
        fail(f"missing tag {path}")
    data = json.loads(raw.decode("utf-8"))
    values = data.get("values")
    if not isinstance(values, list):
        fail(f"{path} does not contain a values array")

    result: set[str] = set()
    for value in values:
        required = True
        if isinstance(value, dict):
            required = bool(value.get("required", True))
            value = value.get("id")
        if not isinstance(value, str):
            fail(f"{path} contains an invalid tag value")
        if value.startswith("#"):
            try:
                result.update(read_tag_values(z, registry, value[1:], seen))
            except SystemExit:
                if required:
                    raise
            continue
        result.add(namespaced(value))
    return result


def category_literal(category: int) -> str:
    for value, literal, _tag in BLOCK_CATEGORY_TAGS:
        if value == category:
            return literal
    return "MC_MINING_TOOL_CATEGORY_NONE"


def harvest_literal(level: int) -> str:
    for value, literal, _tag in REQUIRED_LEVEL_TAGS:
        if value == level:
            return literal
    return "MC_MINING_HARVEST_LEVEL_NONE"


def material_for_item(item_name: str) -> tuple[str, int, int, int, int] | None:
    short = item_name.split(":", 1)[1] if ":" in item_name else item_name
    for prefix, material in MATERIALS.items():
        if short.startswith(f"{prefix}_"):
            literal, material_id, harvest_level, speed, durability = material
            return literal, material_id, harvest_level, int(round(speed * SPEED_SCALE)), durability
    return None


def block_flags_literal(flags: int) -> str:
    names: list[str] = []
    if flags & FLAG_PRESENT:
        names.append("MC_MINING_BLOCK_TOOL_FLAG_PRESENT")
    if flags & FLAG_REQUIRES_CORRECT_TOOL:
        names.append("MC_MINING_BLOCK_TOOL_FLAG_REQUIRES_CORRECT_TOOL")
    return " | ".join(names) if names else "0"


def generate_header(out_h: Path, block_count: int) -> None:
    out_h.write_text(
        f"""#ifndef GENERATED_MINING_DATA_H
#define GENERATED_MINING_DATA_H

#include <stdbool.h>
#include <stdint.h>
#include \"block_registry.h\"
#include \"generated_minecraft_ids.h\"

#define MC_MINING_BLOCK_TOOL_TABLE_SIZE {block_count}
#define MC_MINING_TOOL_SPEED_SCALE {SPEED_SCALE}

typedef enum {{
    MC_MINING_TOOL_CATEGORY_NONE = 0,
    MC_MINING_TOOL_CATEGORY_PICKAXE = 1,
    MC_MINING_TOOL_CATEGORY_AXE = 2,
    MC_MINING_TOOL_CATEGORY_SHOVEL = 3,
    MC_MINING_TOOL_CATEGORY_HOE = 4
}} mc_mining_tool_category_t;

typedef enum {{
    MC_MINING_HARVEST_LEVEL_NONE = 0,
    MC_MINING_HARVEST_LEVEL_STONE = 1,
    MC_MINING_HARVEST_LEVEL_IRON = 2,
    MC_MINING_HARVEST_LEVEL_DIAMOND = 3
}} mc_mining_harvest_level_t;

typedef enum {{
    MC_MINING_TOOL_MATERIAL_NONE = 0,
    MC_MINING_TOOL_MATERIAL_WOOD = 1,
    MC_MINING_TOOL_MATERIAL_STONE = 2,
    MC_MINING_TOOL_MATERIAL_COPPER = 3,
    MC_MINING_TOOL_MATERIAL_IRON = 4,
    MC_MINING_TOOL_MATERIAL_GOLD = 5,
    MC_MINING_TOOL_MATERIAL_DIAMOND = 6,
    MC_MINING_TOOL_MATERIAL_NETHERITE = 7
}} mc_mining_tool_material_t;

enum {{
    MC_MINING_BLOCK_TOOL_FLAG_PRESENT = 1u << 0,
    MC_MINING_BLOCK_TOOL_FLAG_REQUIRES_CORRECT_TOOL = 1u << 1
}};

typedef struct {{
    uint8_t category;
    uint8_t required_level;
    uint16_t flags;
}} mc_mining_block_tool_entry_t;

typedef struct {{
    uint8_t category;
    uint8_t harvest_level;
    uint8_t material;
    uint8_t reserved0;
    uint16_t speed_x100;
    uint16_t durability;
}} mc_mining_tool_item_entry_t;

const mc_mining_block_tool_entry_t *mc_mining_block_tool_entry_from_state(int state_id);
const mc_mining_tool_item_entry_t *mc_mining_tool_item_entry_from_item(int32_t item_id);
const char *mc_mining_tool_category_name(mc_mining_tool_category_t category);

#endif /* GENERATED_MINING_DATA_H */
""",
        encoding="utf-8",
    )


def generate_source(out_c: Path, block_names: list[str], item_ids: dict[str, int], z: zipfile.ZipFile) -> None:
    block_category_by_name: dict[str, int] = {}
    for category, _literal, tag_name in BLOCK_CATEGORY_TAGS:
        for block_name in read_tag_values(z, "block", tag_name):
            block_category_by_name.setdefault(block_name, category)

    required_level_by_name: dict[str, int] = {}
    for level, _literal, tag_name in REQUIRED_LEVEL_TAGS:
        for block_name in read_tag_values(z, "block", tag_name):
            required_level_by_name[block_name] = max(required_level_by_name.get(block_name, HARVEST_NONE), level)

    pickaxe_drop_exemptions: set[str] = set()
    for tag_name in PICKAXE_DROP_EXEMPT_TAGS:
        pickaxe_drop_exemptions.update(read_tag_values(z, "block", tag_name))

    tool_entries: dict[int, tuple[int, int, str, int, int, str]] = {}
    for category, tag_name in ITEM_CATEGORY_TAGS:
        for item_name in read_tag_values(z, "item", tag_name):
            item_id = item_ids.get(item_name, -1)
            material = material_for_item(item_name)
            if item_id < 0 or material is None:
                continue
            material_literal, _material_id, harvest_level, speed_x100, durability = material
            tool_entries[item_id] = (category, harvest_level, material_literal, speed_x100, durability, item_name)

    lines = [
        '#include "generated_mining_data.h"',
        "",
        "static const mc_mining_block_tool_entry_t mc_mining_block_tool_table[MC_MINING_BLOCK_TOOL_TABLE_SIZE] = {",
    ]
    block_entries = 0
    for index, block_name in enumerate(block_names):
        category = block_category_by_name.get(block_name, CATEGORY_NONE)
        required_level = required_level_by_name.get(block_name, HARVEST_NONE)
        if category == CATEGORY_NONE and required_level == HARVEST_NONE:
            continue
        flags = FLAG_PRESENT
        if required_level != HARVEST_NONE or (
            category == CATEGORY_PICKAXE and block_name not in pickaxe_drop_exemptions
        ):
            flags |= FLAG_REQUIRES_CORRECT_TOOL
        lines.append(
            f"    [{index}] = {{{category_literal(category)}, {harvest_literal(required_level)}, "
            f"{block_flags_literal(flags)}}}, /* {block_name} */"
        )
        block_entries += 1
    lines.extend(
        [
            "};",
            "",
            "static const mc_mining_tool_item_entry_t mc_mining_tool_item_table[MC_MINECRAFT_ITEM_MAX_ID + 1] = {",
        ]
    )
    for item_id in sorted(tool_entries):
        category, harvest_level, material_literal, speed_x100, durability, item_name = tool_entries[item_id]
        lines.append(
            f"    [{item_id}] = {{{category_literal(category)}, {harvest_literal(harvest_level)}, {material_literal}, 0, "
            f"{speed_x100}, {durability}}}, /* {item_name} */"
        )
    lines.extend(
        [
            "};",
            "",
            "const mc_mining_block_tool_entry_t *mc_mining_block_tool_entry_from_state(int state_id) {",
            "    if (state_id < 0) return 0;",
            "    if ((size_t)state_id >= GLOBAL_BLOCK_STATES_COUNT) return 0;",
            "    uint16_t block_index = GLOBAL_BLOCK_STATES[state_id].block_index;",
            "    if ((size_t)block_index >= MC_MINING_BLOCK_TOOL_TABLE_SIZE) return 0;",
            "    return &mc_mining_block_tool_table[block_index];",
            "}",
            "",
            "const mc_mining_tool_item_entry_t *mc_mining_tool_item_entry_from_item(int32_t item_id) {",
            "    if (item_id < 0 || item_id > MC_MINECRAFT_ITEM_MAX_ID) return 0;",
            "    const mc_mining_tool_item_entry_t *entry = &mc_mining_tool_item_table[item_id];",
            "    return entry->category != MC_MINING_TOOL_CATEGORY_NONE ? entry : 0;",
            "}",
            "",
            "const char *mc_mining_tool_category_name(mc_mining_tool_category_t category) {",
            "    switch (category) {",
            '        case MC_MINING_TOOL_CATEGORY_PICKAXE: return "pickaxe";',
            '        case MC_MINING_TOOL_CATEGORY_AXE: return "axe";',
            '        case MC_MINING_TOOL_CATEGORY_SHOVEL: return "shovel";',
            '        case MC_MINING_TOOL_CATEGORY_HOE: return "hoe";',
            '        default: return "none";',
            "    }",
            "}",
            "",
        ]
    )
    out_c.write_text("\n".join(lines), encoding="utf-8")
    print(f"generated mining data: {block_entries}/{len(block_names)} block tool entries, {len(tool_entries)} tool items")


def main() -> int:
    if len(sys.argv) != 6:
        fail("usage: gen_mining_data.py <blocks.json> <minecraft_ids.json> <server.jar> <out.c> <out.h>")
    blocks_json = Path(sys.argv[1])
    item_ids_json = Path(sys.argv[2])
    server_jar = Path(sys.argv[3])
    out_c = Path(sys.argv[4])
    out_h = Path(sys.argv[5])

    block_names = load_blocks(blocks_json)
    item_ids = load_item_id_map(item_ids_json)
    with load_server_data_jar(server_jar) as z:
        generate_header(out_h, len(block_names))
        generate_source(out_c, block_names, item_ids, z)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
