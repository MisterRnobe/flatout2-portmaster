#!/usr/bin/env python3
"""Prepare a camera-relative RT mesh from verified linear vertex programs.

No game rendering is changed. Unknown/deforming shaders are rejected, not guessed.
The result is a partial draw snapshot, not an out-of-frustum world scene.
"""
import argparse
import collections
import json
import math
from pathlib import Path
import struct
from check_trace_capture import validate

KNOWN_LINEAR_SHADERS = {'11799442587878920358', '3727128660738880625', '2238934220545660713'}


def matrix_rows(uniforms, data):
    rows = {}
    offset = 0
    for uniform in uniforms:
        if uniform['kind'] != 0:
            continue
        index, count = uniform['index'], uniform['count']
        if not (0 <= index < 256 and 0 < count <= 256-index):
            raise ValueError('Invalid float uniform range')
        if offset + count*16 > len(data):
            raise ValueError('Truncated packed float constants')
        for row in range(max(0, index), min(4, index+count)):
            if row in rows:
                raise ValueError('Duplicate matrix uniform')
            rows[row] = struct.unpack_from('<4f', data, offset+(row-index)*16)
        offset += count*16
    if set(rows) != {0, 1, 2, 3} or not all(math.isfinite(v) for row in rows.values() for v in row):
        raise ValueError('Missing/nonfinite c0..c3')
    return [rows[i] for i in range(4)]


def projection_from_matrix(rows):
    def dot(a, b):
        return sum(rows[a][i]*rows[b][i] for i in range(3))
    x, y, w = dot(0, 0), dot(1, 1), dot(3, 3)
    if min(x, y, w) < 1e-12:
        raise ValueError('Singular projection')
    if any(abs(dot(a, b)) > 1e-3*math.sqrt(dot(a, a)*dot(b, b)) for a, b in [(0, 3), (1, 3), (0, 1)]):
        raise ValueError('Unsupported nonrigid/asymmetric projection')
    a = dot(2, 3)/w
    b = rows[2][3]-a*rows[3][3]
    if not (.99999 <= a <= 10 and b < -1e-8):
        raise ValueError('Unsupported depth projection')
    return [math.sqrt(x/w), math.sqrt(y/w), a, b]


def transform_positions(points, rows, projection):
    result = []
    for p in points:
        clip = [sum(row[i]*p[i] for i in range(3))+row[3] for row in rows]
        view = [clip[0]/projection[0], clip[1]/projection[1], -clip[3]]
        # Match float3 storage consumed by Metal, then check reprojection error.
        view = struct.unpack('<3f', struct.pack('<3f', *view))
        projected = [view[0]*projection[0], view[1]*projection[1], -view[2]*projection[2]+projection[3], -view[2]]
        if not all(math.isfinite(v) for v in view) or any(abs(a-b) > 2e-4*max(1, abs(a), abs(b)) for a, b in zip(clip, projected)):
            raise ValueError('Vertex transform/projection mismatch')
        result.append(view)
    return result


def prepare(source, output):
    source, output = Path(source), Path(output)
    validate(source)
    doc = json.loads((source/'manifest.json').read_text())
    groups = collections.Counter()
    for m in doc['meshes']:
        groups[(m['color_target'], m['depth_target'])] += m['triangles']
    if not groups:
        raise ValueError('Capture contains no meshes')
    target = groups.most_common(1)[0][0]
    positions, indices, mesh_ids, accepted, rejected = [], [], [], [], []
    camera_projection = None
    for mesh_id, m in enumerate(doc['meshes']):
        reason = None
        if m['vertex_shader_hash'] not in KNOWN_LINEAR_SHADERS:
            reason = 'unknown-vertex-program'
        elif (m['color_target'], m['depth_target']) != target:
            reason = 'different-render-target'
        elif not m['projection_estimate_valid']:
            reason = 'missing-projection'
        if reason:
            rejected.append(dict(mesh=mesh_id, reason=reason)); continue
        text = (source/(m['stem']+'.vertex.metal')).read_text()
        if any(text.count(f'oPos.{axis} = dot(v0, c{row});') != 1 for row, axis in enumerate('xyzw')):
            raise ValueError('Verified vertex shader shape changed')
        rows = matrix_rows(m['uniforms'], (source/(m['stem']+'.constants.bin')).read_bytes())
        projection = projection_from_matrix(rows)
        if any(not math.isclose(a, b, rel_tol=2e-4, abs_tol=2e-5) for a, b in zip(projection, m['projection_estimate'])):
            raise ValueError('Captured/decoded projection mismatch')
        if camera_projection is not None and any(not math.isclose(a, b, rel_tol=2e-4, abs_tol=2e-5) for a, b in zip(projection, camera_projection)):
            rejected.append(dict(mesh=mesh_id, reason='different-camera-projection')); continue
        camera_projection = camera_projection or projection
        points = list(struct.iter_unpack('<3f', (source/(m['stem']+'.positions.f32')).read_bytes()))
        view = transform_positions(points, rows, projection)
        base = len(positions)
        local_indices = [v[0] for v in struct.iter_unpack('<I', (source/(m['stem']+'.indices.u32')).read_bytes())]
        first_triangle = len(indices)//3
        positions.extend(view); indices.extend(base+i for i in local_indices)
        mesh_ids.extend([mesh_id]*m['triangles'])
        accepted.append(dict(mesh=mesh_id, first_triangle=first_triangle, triangles=m['triangles'],
                             vertex_shader_hash=m['vertex_shader_hash'], material_cue=m['material_cue']))
    if not accepted:
        raise ValueError('No verified linear meshes for this target')
    output.mkdir(parents=True, exist_ok=True)
    (output/'scene.json').unlink(missing_ok=True)
    (output/'positions.f32').write_bytes(b''.join(struct.pack('<3f', *p) for p in positions))
    (output/'indices.u32').write_bytes(struct.pack('<'+'I'*len(indices), *indices))
    (output/'triangle-mesh.u32').write_bytes(struct.pack('<'+'I'*len(mesh_ids), *mesh_ids))
    bounds = {name: [fn(positions[i][k] for i in indices) for k in range(3)] for name, fn in [('minimum', min), ('maximum', max)]}
    report = dict(schema=1, status='cpu-prepared', coordinate_space='right-handed-view',
                  native_rt_gpu_validated=False, world_space_validated=False, source_frame=doc['frame'],
                  source_capture_status=doc['status'], coverage='submitted-captured-draws-only',
                  target=list(target), projection=camera_projection, vertices=len(positions), triangles=len(indices)//3,
                  accepted=accepted, rejected=rejected, **bounds)
    (output/'scene.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    report = prepare(args.source, args.output)
    print(json.dumps({k: report[k] for k in ['status', 'coordinate_space', 'vertices', 'triangles', 'source_capture_status', 'native_rt_gpu_validated']}, indent=2))


if __name__ == '__main__':
    main()
