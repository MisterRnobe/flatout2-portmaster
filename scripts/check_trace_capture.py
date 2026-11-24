#!/usr/bin/env python3
"""Validate an opt-in raw-position RT geometry capture without a Metal device."""
import argparse
import json
import math
from pathlib import Path
import struct


def validate(directory):
    directory = Path(directory)
    manifest_path = directory / 'manifest.json'
    if manifest_path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError('Oversized manifest')
    doc = json.loads(manifest_path.read_text())
    if (doc.get('schema') != 1 or doc.get('coordinate_space') != 'raw-position-input'
            or doc.get('world_space_validated') is not False
            or doc.get('vertex_shader_evaluated') is not False or doc.get('byte_order') != 'little-endian'):
        raise ValueError('Unsupported geometry/transform contract')
    if doc.get('status') not in ('complete', 'limited', 'no-mesh'):
        raise ValueError('Capture is incomplete')
    meshes = doc['meshes']
    if len(meshes) > 512:
        raise ValueError('Too many meshes')
    vertices = triangles = payload = 0
    stems = set()
    for mesh in meshes:
        stem = mesh['stem']
        if stem != f'mesh-{len(stems)}' or stem in stems:
            raise ValueError('Invalid/duplicate mesh filename')
        stems.add(stem)
        n, t = mesh['vertices'], mesh['triangles']
        if not (0 < n <= 1000000 and 0 < t <= 1000000):
            raise ValueError('Invalid mesh dimensions')
        specs = [('.positions.f32', n * 12), ('.indices.u32', t * 12),
                 ('.vertex.metal', mesh['vertex_source_bytes']), ('.constants.bin', mesh['constant_bytes'])]
        files = []
        for suffix, size in specs:
            if not (0 <= size <= 16 * 1024 * 1024):
                raise ValueError('Oversized mesh file')
            path = directory / (stem + suffix)
            if path.stat().st_size != size:
                raise ValueError(f'Size mismatch: {path.name}')
            payload += size
            if payload > 64 * 1024 * 1024:
                raise ValueError('Oversized capture payload')
            files.append(path.read_bytes())
        points = list(struct.iter_unpack('<fff', files[0]))
        indices = [v[0] for v in struct.iter_unpack('<I', files[1])]
        if not all(math.isfinite(v) for p in points for v in p):
            raise ValueError('Nonfinite vertex')
        if max(indices) >= n:
            raise ValueError('Index outside vertex buffer')
        for i in range(0, len(indices), 3):
            p, q, r = (points[j] for j in indices[i:i+3])
            u, v = [q[k]-p[k] for k in range(3)], [r[k]-p[k] for k in range(3)]
            cross = [u[(k+1)%3]*v[(k+2)%3]-u[(k+2)%3]*v[(k+1)%3] for k in range(3)]
            scale = sum(a*a+b*b for a, b in zip(u, v))
            if sum(c*c for c in cross) <= scale*scale*1e-20:
                raise ValueError('Degenerate triangle in prepared mesh')
        for key, fn in [('minimum', min), ('maximum', max)]:
            actual = [fn(points[j][k] for j in indices) for k in range(3)]
            if len(mesh[key]) != 3 or not all(math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-6)
                                              for a, b in zip(actual, mesh[key])):
                raise ValueError('Incorrect mesh bounds')
        h = 14695981039346656037
        for byte in files[2]:
            h = ((h ^ byte) * 1099511628211) & 0xffffffffffffffff
        if str(h) != mesh['vertex_shader_hash']:
            raise ValueError('Vertex shader source hash mismatch')
        vertices += n
        triangles += t
    if (vertices, triangles, payload) != (doc['vertices'], doc['triangles'], doc['payload_bytes']):
        raise ValueError('Incorrect capture totals')
    results = doc['results']
    if results.get('ready') != len(meshes) or sum(results.values()) != doc['draws_attempted']:
        raise ValueError('Incorrect capture result counts')
    limits = doc['limits']
    if (vertices > limits['vertices'] or triangles > limits['triangles'] or payload > limits['payload_bytes']
            or doc['draws_attempted'] > limits['draws'] or doc['input_elements_budgeted'] > limits['input_elements']):
        raise ValueError('Capture exceeded its budget')
    return dict(status=doc['status'], meshes=len(meshes), vertices=vertices, triangles=triangles,
                payload_bytes=payload, coordinate_space=doc['coordinate_space'], world_space_validated=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.directory), indent=2))


if __name__ == '__main__':
    main()
