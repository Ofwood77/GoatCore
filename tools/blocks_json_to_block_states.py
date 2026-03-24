#!/usr/bin/env python3
import itertools
import json
import sys


def fail(msg: str) -> None:
    print(f"[error] {msg}", file=sys.stderr)
    sys.exit(2)


def norm_name(name: str) -> str:
    if ":" in name:
        return name
    return f"minecraft:{name}"


def prop_values(prop: dict) -> list[str]:
    vals = prop.get("values")
    if isinstance(vals, list) and vals:
        return [str(v) for v in vals]
    t = prop.get("type")
    if t == "bool":
        return ["false", "true"]
    if t == "int":
        n = prop.get("num_values")
        if isinstance(n, int) and n > 0:
            return [str(i) for i in range(n)]
    fail(f"unsupported property format: {prop!r}")
    return []


def canonical_key(base: str, props: dict[str, str]) -> str:
    if not props:
        return base
    items = sorted(props.items(), key=lambda kv: kv[0])
    inner = ",".join(f"{k}={v}" for k, v in items)
    return f"{base}[{inner}]"


def main() -> int:
    if len(sys.argv) != 3:
        fail("usage: blocks_json_to_block_states.py <in blocks.json> <out block_states.json>")
    in_path, out_path = sys.argv[1:]

    with open(in_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, list):
        fail("expected top-level JSON array (minecraft-data blocks.json format)")

    out: dict[str, int] = {}
    seen_ids: set[int] = set()
    max_id = -1

    for b in data:
        if not isinstance(b, dict):
            continue
        name = b.get("name")
        if not isinstance(name, str) or not name:
            continue
        base = norm_name(name)

        try:
            min_id = int(b["minStateId"])
            max_state_id = int(b["maxStateId"])
        except Exception as e:
            fail(f"block {base}: missing minStateId/maxStateId ({e})")

        props = b.get("states", [])
        if props is None:
            props = []
        if not isinstance(props, list):
            fail(f"block {base}: states must be a list")

        prop_defs: list[tuple[str, list[str]]] = []
        for p in props:
            if not isinstance(p, dict):
                fail(f"block {base}: invalid property entry: {p!r}")
            pname = p.get("name")
            if not isinstance(pname, str) or not pname:
                fail(f"block {base}: property missing name: {p!r}")
            prop_defs.append((pname, prop_values(p)))

        # Enumerate combinations in the order of b["states"], with later properties varying faster.
        keys = [pname for pname, _ in prop_defs]
        value_lists = [vals for _, vals in prop_defs]
        combos = itertools.product(*value_lists) if value_lists else [()]

        expected = max_state_id - min_id + 1
        count = 0
        for combo_index, combo in enumerate(combos):
            props_map = {keys[i]: combo[i] for i in range(len(keys))}
            k = canonical_key(base, props_map)
            sid = min_id + combo_index
            if sid > max_id:
                max_id = sid
            if sid in seen_ids:
                fail(f"duplicate state id {sid} at {k}")
            if k in out:
                fail(f"duplicate key {k}")
            out[k] = sid
            seen_ids.add(sid)
            count += 1

        if count != expected:
            fail(f"block {base}: generated {count} states, expected {expected} (min={min_id} max={max_state_id})")

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f, sort_keys=True, separators=(",", ":"))
        f.write("\n")

    print(f"[ok] states={len(out)} max_id={max_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

