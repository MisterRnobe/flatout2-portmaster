#!/usr/bin/env python3
"""Inspect local FO2 collision water without changing game files.

Format reference (independent implementation, engine XYZ/Y-up retained):
https://github.com/xob0t/fo2blender/blob/master/docs/formats/collision.md
The collision/render association is deliberately NOT inferred from a name.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import zlib

MAX_ASSET = 32 * 1024 * 1024


def archive_assets(path):
    with path.open('rb') as stream:
        size = path.stat().st_size
        header = stream.read(24)
        if len(header) != 24:
            raise ValueError('Truncated BFS header')
        magic, _, data, count, _, table = struct.unpack('<6I', header)
        if magic != 0x31736662 or count > 100000 or table < 24 or table + count * 24 > data or data > size:
            raise ValueError('Invalid BFS index')
        stream.seek(table)
        records = stream.read(count * 24)
        for index in range(count):
            flags, offset, raw, packed, _, _ = struct.unpack_from('<6I', records, index * 24)
            if not raw or raw > MAX_ASSET or packed > MAX_ASSET:
                continue
            if not packed or offset < data or offset + packed > size:
                raise ValueError(f'Invalid BFS asset extent: {index}')
            stream.seek(offset)
            payload = stream.read(packed)
            if flags & 1:
                decoder = zlib.decompressobj()
                payload = decoder.decompress(payload, raw + 1)
                if not decoder.eof or decoder.unconsumed_tail or decoder.unused_data:
                    raise ValueError(f'Invalid/bounded BFS zlib stream: {index}')
            if len(payload) != raw:
                raise ValueError(f'BFS asset length mismatch: {index}')
            yield index, payload


def surface_definitions(payload):
    text = payload.decode('cp1252')
    text = re.sub(r'--[^\r\n]*', '', text)
    table = re.search(r'\bSurfaces\s*=\s*\{', text)
    if not table:
        raise ValueError('Surface table missing')
    rows = {}
    for match in re.finditer(r'\[(\d+)\]\s*=\s*\{(.*?)\n\s*\}', text[table.end():], re.S):
        number = int(match[1])
        name = re.search(r'\bName\s*=\s*"([^"\r\n]*)"', match[2])
        if number in rows or not name:
            raise ValueError('Invalid/duplicate physical surface definition')
        row = {'name': name[1]}
        for field in ('BodyCollision', 'CameraCollision', 'RayCollision'):
            value = re.search(r'\b' + field + r'\s*=\s*(true|false)\b', match[2])
            if not value and field != 'RayCollision':
                raise ValueError('Missing collision property')
            row[field] = bool(value and value[1] == 'true')
        rows[number] = row
    if not rows or set(rows) != set(range(1, max(rows) + 1)) or len(rows) > 64:
        raise ValueError('Invalid physical surface numbering')
    return rows


def triangle_records(payload, kind, count, base, flags):
    """Yield original scalar-index triples and the lossless packed flags."""
    offset = 0
    for number in range(count):
        width = (6 if number == 0 else (9 if kind == 0 else 8)) if kind < 2 else {2: 5, 3: 4, 4: 5, 5: 3}.get(kind)
        if width is None or offset + width > len(payload):
            raise ValueError('Invalid/truncated packed collision triangle')
        b = payload[offset:offset + width]
        offset += width
        if kind < 2 and number == 0:
            f = flags | ((b[0] & 63) << 8) | ((b[0] >> 6) << 16)
            indices = base, b[1] | b[2] << 8 | (b[3] & 7) << 16, b[3] >> 3 | b[4] << 5 | b[5] << 13
        elif kind == 0:
            f = (b[1] & 63) | (b[0] & 63) << 8 | (b[0] >> 6) << 16
            indices = b[1] >> 7 | b[2] << 1 | b[3] << 9 | (b[4] & 3) << 17, b[4] >> 2 | b[5] << 6 | (b[6] & 31) << 14, b[6] >> 5 | b[7] << 3 | b[8] << 11
        elif kind == 1:
            f = flags | (b[0] & 63) << 8 | ((b[0] >> 6) & 1) << 16
            indices = b[0] >> 7 | b[1] << 1 | b[2] << 9 | (b[3] & 3) << 17, b[3] >> 2 | b[4] << 6 | (b[5] & 31) << 14, b[5] >> 5 | b[6] << 3 | b[7] << 11
        elif kind == 2:
            f = (b[1] & 63) | (b[0] & 63) << 8 | (b[0] >> 6) << 16
            indices = tuple(base + x for x in b[2:5])
        elif kind == 3:
            f = flags | (b[0] & 63) << 8 | (b[0] >> 6) << 16
            indices = tuple(base + x for x in b[1:4])
        elif kind == 4:
            f = flags | (b[0] & 63) << 8 | ((b[0] >> 6) & 1) << 16
            indices = base + (b[0] >> 7 | b[1] << 1 | (b[2] & 3) << 9), base + (b[2] >> 2 | (b[3] & 31) << 6), base + (b[3] >> 5 | b[4] << 3)
        else:
            f = flags | (b[0] & 63) << 8 | (b[0] >> 6) << 16
            indices = base + (b[1] & 31), base + (b[1] >> 5 | (b[2] & 3) << 3), base + (b[2] >> 2)
        yield indices, f


def inspect_collision(data, surfaces):
    if len(data) < 72 or data[:8] != b'!vq\x98\0\0\0\0':
        raise ValueError('Invalid CDB2 header')
    lower, upper = struct.unpack_from('<3i', data, 8), struct.unpack_from('<3i', data, 20)
    scale = struct.unpack_from('<3f', data, 32)
    if any(not math.isfinite(x) or x <= 0 for x in scale) or any(a > b for a, b in zip(lower, upper)):
        raise ValueError('Invalid CDB2 quantization')
    tri, vert = (64 + x for x in struct.unpack_from('<2I', data, 56))
    if not (72 <= tri <= vert <= len(data)) or (tri - 64) % 8 or (len(data) - vert) % 2:
        raise ValueError('Invalid CDB2 section extents')
    values = memoryview(data)[vert:].cast('h')  # macOS/fixtures are little endian.
    stack, seen = [0], set()
    kinds, counts, water, masks = Counter(), Counter(), [], Counter()
    while stack:
        node = stack.pop()
        if node in seen or node % 8 or node < 0 or node + 8 > tri - 64:
            raise ValueError('Invalid/shared/cyclic CDB2 node')
        seen.add(node)
        low, high = struct.unpack_from('<2I', data, 64 + node)
        if low & 3 != 3:
            child = low >> 8
            stack.extend((child, child + 8))
            continue
        mask, kind, offset = (low >> 2) & 15, (low >> 6) & 7, low >> 9
        count, flags, base = high & 127, (high >> 7) & 63, high >> 13
        if not count:
            continue
        if offset > vert - tri:
            raise ValueError('CDB2 triangle offset outside payload')
        packed = memoryview(data)[tri + offset:vert]
        for indices, packed_flags in triangle_records(packed, kind, count, base, flags):
            if any(i < 0 or i + 2 >= len(values) for i in indices):
                raise ValueError('CDB2 vertex index outside scalar payload')
            raw_surface = packed_flags & 63
            surface_id = raw_surface + 1
            counts[surface_id] += 1
            kinds[kind] += 1
            record = surfaces.get(surface_id)
            # Confirmed authored names + explicit ray property; wet mud is not
            # automatically promoted into this physical-water classification.
            if record and record['name'].lower().endswith('(water)') and record['RayCollision']:
                points = [tuple(values[i + k] * scale[k] for k in range(3)) for i in indices]
                water.append({'points': points, 'raw_surface_code': raw_surface, 'surface_id': surface_id,
                              'packed_flags': packed_flags, 'query_mask': mask})
                masks[mask] += 1
    bounds = None
    if water:
        points = [p for t in water for p in t['points']]
        bounds = {'min': [min(p[k] for p in points) for k in range(3)], 'max': [max(p[k] for p in points) for k in range(3)]}
    return {'triangles': sum(counts.values()), 'packed_kinds': dict(kinds), 'surface_counts': dict(counts),
            'water_triangles': water, 'water_query_masks': dict(masks), 'water_bounds_engine_xyz': bounds,
            'surface_id_join_runtime_verified': False,
            'surface_id_join': 'raw low-six-bit code + 1; loader consumes one-based BED entries in order; no live physics/render association established'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, default=Path('gamedata/data.bfs'))
    parser.add_argument('--output', type=Path, default=Path('work/metal/water-physics-research/catalog'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    definitions, collisions = [], []
    for index, payload in archive_assets(args.archive):
        if payload.startswith(b'!vq\x98'):
            collisions.append((index, payload))
        elif len(payload) < 100000 and b'Surfaces' in payload and b'"Water (water)"' in payload:
            definitions.append((index, payload, surface_definitions(payload)))
    if not definitions or len({hashlib.sha256(b).hexdigest() for _, b, _ in definitions}) != 1:
        raise ValueError('Missing or ambiguous physical surface table')
    table_index, payload, surfaces = definitions[0]
    summary = {'schema': 1, 'archive': str(args.archive), 'surface_table_asset': table_index,
               'surface_table_sha256': hashlib.sha256(payload).hexdigest(), 'physical_water_surfaces': {k: v for k, v in surfaces.items() if v['name'].lower().endswith('(water)')},
               'collision_render_association_verified': False, 'surface_id_join_runtime_verified': False, 'visual_water_enabled': False, 'levels': []}
    signatures = ['# fo2-zoom CDB quantization signatures v1: asset-index then 48-byte object prefix']
    for index, payload in collisions:
        signatures.append(f'{index} ' + (payload[44:56] + payload[32:44] + payload[8:32]).hex())
        report = inspect_collision(payload, surfaces)
        report['asset_index'] = index
        report['sha256'] = hashlib.sha256(payload).hexdigest()
        (args.output / f'collision-{index}.json').write_text(json.dumps(report, indent=2) + '\n')
        summary['levels'].append({k: v for k, v in report.items() if k not in ('water_triangles', 'surface_counts')})
        summary['levels'][-1]['water_triangle_count'] = len(report['water_triangles'])
    (args.output / 'signatures.tsv').write_text('\n'.join(signatures) + '\n')
    (args.output / 'catalog.json').write_text(json.dumps(summary, indent=2) + '\n')
    wet = [r for r in summary['levels'] if r['water_triangle_count']]
    print(f'Collision assets: {len(collisions)}, with physical-water candidates: {len(wet)}')
    for row in wet:
        print(row['asset_index'], row['water_triangle_count'], row['water_bounds_engine_xyz'], row['water_query_masks'])


if __name__ == '__main__':
    main()
