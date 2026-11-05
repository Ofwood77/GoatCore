#!/usr/bin/env python3
"""Generate exact 26.1.1 state support flags/families from audit_hinged_blocks."""
import argparse
import hashlib
from pathlib import Path

def generate(source):
    flags={};families=[]
    for line in source.read_text().splitlines():
        row=line.split('\t')
        if row[0]=='S':
            state,flag=map(int,row[1:]);assert state not in flags and 0<=flag<16;flags[state]=flag
        if row[0]=='F':families.append((row[1],*map(int,row[2:])))
    assert sorted(flags)==list(range(29873)) and len(families)==42
    out=['/* Generated from official 26.1.1 JAR; never edit state IDs by hand. */',
         '/* audit SHA256: '+hashlib.sha256(source.read_bytes()).hexdigest()+' */',
         'static const mc_hinged_family_t families[] = {']
    for name,kind,manual,item,opened,closed in sorted(families):
        out.append(f'    {{"{name}", {kind}, {manual}, {item}, {opened}, {closed}}},')
    out+=['};','static const uint8_t support_flags[29873] = {']
    for i in range(0,29873,32):out.append('    '+','.join(str(flags[j]) for j in range(i,min(i+32,29873)))+',')
    return '\n'.join(out+['};',''])

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('audit',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.write_text(generate(a.audit))
