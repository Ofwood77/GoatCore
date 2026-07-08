#!/usr/bin/env python3
"""Extract a packet-codec inventory from a named Minecraft client/server jar.

This is not a minecraft-data generator yet. It builds an intermediate report
that links vanilla packet ids from generated reports to their Java packet
classes, fields, PacketType constants, and obvious FriendlyByteBuf read/write
calls visible through javap.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any


DEFAULT_JAR = Path("/home/ofwood/.local/share/ModrinthApp/meta/versions/26.1.1/26.1.1.jar")
DEFAULT_PACKETS = Path("data/26.1.1/reports/packets.json")

FIELD_RE = re.compile(
    r"^\s+(?:private|public|protected)\s+(?:static\s+)?(?:final\s+)?(?P<type>[\w.$<>?, \[\]/]+)\s+(?P<name>\w+);"
)
PACKET_TYPE_RE = re.compile(r"// Field (?P<class>[\w/$]+PacketTypes)\.(?P<constant>[A-Z0-9_]+):")
BUF_CALL_RE = re.compile(r"// Method (?P<class>[\w/$]+(?:FriendlyByteBuf|ByteBuf))\.(?P<method>(?:read|write)[A-Za-z0-9_]+):")
CODEC_FIELD_RE = re.compile(r"// Field (?P<class>[\w/$]+)\.(?P<field>[A-Z0-9_]+):Lnet/minecraft/network/codec/StreamCodec;")


def run_javap(jar: Path, class_name: str) -> str:
    cmd = ["javap", "-classpath", str(jar), "-p", "-c", class_name]
    return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)


def load_packets(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def java_class_name(zip_name: str) -> str:
    return zip_name[:-6].replace("/", ".")


def list_packet_classes(jar: Path) -> list[str]:
    with zipfile.ZipFile(jar) as zf:
        names = []
        for name in zf.namelist():
            if not name.startswith("net/minecraft/network/protocol/"):
                continue
            if not name.endswith(".class"):
                continue
            leaf = name.rsplit("/", 1)[-1]
            if "Packet" not in leaf and not leaf.startswith(("Clientbound", "Serverbound")):
                continue
            if leaf in {"Packet.class", "PacketType.class", "PacketFlow.class", "PacketUtils.class"}:
                continue
            names.append(java_class_name(name))
        return sorted(names)


def parse_javap_output(class_name: str, text: str) -> dict[str, Any]:
    fields = []
    packet_type_constants = []
    buf_calls = []
    codec_refs = []

    for line in text.splitlines():
        field_match = FIELD_RE.match(line)
        if field_match:
            name = field_match.group("name")
            typ = " ".join(field_match.group("type").split())
            if name != "STREAM_CODEC":
                fields.append({"name": name, "type": typ})

        type_match = PACKET_TYPE_RE.search(line)
        if type_match:
            constant = type_match.group("constant")
            if constant not in packet_type_constants:
                packet_type_constants.append(constant)

        call_match = BUF_CALL_RE.search(line)
        if call_match:
            item = {
                "buffer": call_match.group("class").replace("/", "."),
                "method": call_match.group("method"),
            }
            if item not in buf_calls:
                buf_calls.append(item)

        codec_match = CODEC_FIELD_RE.search(line)
        if codec_match:
            item = {
                "class": codec_match.group("class").replace("/", "."),
                "field": codec_match.group("field"),
            }
            if item not in codec_refs:
                codec_refs.append(item)

    return {
        "class": class_name,
        "fields": fields,
        "packet_type_constants": packet_type_constants,
        "bytebuf_calls": buf_calls,
        "codec_refs": codec_refs,
    }


def packet_name_to_constant_suffix(packet_name: str) -> str:
    raw = packet_name.split(":", 1)[-1]
    return raw.upper().replace("/", "_").replace(".", "_").replace("-", "_")


def expected_constants(direction: str, packet_name: str) -> list[str]:
    suffix = packet_name_to_constant_suffix(packet_name)
    constants = [f"{direction.upper()}_{suffix}"]
    if packet_name == "minecraft:intention":
        constants.append("CLIENT_INTENTION")
    return constants


def build_report(jar: Path, packets_path: Path, max_classes: int | None) -> dict[str, Any]:
    packets = load_packets(packets_path)
    classes = list_packet_classes(jar)
    if max_classes is not None:
        classes = classes[:max_classes]

    parsed_classes = []
    by_constant: dict[str, list[dict[str, Any]]] = {}
    errors = []

    for class_name in classes:
        try:
            parsed = parse_javap_output(class_name, run_javap(jar, class_name))
        except subprocess.CalledProcessError as exc:
            errors.append({"class": class_name, "error": exc.output.strip()})
            continue
        parsed_classes.append(parsed)
        for constant in parsed["packet_type_constants"]:
            by_constant.setdefault(constant, []).append(parsed)

    packet_rows = []
    for state, state_value in packets.items():
        for direction, direction_value in state_value.items():
            for packet_name, meta in sorted(direction_value.items(), key=lambda item: item[1]["protocol_id"]):
                candidates = expected_constants(direction, packet_name)
                matches = []
                for constant in candidates:
                    matches.extend(by_constant.get(constant, []))
                packet_rows.append({
                    "state": state,
                    "direction": direction,
                    "name": packet_name,
                    "protocol_id": meta["protocol_id"],
                    "expected_constants": candidates,
                    "matches": matches,
                })

    unmatched = [row for row in packet_rows if not row["matches"]]
    return {
        "jar": str(jar),
        "packets_report": str(packets_path),
        "class_count": len(parsed_classes),
        "packet_count": len(packet_rows),
        "matched_packet_count": len(packet_rows) - len(unmatched),
        "unmatched_packet_count": len(unmatched),
        "errors": errors,
        "packets": packet_rows,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Extract Minecraft packet codec hints with javap.")
    parser.add_argument("--jar", type=Path, default=DEFAULT_JAR)
    parser.add_argument("--packets", type=Path, default=DEFAULT_PACKETS)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--max-classes", type=int, default=None, help="debug limit")
    args = parser.parse_args(argv)

    report = build_report(args.jar, args.packets, args.max_classes)
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    print(
        f"[extract-packets] matched={report['matched_packet_count']} "
        f"unmatched={report['unmatched_packet_count']} classes={report['class_count']}",
        file=sys.stderr,
    )
    return 0 if not report["errors"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
