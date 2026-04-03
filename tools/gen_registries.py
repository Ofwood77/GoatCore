#!/usr/bin/env python3
import sys


def fail(msg: str) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(1)


def write_header(path: str) -> None:
    text = """#ifndef GENERATED_REGISTRIES_H
#define GENERATED_REGISTRIES_H

#include "block_registry.h"

#define MC_BLOCK_STATE_MAX_ID ((int)(GLOBAL_BLOCK_STATES_COUNT - 1u))

int mc_block_state_id(const char *name, int fallback);
const char *mc_block_state_key(int id);

#endif /* GENERATED_REGISTRIES_H */
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def write_c(path: str, header_name: str) -> None:
    text = f"""#include "{header_name}"

/* BlockState lookup compatibility shim.
 * The authoritative BlockState registry is generated in block_registry.c
 * from data/26.1-rc-3/reports/blocks.json. */
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main() -> int:
    if len(sys.argv) != 4:
        fail("usage: gen_registries.py <ignored> <out.c> <out.h>")
    _ignored, out_c, out_h = sys.argv[1:]
    write_header(out_h)
    write_c(out_c, out_h.split("/")[-1])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
