#!/usr/bin/env python3
import json
import sys
from pathlib import Path


SCALE = 100
FLAG_PRESENT = 1 << 0
FLAG_UNBREAKABLE = 1 << 1
FLAG_INSTANT = 1 << 2


EXACT: dict[str, tuple[float, int]] = {
    "minecraft:air": (0.0, FLAG_INSTANT),
    "minecraft:cave_air": (0.0, FLAG_INSTANT),
    "minecraft:void_air": (0.0, FLAG_INSTANT),
    "minecraft:water": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:lava": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:bubble_column": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:bedrock": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:barrier": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:command_block": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:chain_command_block": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:repeating_command_block": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:end_portal": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:end_gateway": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:end_portal_frame": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:nether_portal": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:structure_block": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:structure_void": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:jigsaw": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:light": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:moving_piston": (-1.0, FLAG_UNBREAKABLE),
    "minecraft:obsidian": (50.0, 0),
    "minecraft:crying_obsidian": (50.0, 0),
    "minecraft:ancient_debris": (30.0, 0),
    "minecraft:ender_chest": (22.5, 0),
    "minecraft:enchanting_table": (5.0, 0),
    "minecraft:anvil": (5.0, 0),
    "minecraft:chipped_anvil": (5.0, 0),
    "minecraft:damaged_anvil": (5.0, 0),
    "minecraft:beacon": (3.0, 0),
    "minecraft:reinforced_deepslate": (55.0, 0),
    "minecraft:spawner": (5.0, 0),
    "minecraft:trial_spawner": (50.0, 0),
    "minecraft:vault": (50.0, 0),
    "minecraft:furnace": (3.5, 0),
    "minecraft:blast_furnace": (3.5, 0),
    "minecraft:smoker": (3.5, 0),
    "minecraft:chest": (2.5, 0),
    "minecraft:trapped_chest": (2.5, 0),
    "minecraft:barrel": (2.5, 0),
    "minecraft:crafting_table": (2.5, 0),
    "minecraft:cartography_table": (2.5, 0),
    "minecraft:fletching_table": (2.5, 0),
    "minecraft:smithing_table": (2.5, 0),
    "minecraft:loom": (2.5, 0),
    "minecraft:lectern": (2.5, 0),
    "minecraft:composter": (0.6, 0),
    "minecraft:stone": (1.5, 0),
    "minecraft:granite": (1.5, 0),
    "minecraft:polished_granite": (1.5, 0),
    "minecraft:diorite": (1.5, 0),
    "minecraft:polished_diorite": (1.5, 0),
    "minecraft:andesite": (1.5, 0),
    "minecraft:polished_andesite": (1.5, 0),
    "minecraft:cobblestone": (2.0, 0),
    "minecraft:mossy_cobblestone": (2.0, 0),
    "minecraft:end_stone": (3.0, 0),
    "minecraft:netherrack": (0.4, 0),
    "minecraft:basalt": (1.25, 0),
    "minecraft:polished_basalt": (1.25, 0),
    "minecraft:blackstone": (1.5, 0),
    "minecraft:polished_blackstone": (1.5, 0),
    "minecraft:tuff": (1.5, 0),
    "minecraft:calcite": (0.75, 0),
    "minecraft:dripstone_block": (1.5, 0),
    "minecraft:dirt": (0.5, 0),
    "minecraft:coarse_dirt": (0.5, 0),
    "minecraft:podzol": (0.5, 0),
    "minecraft:rooted_dirt": (0.5, 0),
    "minecraft:mud": (0.5, 0),
    "minecraft:clay": (0.6, 0),
    "minecraft:sand": (0.5, 0),
    "minecraft:red_sand": (0.5, 0),
    "minecraft:gravel": (0.6, 0),
    "minecraft:grass_block": (0.6, 0),
    "minecraft:mycelium": (0.6, 0),
    "minecraft:farmland": (0.6, 0),
    "minecraft:moss_block": (0.1, 0),
    "minecraft:snow": (0.1, 0),
    "minecraft:snow_block": (0.2, 0),
    "minecraft:ice": (0.5, 0),
    "minecraft:packed_ice": (0.5, 0),
    "minecraft:blue_ice": (2.8, 0),
    "minecraft:glass": (0.3, 0),
    "minecraft:tinted_glass": (0.3, 0),
    "minecraft:cobweb": (4.0, 0),
    "minecraft:redstone_block": (5.0, 0),
    "minecraft:redstone_lamp": (0.3, 0),
}


def fail(message: str) -> None:
    print(f"[error] {message}", file=sys.stderr)
    raise SystemExit(2)


def load_blocks(path: Path) -> list[str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        fail(f"{path} must contain a JSON object")
    return sorted(name for name in data if isinstance(name, str))


def matches_suffix(name: str, suffixes: tuple[str, ...]) -> bool:
    return any(name.endswith(suffix) for suffix in suffixes)


def infer_hardness(name: str) -> tuple[float, int] | None:
    if name in EXACT:
        return EXACT[name]

    short = name.split(":", 1)[1] if ":" in name else name

    if short in ("wheat", "carrots", "potatoes", "beetroots", "melon_stem", "pumpkin_stem", "attached_melon_stem", "attached_pumpkin_stem"):
        return (0.0, FLAG_INSTANT)
    if matches_suffix(short, ("_ore",)):
        return (4.5, 0) if short.startswith("deepslate_") else (3.0, 0)
    if matches_suffix(short, ("_log", "_wood", "_stem", "_hyphae", "_planks", "_bamboo_block")):
        return (2.0, 0)
    if matches_suffix(short, ("_leaves",)):
        return (0.2, 0)
    if matches_suffix(short, ("_wool", "_carpet")):
        return (0.8, 0)
    if matches_suffix(short, ("_glass", "_glass_pane")):
        return (0.3, 0)
    if matches_suffix(short, ("_concrete",)):
        return (1.8, 0)
    if matches_suffix(short, ("_concrete_powder",)):
        return (0.5, 0)
    if matches_suffix(short, ("_terracotta", "_glazed_terracotta")):
        return (1.25, 0)
    if matches_suffix(short, ("_copper", "_copper_block", "_copper_bulb", "_copper_grate", "_copper_bars", "_copper_chain")):
        return (3.0, 0)
    if matches_suffix(short, ("_block",)) and any(part in short for part in ("iron", "gold", "diamond", "emerald", "lapis", "coal", "netherite", "raw_")):
        return (5.0, 0)
    if any(part in short for part in ("deepslate", "blackstone", "stone_brick", "tuff_brick")):
        return (1.5, 0)
    if any(part in short for part in ("sandstone", "quartz", "bricks", "prismarine")):
        return (0.8, 0) if "quartz" in short else (1.5, 0)
    if matches_suffix(short, ("_stairs", "_slab", "_wall")):
        return (1.5, 0)
    if matches_suffix(short, ("_button",)):
        return (0.5, 0)
    if matches_suffix(short, ("_pressure_plate",)):
        return (0.5, 0)
    if matches_suffix(short, ("_door", "_trapdoor", "_fence", "_fence_gate", "_sign", "_hanging_sign")):
        return (3.0 if "iron" in short else 2.0, 0)
    if matches_suffix(short, ("_sapling", "_flower", "_tulip", "_bush", "_mushroom", "_roots", "_sprouts")):
        return (0.0, FLAG_INSTANT)
    if short in ("short_grass", "tall_grass", "fern", "large_fern", "dead_bush", "torch", "redstone_torch", "wall_torch", "fire"):
        return (0.0, FLAG_INSTANT)
    if "shulker_box" in short:
        return (2.0, 0)
    if "coral" in short:
        return (1.5 if short.endswith("_block") else 0.0, FLAG_INSTANT if not short.endswith("_block") else 0)

    return None


def format_flags(flags: int) -> str:
    names = ["MC_BLOCK_HARDNESS_FLAG_PRESENT"]
    if flags & FLAG_UNBREAKABLE:
        names.append("MC_BLOCK_HARDNESS_FLAG_UNBREAKABLE")
    if flags & FLAG_INSTANT:
        names.append("MC_BLOCK_HARDNESS_FLAG_INSTANT")
    return " | ".join(names)


def generate_header(out_h: Path, table_size: int) -> None:
    out_h.write_text(
        f"""#ifndef GENERATED_BLOCK_HARDNESS_H
#define GENERATED_BLOCK_HARDNESS_H

#include <stdbool.h>
#include <stdint.h>
#include \"block_registry.h\"

#define MC_BLOCK_HARDNESS_TABLE_SIZE {table_size}
#define MC_BLOCK_HARDNESS_SCALE {SCALE}

enum {{
    MC_BLOCK_HARDNESS_FLAG_PRESENT = 1u << 0,
    MC_BLOCK_HARDNESS_FLAG_UNBREAKABLE = 1u << 1,
    MC_BLOCK_HARDNESS_FLAG_INSTANT = 1u << 2
}};

typedef struct {{
    int16_t hardness_x100;
    uint16_t flags;
}} mc_block_hardness_entry_t;

const mc_block_hardness_entry_t *mc_block_hardness_entry_from_state(int state_id);
float mc_block_hardness_from_state(int state_id, float fallback);
bool mc_block_hardness_is_unbreakable(int state_id);

#endif /* GENERATED_BLOCK_HARDNESS_H */
""",
        encoding="utf-8",
    )


def generate_source(out_c: Path, block_names: list[str]) -> None:
    lines = [
        '#include "generated_block_hardness.h"',
        "",
        f"static const mc_block_hardness_entry_t mc_block_hardness_table[MC_BLOCK_HARDNESS_TABLE_SIZE] = {{",
    ]
    covered = 0
    for index, name in enumerate(block_names):
        inferred = infer_hardness(name)
        if inferred is None:
            continue
        hardness, flags = inferred
        hardness_x100 = int(round(hardness * SCALE))
        lines.append(f"    [{index}] = {{{hardness_x100}, {format_flags(flags)}}}, /* {name} */")
        covered += 1
    lines.extend(
        [
            "};",
            "",
            "const mc_block_hardness_entry_t *mc_block_hardness_entry_from_state(int state_id) {",
            "    if (state_id < 0) return 0;",
            "    if ((size_t)state_id >= GLOBAL_BLOCK_STATES_COUNT) return 0;",
            "    uint16_t block_index = GLOBAL_BLOCK_STATES[state_id].block_index;",
            "    if ((size_t)block_index >= MC_BLOCK_HARDNESS_TABLE_SIZE) return 0;",
            "    return &mc_block_hardness_table[block_index];",
            "}",
            "",
            "float mc_block_hardness_from_state(int state_id, float fallback) {",
            "    const mc_block_hardness_entry_t *entry = mc_block_hardness_entry_from_state(state_id);",
            "    if (!entry || (entry->flags & MC_BLOCK_HARDNESS_FLAG_PRESENT) == 0u) return fallback;",
            "    if ((entry->flags & MC_BLOCK_HARDNESS_FLAG_UNBREAKABLE) != 0u) return -1.0f;",
            "    return (float)entry->hardness_x100 / (float)MC_BLOCK_HARDNESS_SCALE;",
            "}",
            "",
            "bool mc_block_hardness_is_unbreakable(int state_id) {",
            "    const mc_block_hardness_entry_t *entry = mc_block_hardness_entry_from_state(state_id);",
            "    return entry && (entry->flags & MC_BLOCK_HARDNESS_FLAG_UNBREAKABLE) != 0u;",
            "}",
            "",
        ]
    )
    out_c.write_text("\n".join(lines), encoding="utf-8")
    print(f"generated block hardness: {covered}/{len(block_names)} block families covered")


def main() -> int:
    if len(sys.argv) != 4:
        fail("usage: gen_block_hardness.py <blocks.json> <out.c> <out.h>")
    blocks_json = Path(sys.argv[1])
    out_c = Path(sys.argv[2])
    out_h = Path(sys.argv[3])
    block_names = load_blocks(blocks_json)
    generate_header(out_h, len(block_names))
    generate_source(out_c, block_names)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
