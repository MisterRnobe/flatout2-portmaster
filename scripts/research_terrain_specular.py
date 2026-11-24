#!/usr/bin/env python3
"""Read-only local evidence for the PC terrain specular SSR mask.

Extracts private reference sources under work/, never modifies the game archive.
The alpha product is an authored specular mask; it does not identify mud by name.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def extract(archive, index):
    with archive.open('rb') as stream:
        magic, _, data_offset, count, _, table = struct.unpack('<6I', stream.read(24))
        if magic != 0x31736662 or index >= count or table + count * 24 > data_offset:
            raise ValueError('Unsupported BFS table')
        stream.seek(table + index * 24)
        flags, offset, raw, packed, _, _ = struct.unpack('<6I', stream.read(24))
        if not (0 < raw < 100000 and 0 < packed < 100000 and offset >= data_offset):
            raise ValueError('Unexpected reference asset size/offset')
        stream.seek(offset)
        payload = stream.read(packed)
    if flags & 1:
        payload = zlib.decompress(payload)
    if len(payload) != raw:
        raise ValueError('Truncated BFS asset')
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, default=Path('gamedata/data.bfs'))
    parser.add_argument('--output', type=Path, default=Path('work/metal/mud-research'))
    args = parser.parse_args()
    source = extract(args.archive, 2411)
    library = extract(args.archive, 5354)
    if b'// pro_lightmapped_spec.sha' not in source or b'"pro_lightmapped_spec.sha"' not in library:
        raise ValueError('Archive asset indices differ from the verified PC release')
    words = [0xffff0101, 0x42, 0xb00f0000, 0x42, 0xb00f0001, 5, 0x800f0000,
             0xb0e40000, 0xb0e40001, 4, 0x80170000, 0x90e40000, 0x80ff0000, 0x80e40000, 0xffff]
    code = struct.pack('<15I', *words)
    captured = Path('work/metal/shaders/40e19e2958b0afb1.bin')
    if captured.read_bytes() != code:
        raise ValueError('Captured program differs from the verified assembly tokens')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'pro_lightmapped_spec.sha').write_bytes(source)
    evidence = dict(asset='data.bfs / pro_lightmapped_spec.sha', asset_record_index=2411,
                    asset_sha256=hashlib.sha256(source).hexdigest(), shader_library_entry=3,
                    shader_id=2, pixel_bytecode_bytes=len(code), pixel_bytecode_fnv64='40e19e2958b0afb1',
                    pixel_bytecode_sha256=hashlib.sha256(code).hexdigest(),
                    authored_mask='sampled Tex0 alpha * sampled Tex1 alpha',
                    authored_specular_exponent=8,
                    scope='Reflective lightmapped terrain; alpha is a specular mask, not a mud/wetness classification.')
    (args.output / 'evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
