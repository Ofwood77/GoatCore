#!/usr/bin/env python3
import json
import sys
from pathlib import Path


FLAG_PRESENT = 1 << 0
FLAG_SUPPORTED = 1 << 1
FLAG_FIXED_ITEM = 1 << 2
FLAG_SILK_TOUCH_FALLBACK = 1 << 3
FLAG_NO_DROP = 1 << 4
FLAG_IGNORES_SURVIVES_EXPLOSION = 1 << 5
FLAG_FORTUNE_FALLBACK = 1 << 6
FLAG_UNSUPPORTED_COMPLEX = 1 << 7


def fail(message: str) -> None:
    print(f"[error] {message}", file=sys.stderr)
    raise SystemExit(2)


def strip_namespace(name: str) -> str:
    if name.startswith("minecraft:"):
        return name.split(":", 1)[1]
    return name


def load_item_id_map(path: Path) -> dict[str, int]:
    data = json.loads(path.read_text(encoding="utf-8"))
    items = data.get("items")
    if not isinstance(items, list):
        fail(f"{path} does not contain an items array")

    item_id_by_name: dict[str, int] = {}
    for item in items:
        if not isinstance(item, dict):
            continue
        name = item.get("name")
        item_id = item.get("id")
        if isinstance(name, str) and isinstance(item_id, int):
            item_id_by_name[name] = item_id
    return item_id_by_name


def load_block_names(path: Path) -> list[str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        fail(f"{path} must contain a JSON object")
    return sorted(name for name in data.keys() if isinstance(name, str))


def is_number_equal(value: object, expected: float) -> bool:
    return isinstance(value, (int, float)) and float(value) == expected


def classify_conditions(raw: object) -> str:
    if raw is None:
        return "none"
    if not isinstance(raw, list):
        return "unsupported"
    if not raw:
        return "none"
    if len(raw) != 1:
        return "unsupported"
    cond = raw[0]
    if not isinstance(cond, dict):
        return "unsupported"
    cond_name = cond.get("condition")
    if cond_name == "minecraft:survives_explosion":
        return "survives_explosion_only"
    if cond_name == "minecraft:table_bonus":
        return "table_bonus_only"
    if cond_name != "minecraft:match_tool":
        return "unsupported"

    predicate = cond.get("predicate")
    if not isinstance(predicate, dict):
        return "unsupported"
    predicates = predicate.get("predicates")
    if not isinstance(predicates, dict):
        return "unsupported"
    enchantments = predicates.get("minecraft:enchantments")
    if not isinstance(enchantments, list) or len(enchantments) != 1:
        return "unsupported"
    ench = enchantments[0]
    if not isinstance(ench, dict):
        return "unsupported"
    if ench.get("enchantments") != "minecraft:silk_touch":
        return "unsupported"
    levels = ench.get("levels")
    if not isinstance(levels, dict):
        return "unsupported"
    min_level = levels.get("min")
    if not isinstance(min_level, (int, float)) or float(min_level) < 1.0:
        return "unsupported"
    return "silk_touch_only"


def make_result(kind: str, *, item_name: str | None = None, flags: int = 0, reason: str = "") -> dict[str, object]:
    return {
        "kind": kind,
        "item_name": item_name,
        "flags": flags,
        "reason": reason,
    }


def merge_flags(*parts: int) -> int:
    value = 0
    for part in parts:
        value |= part
    return value


def reduce_entry(entry: object) -> dict[str, object]:
    if not isinstance(entry, dict):
        return make_result("unsupported", reason="entry_not_object")

    entry_type = entry.get("type")
    cond_kind = classify_conditions(entry.get("conditions"))
    functions = entry.get("functions")
    if functions not in (None, []):
        return make_result("unsupported", reason="entry_functions")

    if entry_type == "minecraft:item":
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            return make_result("unsupported", reason="item_name")
        if cond_kind == "none":
            return make_result("drop", item_name=name, flags=FLAG_FIXED_ITEM)
        if cond_kind == "survives_explosion_only":
            return make_result("drop", item_name=name, flags=merge_flags(FLAG_FIXED_ITEM, FLAG_IGNORES_SURVIVES_EXPLOSION))
        if cond_kind == "silk_touch_only":
            return make_result("conditional_skip", flags=0)
        if cond_kind == "table_bonus_only":
            return make_result("conditional_skip", flags=FLAG_FORTUNE_FALLBACK)
        return make_result("unsupported", reason="item_conditions")

    if entry_type == "minecraft:alternatives":
        if cond_kind not in ("none", "survives_explosion_only"):
            return make_result("unsupported", reason="alternatives_conditions")
        children = entry.get("children")
        if not isinstance(children, list) or not children:
            return make_result("unsupported", reason="alternatives_children")

        saw_skipped_child = False
        combined_flags = 0
        if cond_kind == "survives_explosion_only":
            combined_flags |= FLAG_IGNORES_SURVIVES_EXPLOSION

        for child in children:
            reduced = reduce_entry(child)
            kind = reduced["kind"]
            if kind == "conditional_skip":
                saw_skipped_child = True
                combined_flags |= int(reduced["flags"])
                continue
            if kind == "drop":
                flags = int(reduced["flags"]) | combined_flags
                if saw_skipped_child:
                    flags |= FLAG_SILK_TOUCH_FALLBACK
                return make_result("drop", item_name=str(reduced["item_name"]), flags=flags)
            if kind == "no_drop":
                if saw_skipped_child:
                    flags = int(reduced["flags"]) | combined_flags | FLAG_SILK_TOUCH_FALLBACK
                    return make_result("no_drop", flags=flags)
                return reduced
            return reduced

        if saw_skipped_child:
            return make_result("no_drop", flags=merge_flags(FLAG_NO_DROP, combined_flags))
        return make_result("unsupported", reason="alternatives_no_default")

    return make_result("unsupported", reason=f"entry_type:{entry_type}")


def reduce_loot_table(table: object) -> dict[str, object]:
    if not isinstance(table, dict):
        return make_result("unsupported", reason="table_not_object")
    if table.get("type") != "minecraft:block":
        return make_result("unsupported", reason="table_type")

    pools = table.get("pools")
    if not isinstance(pools, list) or len(pools) != 1:
        return make_result("unsupported", reason="pool_count")
    pool = pools[0]
    if not isinstance(pool, dict):
        return make_result("unsupported", reason="pool_not_object")
    if not is_number_equal(pool.get("rolls"), 1.0):
        return make_result("unsupported", reason="pool_rolls")
    bonus_rolls = pool.get("bonus_rolls", 0.0)
    if not is_number_equal(bonus_rolls, 0.0):
        return make_result("unsupported", reason="pool_bonus_rolls")

    pool_cond_kind = classify_conditions(pool.get("conditions"))
    if pool_cond_kind not in ("none", "survives_explosion_only"):
        return make_result("unsupported", reason="pool_conditions")

    entries = pool.get("entries")
    if not isinstance(entries, list) or len(entries) != 1:
        return make_result("unsupported", reason="pool_entries")

    reduced = reduce_entry(entries[0])
    if reduced["kind"] in ("drop", "no_drop"):
        flags = int(reduced["flags"]) | FLAG_PRESENT | FLAG_SUPPORTED
        if pool_cond_kind == "survives_explosion_only":
            flags |= FLAG_IGNORES_SURVIVES_EXPLOSION
        reduced["flags"] = flags
        return reduced

    if reduced["kind"] == "conditional_skip":
        flags = int(reduced["flags"]) | FLAG_PRESENT | FLAG_SUPPORTED | FLAG_NO_DROP
        if pool_cond_kind == "survives_explosion_only":
            flags |= FLAG_IGNORES_SURVIVES_EXPLOSION
        return make_result("no_drop", flags=flags)

    reduced["flags"] = FLAG_PRESENT | FLAG_UNSUPPORTED_COMPLEX
    return reduced


def format_flags(flags: int) -> str:
    names = []
    if flags & FLAG_PRESENT:
        names.append("MC_BLOCK_LOOT_FLAG_PRESENT")
    if flags & FLAG_SUPPORTED:
        names.append("MC_BLOCK_LOOT_FLAG_SUPPORTED")
    if flags & FLAG_FIXED_ITEM:
        names.append("MC_BLOCK_LOOT_FLAG_FIXED_ITEM")
    if flags & FLAG_SILK_TOUCH_FALLBACK:
        names.append("MC_BLOCK_LOOT_FLAG_SILK_TOUCH_FALLBACK")
    if flags & FLAG_NO_DROP:
        names.append("MC_BLOCK_LOOT_FLAG_NO_DROP")
    if flags & FLAG_IGNORES_SURVIVES_EXPLOSION:
        names.append("MC_BLOCK_LOOT_FLAG_IGNORES_SURVIVES_EXPLOSION")
    if flags & FLAG_FORTUNE_FALLBACK:
        names.append("MC_BLOCK_LOOT_FLAG_FORTUNE_FALLBACK")
    if flags & FLAG_UNSUPPORTED_COMPLEX:
        names.append("MC_BLOCK_LOOT_FLAG_UNSUPPORTED_COMPLEX")
    return " | ".join(names) if names else "0"


def write_header(path: Path, table_size: int) -> None:
    text = """#ifndef GENERATED_BLOCK_LOOT_H
#define GENERATED_BLOCK_LOOT_H

#include <stdint.h>
#include "block_registry.h"

#define MC_BLOCK_LOOT_TABLE_SIZE @@TABLE_SIZE@@

enum {
    MC_BLOCK_LOOT_FLAG_PRESENT = 1u << 0,
    MC_BLOCK_LOOT_FLAG_SUPPORTED = 1u << 1,
    MC_BLOCK_LOOT_FLAG_FIXED_ITEM = 1u << 2,
    MC_BLOCK_LOOT_FLAG_SILK_TOUCH_FALLBACK = 1u << 3,
    MC_BLOCK_LOOT_FLAG_NO_DROP = 1u << 4,
    MC_BLOCK_LOOT_FLAG_IGNORES_SURVIVES_EXPLOSION = 1u << 5,
    MC_BLOCK_LOOT_FLAG_FORTUNE_FALLBACK = 1u << 6,
    MC_BLOCK_LOOT_FLAG_UNSUPPORTED_COMPLEX = 1u << 7
};

typedef struct {
    int32_t item_id;
    uint16_t count;
    uint16_t flags;
} mc_block_loot_entry_t;

const mc_block_loot_entry_t *mc_block_loot_entry_from_state(int state_id);
int mc_block_loot_default_item_id_from_state(int state_id, int fallback);

#endif /* GENERATED_BLOCK_LOOT_H */
"""
    text = text.replace("@@TABLE_SIZE@@", str(table_size))
    path.write_text(text, encoding="utf-8")


def write_source(path: Path, block_names: list[str], entries: list[dict[str, object]]) -> None:
    lines: list[str] = []
    lines.append('#include "generated_block_loot.h"')
    lines.append("")
    lines.append("static const mc_block_loot_entry_t mc_block_loot_table[MC_BLOCK_LOOT_TABLE_SIZE] = {")
    for index, (block_name, entry) in enumerate(zip(block_names, entries)):
        item_id = int(entry["item_id"])
        count = int(entry["count"])
        flags_expr = format_flags(int(entry["flags"]))
        lines.append(f'    [{index}] = {{{item_id}, {count}u, {flags_expr}}}, /* {block_name} */')
    lines.append("};")
    lines.append("")
    lines.append("const mc_block_loot_entry_t *mc_block_loot_entry_from_state(int state_id) {")
    lines.append("    if (state_id < 0) return NULL;")
    lines.append("    if ((size_t)state_id >= GLOBAL_BLOCK_STATES_COUNT) return NULL;")
    lines.append("    uint16_t block_index = GLOBAL_BLOCK_STATES[state_id].block_index;")
    lines.append("    if ((size_t)block_index >= MC_BLOCK_LOOT_TABLE_SIZE) return NULL;")
    lines.append("    return &mc_block_loot_table[block_index];")
    lines.append("}")
    lines.append("")
    lines.append("int mc_block_loot_default_item_id_from_state(int state_id, int fallback) {")
    lines.append("    const mc_block_loot_entry_t *entry = mc_block_loot_entry_from_state(state_id);")
    lines.append("    if (!entry) return fallback;")
    lines.append("    if ((entry->flags & MC_BLOCK_LOOT_FLAG_SUPPORTED) == 0u) return fallback;")
    lines.append("    if (entry->item_id < 0 || entry->count == 0u) return fallback;")
    lines.append("    return entry->item_id;")
    lines.append("}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    verbose = False
    args = [arg for arg in sys.argv[1:] if arg != "--verbose"]
    if len(args) != len(sys.argv[1:]):
        verbose = True
    if len(args) != 5:
        print("usage: gen_block_loot.py minecraft_ids.json blocks.json loot_dir out.c out.h [--verbose]", file=sys.stderr)
        return 1

    ids_path = Path(args[0])
    blocks_path = Path(args[1])
    loot_dir = Path(args[2])
    out_c = Path(args[3])
    out_h = Path(args[4])

    item_id_by_name = load_item_id_map(ids_path)
    block_names = load_block_names(blocks_path)
    entries: list[dict[str, object]] = []
    ignored: list[tuple[str, str]] = []
    stats = {
        "missing": 0,
        "supported_drop": 0,
        "supported_no_drop": 0,
        "unsupported": 0,
    }

    for block_name in block_names:
        loot_path = loot_dir / f"{strip_namespace(block_name)}.json"
        if not loot_path.exists():
            stats["missing"] += 1
            entries.append({"item_id": -1, "count": 0, "flags": 0})
            ignored.append((block_name, "missing_loot_file"))
            continue

        reduced = reduce_loot_table(json.loads(loot_path.read_text(encoding="utf-8")))
        kind = str(reduced["kind"])
        if kind == "unsupported":
            stats["unsupported"] += 1
            flags = int(reduced["flags"])
            entries.append({"item_id": -1, "count": 0, "flags": flags})
            ignored.append((block_name, str(reduced["reason"])))
            continue

        if kind == "no_drop":
            stats["supported_no_drop"] += 1
            entries.append({"item_id": -1, "count": 0, "flags": int(reduced["flags"])})
            continue

        item_name = str(reduced["item_name"])
        item_id = item_id_by_name.get(item_name)
        if item_id is None:
            stats["unsupported"] += 1
            entries.append({"item_id": -1, "count": 0, "flags": FLAG_PRESENT | FLAG_UNSUPPORTED_COMPLEX})
            ignored.append((block_name, f"unknown_item:{item_name}"))
            continue

        stats["supported_drop"] += 1
        entries.append({"item_id": item_id, "count": 1, "flags": int(reduced["flags"])})

    write_header(out_h, len(block_names))
    write_source(out_c, block_names, entries)

    print(
        "generated block loot: "
        f"{stats['supported_drop']} drop, "
        f"{stats['supported_no_drop']} no-drop, "
        f"{stats['unsupported']} unsupported, "
        f"{stats['missing']} missing"
    )
    if verbose:
        for block_name, reason in ignored:
            print(f"ignored {block_name}: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
