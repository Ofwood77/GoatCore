#!/usr/bin/env python3
import json
import os
import subprocess
import sys


def fail(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def gperf_input(entries):
    lines = []
    lines.append("%{")
    lines.append("#include <string.h>")
    lines.append("#include <stddef.h>")
    lines.append("struct mc_block_state { const char *name; int id; };")
    lines.append("%}")
    lines.append("struct mc_block_state;")
    lines.append("%%")
    for name, val in entries:
        lines.append(f"{name},{val}")
    lines.append("%%")
    return "\n".join(lines) + "\n"


def run_gperf(text):
    try:
        proc = subprocess.run(
            ["gperf", "-L", "C", "-t", "-D", "-N", "mc_block_state_lookup", "-H", "mc_block_state_hash"],
            input=text.encode("utf-8"),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
    except FileNotFoundError:
        return None
    except subprocess.CalledProcessError as e:
        fail(f"gperf failed: {e.stderr.decode('utf-8', errors='ignore')}")
    return proc.stdout.decode("utf-8")


def write_header(path, max_id):
    header = """#ifndef GENERATED_REGISTRIES_H
#define GENERATED_REGISTRIES_H

#define MC_BLOCK_STATE_MAX_ID %d

int mc_block_state_id(const char *name, int fallback);
const char *mc_block_state_key(int id);

#endif /* GENERATED_REGISTRIES_H */
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(header % max_id)


def write_c(path, gperf_out, entries, max_id, by_id):
    src = []
    src.append('#include "generated_registries.h"')
    src.append("#include <string.h>")
    src.append("")
    src.append("#if defined(__GNUC__) || defined(__clang__)")
    src.append("#pragma GCC diagnostic push")
    src.append("#pragma GCC diagnostic ignored \"-Wunused-parameter\"")
    src.append("#pragma GCC diagnostic ignored \"-Wmissing-field-initializers\"")
    src.append("#endif")
    src.append("")
    if gperf_out:
        src.append(gperf_out)
    else:
        src.append("struct mc_block_state { const char *name; int id; };")
        src.append("static const struct mc_block_state mc_block_state_table[] = {")
        for name, val in entries:
            src.append(f'    {{"{name}", {val}}},')
        src.append("};")
        src.append("static const struct mc_block_state *mc_block_state_lookup(const char *str, size_t len) {")
        src.append("    (void)len;")
        src.append("    size_t lo = 0;")
        src.append("    size_t hi = sizeof(mc_block_state_table)/sizeof(mc_block_state_table[0]);")
        src.append("    while (lo < hi) {")
        src.append("        size_t mid = lo + (hi - lo) / 2;")
        src.append("        int cmp = strcmp(str, mc_block_state_table[mid].name);")
        src.append("        if (cmp == 0) return &mc_block_state_table[mid];")
        src.append("        if (cmp < 0) hi = mid;")
        src.append("        else lo = mid + 1;")
        src.append("    }")
        src.append("    return NULL;")
        src.append("}")
    src.append("")
    src.append("#if defined(__GNUC__) || defined(__clang__)")
    src.append("#pragma GCC diagnostic pop")
    src.append("#endif")
    src.append("")
    src.append("static const char *mc_block_state_by_id[MC_BLOCK_STATE_MAX_ID + 1] = {")
    for i, name in sorted(by_id.items(), key=lambda kv: kv[0]):
        src.append(f'    [{i}] = "{name}",')
    src.append("};")
    src.append("")
    src.append("int mc_block_state_id(const char *name, int fallback) {")
    src.append("    if (!name) return fallback;")
    src.append("    const struct mc_block_state *res = mc_block_state_lookup(name, strlen(name));")
    src.append("    return res ? res->id : fallback;")
    src.append("}")
    src.append("")
    src.append("const char *mc_block_state_key(int id) {")
    src.append("    if (id < 0 || id > MC_BLOCK_STATE_MAX_ID) return NULL;")
    src.append("    return mc_block_state_by_id[id];")
    src.append("}")
    src.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(src))


def main():
    if len(sys.argv) != 4:
        fail("usage: gen_registries.py <block_states.json> <out.c> <out.h>")
    json_path, out_c, out_h = sys.argv[1:]
    data = load_json(json_path)
    if not isinstance(data, dict):
        fail("block_states.json must be a JSON object of {name: id}")
    entries = sorted(data.items(), key=lambda kv: kv[0])
    by_id = {}
    max_id = -1
    for name, val in entries:
        try:
            i = int(val)
        except Exception:
            fail(f"invalid id for {name}: {val!r}")
        if i in by_id:
            fail(f"duplicate id {i} for {name} and {by_id[i]}")
        by_id[i] = name
        if i > max_id:
            max_id = i
    # gperf input uses comma to separate key/value; our canonical keys contain commas.
    # Keep gperf for tiny maps without commas, otherwise fallback to binary search table.
    use_gperf = all("," not in k and "\n" not in k for k, _ in entries)
    gperf_out = run_gperf(gperf_input(entries)) if use_gperf else None
    write_header(out_h, max_id)
    write_c(out_c, gperf_out, entries, max_id, by_id)


if __name__ == "__main__":
    main()
