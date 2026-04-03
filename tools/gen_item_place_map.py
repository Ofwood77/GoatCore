#!/usr/bin/env python3
import json
import sys


def short_name(full_name: str) -> str:
    if full_name.startswith("minecraft:"):
        return full_name.split(":", 1)[1]
    return full_name


def resolve_default_state(block: dict) -> int:
    states = block.get("states")
    min_state = block.get("minStateId")
    default_state = block.get("defaultState")

    if isinstance(states, list) and states:
        if min_state is not None:
            return int(min_state)
        if default_state is not None:
            return int(default_state)
        return -1

    if default_state is not None:
        return int(default_state)
    if min_state is not None:
        return int(min_state)
    return -1


def main():
    if len(sys.argv) != 5:
        print("usage: gen_item_place_map.py minecraft_ids.json blocks.json out.c out.h", file=sys.stderr)
        return 1

    ids_path, blocks_path, out_c, out_h = sys.argv[1:]
    meta = json.load(open(ids_path, "r", encoding="utf-8"))
    items = meta.get("items", [])
    blocks = json.load(open(blocks_path, "r", encoding="utf-8"))

    by_name_block = {b["name"]: b for b in blocks}
    max_item_id = max((int(item["id"]) for item in items), default=-1)
    names = ["NULL"] * (max_item_id + 1)
    states = [-1] * (max_item_id + 1)

    mapped = 0
    for item in items:
        item_id = int(item["id"])
        full_name = item["name"]
        names[item_id] = json.dumps(full_name)
        block = by_name_block.get(short_name(full_name))
        if block is not None:
            states[item_id] = resolve_default_state(block)
            mapped += 1

    with open(out_h, "w", encoding="utf-8") as fh:
        fh.write("#ifndef GENERATED_ITEM_PLACE_H\n")
        fh.write("#define GENERATED_ITEM_PLACE_H\n\n")
        fh.write(f"#define MC_ITEM_MAX_ID {max_item_id}\n\n")
        fh.write("int mc_item_default_place_state(int item_id);\n")
        fh.write("const char *mc_item_name(int item_id);\n\n")
        fh.write("#endif\n")

    with open(out_c, "w", encoding="utf-8") as fc:
        fc.write('#include "generated_item_place.h"\n\n')
        fc.write("#include <stddef.h>\n\n")
        fc.write("static const int mc_item_default_state_table[] = {\n")
        for idx, state in enumerate(states):
            fc.write(f"    [{idx}] = {state},\n")
        fc.write("};\n\n")
        fc.write("static const char *mc_item_name_table[] = {\n")
        for idx, name in enumerate(names):
            fc.write(f"    [{idx}] = {name},\n")
        fc.write("};\n\n")
        fc.write("int mc_item_default_place_state(int item_id) {\n")
        fc.write("    if (item_id < 0 || item_id > MC_ITEM_MAX_ID) return -1;\n")
        fc.write("    return mc_item_default_state_table[item_id];\n")
        fc.write("}\n\n")
        fc.write("const char *mc_item_name(int item_id) {\n")
        fc.write("    if (item_id < 0 || item_id > MC_ITEM_MAX_ID) return NULL;\n")
        fc.write("    return mc_item_name_table[item_id];\n")
        fc.write("}\n")

    print(f"generated item place map: {mapped} block items, max item id {max_item_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
