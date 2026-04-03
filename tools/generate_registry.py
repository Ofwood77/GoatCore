#!/usr/bin/env python3
import json
import sys


AIR_KEYS = {
    "minecraft:air",
    "minecraft:cave_air",
    "minecraft:void_air",
}


def fail(message: str) -> None:
    print(f"[error] {message}", file=sys.stderr)
    raise SystemExit(2)


def canonical_state_key(block_name: str, state: dict) -> str:
    props = state.get("properties")
    if props is None:
        return block_name
    if not isinstance(props, dict):
        fail(f"state properties for {block_name} must be an object")
    if not props:
        return block_name
    items = sorted((str(k), str(v)) for k, v in props.items())
    inner = ",".join(f"{k}={v}" for k, v in items)
    return f"{block_name}[{inner}]"


def load_report(path: str) -> tuple[list[dict], int]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)

    if not isinstance(data, dict):
        fail("blocks report must be a JSON object")

    blocks: list[dict] = []
    seen_state_ids: dict[int, str] = {}
    seen_keys: dict[str, int] = {}
    max_state_id = -1

    for block_index, block_name in enumerate(sorted(data.keys())):
        block = data[block_name]
        if not isinstance(block_name, str) or not block_name:
            fail(f"invalid block name: {block_name!r}")
        if not isinstance(block, dict):
            fail(f"block entry for {block_name} must be an object")

        states = block.get("states")
        if not isinstance(states, list) or not states:
            fail(f"block {block_name} must contain a non-empty states array")

        default_state_id = None
        min_state_id = None
        max_block_state_id = None
        state_rows: list[dict] = []

        for state in states:
            if not isinstance(state, dict):
                fail(f"block {block_name} has an invalid state entry: {state!r}")
            raw_id = state.get("id")
            if not isinstance(raw_id, int) or raw_id < 0:
                fail(f"block {block_name} has an invalid state id: {raw_id!r}")

            key = canonical_state_key(block_name, state)
            if raw_id in seen_state_ids:
                fail(f"duplicate state id {raw_id} for {key} and {seen_state_ids[raw_id]}")
            if key in seen_keys:
                fail(f"duplicate canonical key {key} for ids {seen_keys[key]} and {raw_id}")

            seen_state_ids[raw_id] = key
            seen_keys[key] = raw_id
            if raw_id > max_state_id:
                max_state_id = raw_id

            flags = ["MC_BLOCK_FLAG_VALID"]
            if state.get("default") is True:
                if default_state_id is not None:
                    fail(f"block {block_name} defines multiple default states")
                default_state_id = raw_id
                flags.append("MC_BLOCK_FLAG_IS_DEFAULT_STATE")
            if block_name in AIR_KEYS:
                flags.append("MC_BLOCK_FLAG_IS_AIR")

            if min_state_id is None or raw_id < min_state_id:
                min_state_id = raw_id
            if max_block_state_id is None or raw_id > max_block_state_id:
                max_block_state_id = raw_id

            state_rows.append(
                {
                    "id": raw_id,
                    "key": key,
                    "flags_expr": " | ".join(flags),
                    "block_index": block_index,
                }
            )

        if default_state_id is None:
            fail(f"block {block_name} does not define a default state")

        blocks.append(
            {
                "name": block_name,
                "default_state": default_state_id,
                "min_state_id": min_state_id,
                "max_state_id": max_block_state_id,
                "block_index": block_index,
                "states": state_rows,
            }
        )

    return blocks, max_state_id


def generate_header(path: str) -> None:
    text = """#ifndef MC_BLOCK_REGISTRY_H
#define MC_BLOCK_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t mc_global_state_id_t;
typedef uint32_t mc_block_id_t;

enum {
    MC_BLOCK_FLAG_VALID = 1u << 0,
    MC_BLOCK_FLAG_IS_AIR = 1u << 1,
    MC_BLOCK_FLAG_IS_DEFAULT_STATE = 1u << 2,
    MC_BLOCK_FLAG_IS_SOLID = 1u << 3,
    MC_BLOCK_FLAG_IS_OPAQUE = 1u << 4,
    MC_BLOCK_FLAG_HAS_BLOCK_ENTITY = 1u << 5
};

typedef struct {
    const char *name;
    mc_global_state_id_t default_state;
    mc_global_state_id_t min_state_id;
    mc_global_state_id_t max_state_id;
} mc_block_desc_t;

typedef struct {
    uint32_t flags;
    uint8_t luminance;
    uint8_t reserved0;
    uint16_t block_index;
} mc_block_properties_t;

extern const mc_block_desc_t GLOBAL_BLOCKS[];
extern const size_t GLOBAL_BLOCK_COUNT;
extern const mc_block_properties_t GLOBAL_BLOCK_STATES[];
extern const size_t GLOBAL_BLOCK_STATES_COUNT;

mc_global_state_id_t mc_global_state_id_from_key(const char *key, mc_global_state_id_t fallback);
const char *mc_global_state_key(mc_global_state_id_t id);

#endif /* MC_BLOCK_REGISTRY_H */
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def generate_source(path: str, blocks: list[dict], max_state_id: int) -> None:
    states_by_id = [None] * (max_state_id + 1)
    key_rows = []
    for block in blocks:
        for state in block["states"]:
            states_by_id[state["id"]] = state
            key_rows.append((state["key"], state["id"]))
    key_rows.sort(key=lambda item: item[0])

    lines = []
    lines.append('#include "block_registry.h"')
    lines.append("")
    lines.append("#include <string.h>")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char *key;")
    lines.append("    mc_global_state_id_t id;")
    lines.append("} mc_state_key_entry_t;")
    lines.append("")
    lines.append(f"const size_t GLOBAL_BLOCK_COUNT = {len(blocks)}u;")
    lines.append(f"const size_t GLOBAL_BLOCK_STATES_COUNT = {max_state_id + 1}u;")
    lines.append("")
    lines.append(f"const mc_block_desc_t GLOBAL_BLOCKS[{len(blocks)}] = {{")
    for block in blocks:
        lines.append(
            f'    [{block["block_index"]}] = {{"{block["name"]}", {block["default_state"]}u, {block["min_state_id"]}u, {block["max_state_id"]}u}},'
        )
    lines.append("};")
    lines.append("")
    lines.append(f"const mc_block_properties_t GLOBAL_BLOCK_STATES[{max_state_id + 1}] = {{")
    for state_id, state in enumerate(states_by_id):
        if state is None:
            lines.append(f"    [{state_id}] = {{0u, 0u, 0u, 0u}},")
        else:
            lines.append(
                f'    [{state_id}] = {{{state["flags_expr"]}, 0u, 0u, {state["block_index"]}u}},'
            )
    lines.append("};")
    lines.append("")
    lines.append(f"static const char *mc_state_keys_by_id[{max_state_id + 1}] = {{")
    for state_id, state in enumerate(states_by_id):
        if state is not None:
            lines.append(f'    [{state_id}] = "{state["key"]}",')
    lines.append("};")
    lines.append("")
    lines.append(f"static const mc_state_key_entry_t mc_state_key_table[{len(key_rows)}] = {{")
    for key, state_id in key_rows:
        lines.append(f'    {{"{key}", {state_id}u}},')
    lines.append("};")
    lines.append("")
    lines.append("mc_global_state_id_t mc_global_state_id_from_key(const char *key, mc_global_state_id_t fallback) {")
    lines.append("    size_t lo = 0;")
    lines.append("    size_t hi = sizeof(mc_state_key_table) / sizeof(mc_state_key_table[0]);")
    lines.append("")
    lines.append("    if (!key) return fallback;")
    lines.append("")
    lines.append("    while (lo < hi) {")
    lines.append("        size_t mid = lo + (hi - lo) / 2;")
    lines.append("        int cmp = strcmp(key, mc_state_key_table[mid].key);")
    lines.append("        if (cmp == 0) return mc_state_key_table[mid].id;")
    lines.append("        if (cmp < 0) hi = mid;")
    lines.append("        else lo = mid + 1;")
    lines.append("    }")
    lines.append("")
    lines.append("    return fallback;")
    lines.append("}")
    lines.append("")
    lines.append("const char *mc_global_state_key(mc_global_state_id_t id) {")
    lines.append("    if ((size_t)id >= GLOBAL_BLOCK_STATES_COUNT) return NULL;")
    lines.append("    return mc_state_keys_by_id[id];")
    lines.append("}")
    lines.append("")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main() -> int:
    if len(sys.argv) != 4:
        fail("usage: generate_registry.py <in blocks.json> <out.c> <out.h>")

    in_path, out_c, out_h = sys.argv[1:]
    blocks, max_state_id = load_report(in_path)
    generate_header(out_h)
    generate_source(out_c, blocks, max_state_id)
    print(f"[ok] blocks={len(blocks)} states={max_state_id + 1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
