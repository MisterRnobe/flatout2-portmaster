#!/usr/bin/env python3
"""Summarize bounded master-list TCP and QR-master captures without networking."""
import argparse
import json
from pathlib import Path
import re


def inspect_decoded_list(payload):
    """Recognize a complete canonical empty V2 list after external decoding.

    Parse the typed key schema, then the empty string table + terminal marker.
    Nonempty record layouts are deliberately not counted by this small parser.
    """
    if len(payload) < 7:
        raise ValueError('Truncated decoded list header')
    cursor, fields = 7, []
    for _ in range(payload[6]):
        if cursor >= len(payload):
            raise ValueError('Truncated decoded key schema')
        end = payload.index(0, cursor+1)
        fields.append(payload[cursor+1:end].decode('ascii'))
        cursor = end+1
    tail = payload[cursor:]
    return dict(fields=fields, canonical_empty_list=tail == b'\x00\x00\xff\xff\xff\xff',
                record_section_bytes=len(tail), has_terminal_marker=tail.endswith(b'\x00\xff\xff\xff\xff'))


def summarize(root):
    root = Path(root)
    groups = {}
    for p in sorted(root.glob('*.bin')):
        match = re.fullmatch(r'([0-9a-f]{8})-(send|recv)-(\d{3})\.bin', p.name)
        if match:
            groups.setdefault(match[1], {'send': [], 'recv': []})[match[2]].append(p)
    streams = []
    for handle, parts in sorted(groups.items()):
        request = b''.join(p.read_bytes() for p in parts['send'])
        received = sum(p.stat().st_size for p in parts['recv'])
        entry = dict(socket=handle, sent_bytes=len(request), received_bytes=received,
                     game_identifiers=sorted({x.decode('ascii') for x in re.findall(rb'flatout2(?:pc|zp)', request)}),
                     status='response-captured' if received else 'request-only' if request else 'no-payload')
        decoded = root/(handle+'-decoded.bin')
        if decoded.exists():
            entry['decoded_list'] = inspect_decoded_list(decoded.read_bytes())
        streams.append(entry)
    qr = []
    for p in sorted((root/'qr2').glob('*.bin')):
        match = re.fullmatch(r'([0-9a-f]{8})-(tx|rx)-(\d{3})\.bin', p.name)
        if not match:
            continue
        packet = p.read_bytes()
        kind = packet[2] if packet.startswith(b'\xfe\xfd') and len(packet) >= 3 else packet[0] if packet else None
        qr.append(dict(socket=match[1], direction=match[2], bytes=len(packet),
                       type_hex=f'{kind:02x}' if kind is not None else None,
                       game_identifiers=sorted({x.decode('ascii') for x in re.findall(rb'flatout2(?:pc|zp)', packet)})))
    return dict(master_streams=streams, qr_master_packets=qr,
                qr_master_reply_captured=any(p['direction']=='rx' for p in qr),
                public_registration_verified=False,
                note='TCP replies are encrypted; QR contact alone does not prove public visibility or joinability.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error('trace directory does not exist')
    text = json.dumps(summarize(args.directory), indent=2)+'\n'
    (args.directory/'summary.json').write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
