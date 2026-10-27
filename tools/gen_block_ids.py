#!/usr/bin/env python3
"""Map internal sorted block indexes to exact Vanilla registry protocol IDs."""
import json
from pathlib import Path
import sys


def generate(blocks_path, registries_path, source_path, header_path):
    blocks = json.loads(Path(blocks_path).read_text())
    entries = json.loads(Path(registries_path).read_text())["minecraft:block"]["entries"]
    if set(blocks) != set(entries):
        raise ValueError("block registry and state report have different block names")
    ids = [entries[name]["protocol_id"] for name in sorted(blocks)]
    if any(type(value) is not int for value in ids) or sorted(ids) != list(range(len(blocks))):
        raise ValueError("block protocol IDs must be unique and dense from zero")
    Path(header_path).write_text('''#ifndef GENERATED_BLOCK_IDS_H
#define GENERATED_BLOCK_IDS_H
#include <stdint.h>
/* Converts a global state ID to the minecraft:block registry protocol ID. */
int32_t mc_block_protocol_id_for_state(int32_t state);
#endif
''')
    lines = ['#include "generated_block_ids.h"', '#include "block_registry.h"', '',
             '/* Internal descriptor order is alphabetical, not Vanilla registry order. */',
             f'static const int32_t protocol_ids[{len(ids)}] = {{']
    lines += [f'    {value}, /* {name} */' for name, value in zip(sorted(blocks), ids)]
    lines += ['};', '', 'int32_t mc_block_protocol_id_for_state(int32_t state) {',
              '    if (state < 0 || (size_t)state >= GLOBAL_BLOCK_STATES_COUNT ||',
              '        !(GLOBAL_BLOCK_STATES[state].flags & MC_BLOCK_FLAG_VALID)) return -1;',
              '    return protocol_ids[GLOBAL_BLOCK_STATES[state].block_index];', '}', '']
    Path(source_path).write_text('\n'.join(lines))


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("usage: gen_block_ids.py blocks.json registries.json out.c out.h")
    generate(*sys.argv[1:])
