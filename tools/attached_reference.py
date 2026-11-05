"""Validated per-family mining facts from the official 26.1.1 Java auditor."""
from pathlib import Path


def load_attached_reference():
    path = Path(__file__).resolve().parents[1] / 'tests/fixtures/vanilla-26.1.1-attached.tsv'
    families = {}
    defaults = {}
    seen = set()
    current = None
    for line in path.read_text().splitlines():
        r = line.split()
        if r[0] == 'F':
            current = r[1]
            if current in families:
                raise ValueError('duplicate attached family')
            families[current] = set()
            defaults[current] = int(r[4])
        elif r[0] == 'B':
            state, hardness, required = map(int, r[1:])
            if current is None or state in seen or hardness < 0 or required not in (0, 1):
                raise ValueError('invalid attached mining reference')
            seen.add(state)
            families[current].add((hardness, required))
    if len(seen) != 63 or len(families) != 17 or not set(defaults.values()) <= seen:
        raise ValueError('incomplete attached mining reference')
    if any(len(values) != 1 for values in families.values()):
        raise ValueError('state-dependent hardness requires an extended generator')
    return {name: next(iter(values)) for name, values in families.items()}
