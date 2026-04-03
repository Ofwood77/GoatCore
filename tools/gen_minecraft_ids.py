#!/usr/bin/env python3
import json
import re
import subprocess
import sys
from pathlib import Path

EXPECTED_PROTOCOL_VERSION = 775
EXPECTED_VERSION_IDS = {"26.1", "26.1.1"}


PACKET_PROTOCOLS = {
    "handshaking": "net.minecraft.network.protocol.handshake.HandshakeProtocols",
    "status": "net.minecraft.network.protocol.status.StatusProtocols",
    "login": "net.minecraft.network.protocol.login.LoginProtocols",
    "configuration": "net.minecraft.network.protocol.configuration.ConfigurationProtocols",
    "play": "net.minecraft.network.protocol.game.GameProtocols",
}

ENTITY_CLASS = "net.minecraft.world.entity.EntityType"
BLOCK_ENTITY_CLASS = "net.minecraft.world.level.block.entity.BlockEntityType"
ITEMS_CLASS = "net.minecraft.world.item.Items"
PROTO_FALLBACK_PATH = Path(__file__).resolve().parent.parent / "assets" / "proto.yml"

PACKET_FALLBACK_NAME_MAP = {
    ("handshaking", "serverbound", "intention"): "CLIENT_INTENTION",
    ("handshaking", "serverbound", "set_protocol"): "CLIENT_INTENTION",
    ("status", "serverbound", "ping_start"): "SERVERBOUND_STATUS_REQUEST",
    ("status", "serverbound", "ping"): "SERVERBOUND_PING_REQUEST",
    ("status", "clientbound", "server_info"): "CLIENTBOUND_STATUS_RESPONSE",
    ("status", "clientbound", "ping"): "CLIENTBOUND_PONG_RESPONSE",
    ("login", "serverbound", "login_start"): "SERVERBOUND_HELLO",
    ("login", "serverbound", "login_acknowledged"): "SERVERBOUND_LOGIN_ACKNOWLEDGED",
    ("login", "clientbound", "success"): "CLIENTBOUND_LOGIN_FINISHED",
    ("configuration", "serverbound", "finish_configuration"): "SERVERBOUND_FINISH_CONFIGURATION",
    ("configuration", "serverbound", "keep_alive"): "SERVERBOUND_KEEP_ALIVE",
    ("configuration", "serverbound", "select_known_packs"): "SERVERBOUND_SELECT_KNOWN_PACKS",
    ("configuration", "clientbound", "disconnect"): "CLIENTBOUND_DISCONNECT",
    ("configuration", "clientbound", "finish_configuration"): "CLIENTBOUND_FINISH_CONFIGURATION",
    ("configuration", "clientbound", "keep_alive"): "CLIENTBOUND_KEEP_ALIVE",
    ("configuration", "clientbound", "registry_data"): "CLIENTBOUND_REGISTRY_DATA",
    ("configuration", "clientbound", "select_known_packs"): "CLIENTBOUND_SELECT_KNOWN_PACKS",
    ("play", "serverbound", "teleport_confirm"): "SERVERBOUND_ACCEPT_TELEPORTATION",
    ("play", "serverbound", "chat_command"): "SERVERBOUND_CHAT_COMMAND",
    ("play", "serverbound", "chat_command_signed"): "SERVERBOUND_CHAT_COMMAND_SIGNED",
    ("play", "serverbound", "window_click"): "SERVERBOUND_CONTAINER_CLICK",
    ("play", "serverbound", "close_window"): "SERVERBOUND_CONTAINER_CLOSE",
    ("play", "serverbound", "keep_alive"): "SERVERBOUND_KEEP_ALIVE",
    ("play", "serverbound", "position"): "SERVERBOUND_MOVE_PLAYER_POS",
    ("play", "serverbound", "position_look"): "SERVERBOUND_MOVE_PLAYER_POS_ROT",
    ("play", "serverbound", "look"): "SERVERBOUND_MOVE_PLAYER_ROT",
    ("play", "serverbound", "flying"): "SERVERBOUND_MOVE_PLAYER_STATUS_ONLY",
    ("play", "serverbound", "block_dig"): "SERVERBOUND_PLAYER_ACTION",
    ("play", "serverbound", "held_item_slot"): "SERVERBOUND_SET_CARRIED_ITEM",
    ("play", "serverbound", "set_creative_slot"): "SERVERBOUND_SET_CREATIVE_MODE_SLOT",
    ("play", "serverbound", "block_place"): "SERVERBOUND_USE_ITEM_ON",
    ("play", "clientbound", "spawn_entity"): "CLIENTBOUND_ADD_ENTITY",
    ("play", "clientbound", "tile_entity_data"): "CLIENTBOUND_BLOCK_ENTITY_DATA",
    ("play", "clientbound", "block_change"): "CLIENTBOUND_BLOCK_UPDATE",
    ("play", "clientbound", "declare_commands"): "CLIENTBOUND_COMMANDS",
    ("play", "clientbound", "close_window"): "CLIENTBOUND_CONTAINER_CLOSE",
    ("play", "clientbound", "window_items"): "CLIENTBOUND_CONTAINER_SET_CONTENT",
    ("play", "clientbound", "set_slot"): "CLIENTBOUND_CONTAINER_SET_SLOT",
    ("play", "clientbound", "kick_disconnect"): "CLIENTBOUND_DISCONNECT",
    ("play", "clientbound", "entity_status"): "CLIENTBOUND_ENTITY_EVENT",
    ("play", "clientbound", "unload_chunk"): "CLIENTBOUND_FORGET_LEVEL_CHUNK",
    ("play", "clientbound", "game_state_change"): "CLIENTBOUND_GAME_EVENT",
    ("play", "clientbound", "keep_alive"): "CLIENTBOUND_KEEP_ALIVE",
    ("play", "clientbound", "map_chunk"): "CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT",
    ("play", "clientbound", "login"): "CLIENTBOUND_LOGIN",
    ("play", "clientbound", "rel_entity_move"): "CLIENTBOUND_MOVE_ENTITY_POS",
    ("play", "clientbound", "entity_move_look"): "CLIENTBOUND_MOVE_ENTITY_POS_ROT",
    ("play", "clientbound", "entity_look"): "CLIENTBOUND_MOVE_ENTITY_ROT",
    ("play", "clientbound", "open_window"): "CLIENTBOUND_OPEN_SCREEN",
    ("play", "clientbound", "abilities"): "CLIENTBOUND_PLAYER_ABILITIES",
    ("play", "clientbound", "player_remove"): "CLIENTBOUND_PLAYER_INFO_REMOVE",
    ("play", "clientbound", "player_info"): "CLIENTBOUND_PLAYER_INFO_UPDATE",
    ("play", "clientbound", "position"): "CLIENTBOUND_PLAYER_POSITION",
    ("play", "clientbound", "entity_destroy"): "CLIENTBOUND_REMOVE_ENTITIES",
    ("play", "clientbound", "entity_head_rotation"): "CLIENTBOUND_ROTATE_HEAD",
    ("play", "clientbound", "update_view_position"): "CLIENTBOUND_SET_CHUNK_CACHE_CENTER",
    ("play", "clientbound", "spawn_position"): "CLIENTBOUND_SET_DEFAULT_SPAWN_POSITION",
    ("play", "clientbound", "entity_metadata"): "CLIENTBOUND_SET_ENTITY_DATA",
    ("play", "clientbound", "held_item_slot"): "CLIENTBOUND_SET_HELD_SLOT",
    ("play", "clientbound", "entity_teleport"): "CLIENTBOUND_TELEPORT_ENTITY",
}

FALLBACK_ENTITY_TYPES = [
    {"id": 57, "name": "minecraft:item"},
    {"id": 149, "name": "minecraft:player"},
]

FALLBACK_BLOCK_ENTITY_TYPES = [
    {"id": 1, "name": "minecraft:chest"},
    {"id": 2, "name": "minecraft:trapped_chest"},
    {"id": 3, "name": "minecraft:ender_chest"},
]

REPORT_STATE_NAME_MAP = {
    "handshake": "handshaking",
    "status": "status",
    "login": "login",
    "configuration": "configuration",
    "play": "play",
}


def run_javap(client_root: str, flags: list[str], class_name: str) -> str:
    cmd = ["javap", "-classpath", client_root, *flags, class_name]
    proc = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return proc.stdout


def sanitize_macro(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum():
            out.append(ch.upper())
        else:
            out.append("_")
    macro = "".join(out)
    while "__" in macro:
        macro = macro.replace("__", "_")
    return macro.strip("_")


def to_mc_name(simple_name: str) -> str:
    return f"minecraft:{simple_name.lower()}"


def parse_protocol_packets(text: str) -> list[tuple[str, list[str]]]:
    methods: list[tuple[str, list[str]]] = []
    current_name = None
    current_packets: list[str] = []
    in_code = False

    def finish_method():
        nonlocal current_name, current_packets, in_code
        if current_name and current_packets:
            clientbound = sum(1 for name in current_packets if name.startswith("CLIENTBOUND_"))
            serverbound = sum(1 for name in current_packets if name.startswith("SERVERBOUND_"))
            if clientbound or serverbound:
                direction = "clientbound" if clientbound >= serverbound else "serverbound"
                methods.append((direction, list(current_packets)))
            elif current_packets == ["CLIENT_INTENTION"]:
                methods.append(("serverbound", list(current_packets)))
        current_name = None
        current_packets = []
        in_code = False

    for line in text.splitlines():
        method_match = re.match(r"^\s*private static void (lambda\$static\$\d+)\(.*\);$", line)
        if method_match:
            finish_method()
            current_name = method_match.group(1)
            continue
        if current_name and line.strip() == "Code:":
            in_code = True
            continue
        if current_name and in_code:
            if re.match(r"^\s*(public|private|static|protected).*\)\s*;?$", line) or line.strip() == "static {};" or line.strip() == "static {};":
                finish_method()
                if line.strip() == "static {};" or line.strip() == "static {};":
                    current_name = None
                continue
            packet_match = re.search(r"PacketTypes\.([A-Z0-9_]+):Lnet/minecraft/network/protocol/PacketType;", line)
            if packet_match:
                current_packets.append(packet_match.group(1))
    finish_method()
    return methods


def parse_registered_names(text: str) -> list[str]:
    names: list[str] = []
    in_static = False
    pending_name = None
    for line in text.splitlines():
        if line.startswith("  static {};"):
            in_static = True
            pending_name = None
            continue
        if not in_static:
            continue
        if line.startswith("}"):
            break
        name_match = re.search(r"// String ([a-z0-9_]+)", line)
        if name_match:
            pending_name = name_match.group(1)
            continue
        if pending_name and "Method register:(Ljava/lang/String;" in line:
            names.append(to_mc_name(pending_name))
            pending_name = None
    return names


def parse_item_fields(text: str) -> list[str]:
    items: list[str] = []
    for line in text.splitlines():
        field_match = re.match(r"^\s*public static final net\.minecraft\.world\.item\.Item ([A-Z0-9_]+);$", line)
        if field_match:
            items.append(to_mc_name(field_match.group(1)))
    return items


def parse_proto_enum_names(proto_text: str, section_header: str) -> list[str]:
    marker = f"^{section_header}:"
    idx = proto_text.find(marker)
    if idx < 0:
        return []
    tail = proto_text[idx:].splitlines()
    names: list[str] = []
    in_enum = False
    for line in tail:
        if line.startswith("^") and line != marker:
            break
        if not in_enum:
            if line.strip() == "name: varint =>":
                in_enum = True
            continue
        if line.startswith("      params:") or line.startswith("      0x") or line.startswith("      params:"):
            break
        match_named = re.match(r"^\s*-\s*([a-z0-9_]+)\s*$", line)
        if match_named:
            names.append(match_named.group(1))
            continue
        match_hex = re.match(r"^\s*0x[0-9a-fA-F]+:\s*([a-z0-9_]+)\s*$", line)
        if match_hex:
            names.append(match_hex.group(1))
    return names


def build_fallback_packets_from_proto(proto_path: Path) -> list[dict]:
    proto_text = proto_path.read_text(encoding="utf-8")
    sections = [
        ("handshaking", "serverbound", "handshaking.toServer.types"),
        ("status", "serverbound", "status.toServer.types"),
        ("status", "clientbound", "status.toClient.types"),
        ("login", "serverbound", "login.toServer.types"),
        ("login", "clientbound", "login.toClient.types"),
        ("configuration", "serverbound", "configuration.toServer.types"),
        ("configuration", "clientbound", "configuration.toClient.types"),
        ("play", "serverbound", "play.toServer.types"),
        ("play", "clientbound", "play.toClient.types"),
    ]
    packets: list[dict] = []
    for state, direction, header in sections:
        names = parse_proto_enum_names(proto_text, header)
        for packet_id, proto_name in enumerate(names):
            logical_name = PACKET_FALLBACK_NAME_MAP.get((state, direction, proto_name))
            if logical_name is None:
                continue
            packets.append({"state": state, "direction": direction, "id": packet_id, "name": logical_name})
    return packets


def emit_header(out_h: Path, packets: list[dict], items: list[str], entities: list[str], block_entities: list[str]) -> None:
    with out_h.open("w", encoding="utf-8") as fh:
        fh.write("#ifndef GENERATED_MINECRAFT_IDS_H\n")
        fh.write("#define GENERATED_MINECRAFT_IDS_H\n\n")
        fh.write("#include <stdint.h>\n")
        fh.write('#include "mc_server.h"\n\n')
        fh.write("typedef enum {\n")
        fh.write("    MC_MINECRAFT_PACKET_DIR_SERVERBOUND = 0,\n")
        fh.write("    MC_MINECRAFT_PACKET_DIR_CLIENTBOUND = 1,\n")
        fh.write("} mc_minecraft_packet_direction_t;\n\n")

        for entry in packets:
            macro = f"MC_PKT_{sanitize_macro(entry['state'])}_{sanitize_macro(entry['name'])}"
            fh.write(f"#define {macro} {entry['id']}\n")
        fh.write("\n")

        fh.write(f"#define MC_MINECRAFT_ITEM_MAX_ID {max((entry['id'] for entry in items), default=-1)}\n")
        fh.write(f"#define MC_MINECRAFT_ENTITY_TYPE_MAX_ID {max((entry['id'] for entry in entities), default=-1)}\n")
        fh.write(f"#define MC_MINECRAFT_BLOCK_ENTITY_TYPE_MAX_ID {max((entry['id'] for entry in block_entities), default=-1)}\n\n")

        fh.write("int32_t mc_minecraft_packet_id(mc_proto_state_t state, mc_minecraft_packet_direction_t direction, const char *name);\n")
        fh.write("const char *mc_minecraft_packet_name(mc_proto_state_t state, mc_minecraft_packet_direction_t direction, int32_t id);\n")
        fh.write("int32_t mc_minecraft_item_id(const char *name);\n")
        fh.write("const char *mc_minecraft_item_name(int32_t id);\n")
        fh.write("int32_t mc_minecraft_entity_type_id(const char *name);\n")
        fh.write("const char *mc_minecraft_entity_type_name(int32_t id);\n")
        fh.write("int32_t mc_minecraft_block_entity_type_id(const char *name);\n")
        fh.write("const char *mc_minecraft_block_entity_type_name(int32_t id);\n\n")
        fh.write("#endif\n")


def emit_c(out_c: Path, header_name: str, packets: list[dict], items: list[dict], entities: list[dict], block_entities: list[dict]) -> None:
    with out_c.open("w", encoding="utf-8") as fc:
        fc.write(f'#include "{header_name}"\n\n')
        fc.write("#include <stddef.h>\n")
        fc.write("#include <string.h>\n\n")
        fc.write("typedef struct {\n")
        fc.write("    mc_proto_state_t state;\n")
        fc.write("    mc_minecraft_packet_direction_t direction;\n")
        fc.write("    int32_t id;\n")
        fc.write("    const char *name;\n")
        fc.write("} mc_minecraft_packet_entry_t;\n\n")
        fc.write("typedef struct {\n")
        fc.write("    const char *name;\n")
        fc.write("    int32_t id;\n")
        fc.write("} mc_minecraft_named_id_t;\n\n")

        fc.write("static const mc_minecraft_packet_entry_t mc_minecraft_packets[] = {\n")
        for entry in packets:
            state_macro = f"MC_STATE_{sanitize_macro(entry['state'])}"
            dir_macro = f"MC_MINECRAFT_PACKET_DIR_{sanitize_macro(entry['direction'])}"
            fc.write(f'    {{{state_macro}, {dir_macro}, {entry["id"]}, "{entry["name"]}"}},\n')
        fc.write("};\n\n")

        def write_named_table(table_name: str, values: list[dict], max_id: int) -> None:
            fc.write(f"static const char *{table_name}_by_id[] = {{\n")
            by_id = {entry["id"]: entry["name"] for entry in values}
            for idx in range(max_id + 1):
                name = by_id.get(idx)
                fc.write(f'    [{idx}] = {"NULL" if name is None else json.dumps(name)},\n')
            fc.write("};\n\n")
            fc.write(f"static const mc_minecraft_named_id_t {table_name}_by_name[] = {{\n")
            for entry in values:
                fc.write(f'    {{"{entry["name"]}", {entry["id"]}}},\n')
            fc.write("};\n\n")

        write_named_table("mc_minecraft_items", items, max((entry["id"] for entry in items), default=-1))
        write_named_table("mc_minecraft_entity_types", entities, max((entry["id"] for entry in entities), default=-1))
        write_named_table("mc_minecraft_block_entity_types", block_entities, max((entry["id"] for entry in block_entities), default=-1))

        fc.write("static int32_t find_named_id(const mc_minecraft_named_id_t *table, size_t count, const char *name) {\n")
        fc.write("    if (!name) return -1;\n")
        fc.write("    for (size_t i = 0; i < count; i++) {\n")
        fc.write("        if (strcmp(table[i].name, name) == 0) return table[i].id;\n")
        fc.write("    }\n")
        fc.write("    return -1;\n")
        fc.write("}\n\n")

        fc.write("int32_t mc_minecraft_packet_id(mc_proto_state_t state, mc_minecraft_packet_direction_t direction, const char *name) {\n")
        fc.write("    if (!name) return -1;\n")
        fc.write("    for (size_t i = 0; i < sizeof(mc_minecraft_packets) / sizeof(mc_minecraft_packets[0]); i++) {\n")
        fc.write("        const mc_minecraft_packet_entry_t *entry = &mc_minecraft_packets[i];\n")
        fc.write("        if (entry->state == state && entry->direction == direction && strcmp(entry->name, name) == 0) return entry->id;\n")
        fc.write("    }\n")
        fc.write("    return -1;\n")
        fc.write("}\n\n")

        fc.write("const char *mc_minecraft_packet_name(mc_proto_state_t state, mc_minecraft_packet_direction_t direction, int32_t id) {\n")
        fc.write("    for (size_t i = 0; i < sizeof(mc_minecraft_packets) / sizeof(mc_minecraft_packets[0]); i++) {\n")
        fc.write("        const mc_minecraft_packet_entry_t *entry = &mc_minecraft_packets[i];\n")
        fc.write("        if (entry->state == state && entry->direction == direction && entry->id == id) return entry->name;\n")
        fc.write("    }\n")
        fc.write("    return NULL;\n")
        fc.write("}\n\n")

        fc.write("int32_t mc_minecraft_item_id(const char *name) {\n")
        fc.write("    return find_named_id(mc_minecraft_items_by_name, sizeof(mc_minecraft_items_by_name) / sizeof(mc_minecraft_items_by_name[0]), name);\n")
        fc.write("}\n\n")
        fc.write("const char *mc_minecraft_item_name(int32_t id) {\n")
        fc.write("    if (id < 0 || id > MC_MINECRAFT_ITEM_MAX_ID) return NULL;\n")
        fc.write("    return mc_minecraft_items_by_id[id];\n")
        fc.write("}\n\n")

        fc.write("int32_t mc_minecraft_entity_type_id(const char *name) {\n")
        fc.write("    return find_named_id(mc_minecraft_entity_types_by_name, sizeof(mc_minecraft_entity_types_by_name) / sizeof(mc_minecraft_entity_types_by_name[0]), name);\n")
        fc.write("}\n\n")
        fc.write("const char *mc_minecraft_entity_type_name(int32_t id) {\n")
        fc.write("    if (id < 0 || id > MC_MINECRAFT_ENTITY_TYPE_MAX_ID) return NULL;\n")
        fc.write("    return mc_minecraft_entity_types_by_id[id];\n")
        fc.write("}\n\n")

        fc.write("int32_t mc_minecraft_block_entity_type_id(const char *name) {\n")
        fc.write("    return find_named_id(mc_minecraft_block_entity_types_by_name,\n")
        fc.write("                         sizeof(mc_minecraft_block_entity_types_by_name) / sizeof(mc_minecraft_block_entity_types_by_name[0]),\n")
        fc.write("                         name);\n")
        fc.write("}\n\n")
        fc.write("const char *mc_minecraft_block_entity_type_name(int32_t id) {\n")
        fc.write("    if (id < 0 || id > MC_MINECRAFT_BLOCK_ENTITY_TYPE_MAX_ID) return NULL;\n")
        fc.write("    return mc_minecraft_block_entity_types_by_id[id];\n")
        fc.write("}\n")


def emit_json(out_json: Path, packets: list[dict], items: list[dict], entities: list[dict], block_entities: list[dict]) -> None:
    payload = {
        "packets": packets,
        "items": items,
        "entity_types": entities,
        "block_entity_types": block_entities,
    }
    out_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_version_info(client_root: Path) -> dict:
    candidates = [
        client_root / "version.json",
        client_root.parent / "version.json",
        client_root.parent / "server" / "version.json",
        client_root.parent / "client" / "version.json",
    ]
    for path in candidates:
        if path.exists():
            return json.load(path.open("r", encoding="utf-8"))
    return {}


def is_reports_root(root: Path) -> bool:
    return root.is_dir() and (root / "packets.json").exists() and (root / "registries.json").exists()


def packet_logical_name(state: str, direction: str, packet_name: str) -> str:
    suffix = packet_name.split(":", 1)[1] if ":" in packet_name else packet_name
    mapped = PACKET_FALLBACK_NAME_MAP.get((state, direction, suffix))
    if mapped is not None:
        return mapped
    prefix = "CLIENTBOUND" if direction == "clientbound" else "SERVERBOUND"
    return f"{prefix}_{sanitize_macro(suffix)}"


def load_reports_packets(reports_root: Path) -> list[dict]:
    raw = json.loads((reports_root / "packets.json").read_text(encoding="utf-8"))
    packets: list[dict] = []
    for report_state, normalized_state in REPORT_STATE_NAME_MAP.items():
        state_section = raw.get(report_state)
        if not isinstance(state_section, dict):
            continue
        for direction in ("serverbound", "clientbound"):
            dir_section = state_section.get(direction)
            if not isinstance(dir_section, dict):
                continue
            for packet_name, meta in dir_section.items():
                if not isinstance(packet_name, str) or not isinstance(meta, dict):
                    continue
                packet_id = meta.get("protocol_id")
                if not isinstance(packet_id, int):
                    continue
                packets.append(
                    {
                        "state": normalized_state,
                        "direction": direction,
                        "id": packet_id,
                        "name": packet_logical_name(normalized_state, direction, packet_name),
                    }
                )
    return packets


def load_registry_entries(reports_root: Path, registry_name: str) -> list[dict]:
    raw = json.loads((reports_root / "registries.json").read_text(encoding="utf-8"))
    reg = raw.get(registry_name)
    if not isinstance(reg, dict):
        return []
    entries = reg.get("entries")
    if not isinstance(entries, dict):
        return []
    values: list[dict] = []
    for name, meta in entries.items():
        if not isinstance(name, str) or not isinstance(meta, dict):
            continue
        protocol_id = meta.get("protocol_id")
        if not isinstance(protocol_id, int):
            continue
        values.append({"id": protocol_id, "name": name})
    values.sort(key=lambda entry: entry["id"])
    return values


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: gen_minecraft_ids.py <client_root> <out.c> <out.h> <out.json>", file=sys.stderr)
        return 1

    client_root = sys.argv[1]
    out_c = Path(sys.argv[2])
    out_h = Path(sys.argv[3])
    out_json = Path(sys.argv[4])

    source_root = Path(client_root)
    if not source_root.exists():
        print(f"client root not found: {client_root}", file=sys.stderr)
        return 1

    reports_mode = is_reports_root(source_root)
    version_info = {} if reports_mode else load_version_info(source_root)
    version_id = version_info.get("id")
    protocol_version = version_info.get("protocol_version")
    if version_id is None:
        version_id = "26.1.1"
    if protocol_version is None:
        protocol_version = EXPECTED_PROTOCOL_VERSION
    if protocol_version != EXPECTED_PROTOCOL_VERSION or version_id not in EXPECTED_VERSION_IDS:
        print(
            f"client source version {version_id!r} protocol={protocol_version!r} does not match "
            f"expected 26.1/{EXPECTED_PROTOCOL_VERSION}",
            file=sys.stderr,
        )
        return 1

    if reports_mode:
        packets = load_reports_packets(source_root)
        items = load_registry_entries(source_root, "minecraft:item")
        entities = load_registry_entries(source_root, "minecraft:entity_type")
        block_entities = load_registry_entries(source_root, "minecraft:block_entity_type")
    else:
        packets = []
        for state, class_name in PACKET_PROTOCOLS.items():
            text = run_javap(client_root, ["-p", "-c"], class_name)
            for direction, names in parse_protocol_packets(text):
                for packet_id, name in enumerate(names):
                    packets.append({"state": state, "direction": direction, "id": packet_id, "name": name})

        entity_text = run_javap(client_root, ["-p", "-c"], ENTITY_CLASS)
        block_entity_text = run_javap(client_root, ["-p", "-c"], BLOCK_ENTITY_CLASS)
        items_text = run_javap(client_root, ["-p", "-constants"], ITEMS_CLASS)

        entities = [{"id": idx, "name": name} for idx, name in enumerate(parse_registered_names(entity_text))]
        block_entities = [{"id": idx, "name": name} for idx, name in enumerate(parse_registered_names(block_entity_text))]
        items = [{"id": idx, "name": name} for idx, name in enumerate(parse_item_fields(items_text))]

    if not packets or not items or not entities or not block_entities:
        print("failed to extract minecraft ids from client classes", file=sys.stderr)
        return 1

    emit_header(out_h, packets, items, entities, block_entities)
    emit_c(out_c, out_h.name, packets, items, entities, block_entities)
    emit_json(out_json, packets, items, entities, block_entities)

    print(
        f"generated minecraft ids: packets={len(packets)} items={len(items)} "
        f"entity_types={len(entities)} block_entity_types={len(block_entities)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
