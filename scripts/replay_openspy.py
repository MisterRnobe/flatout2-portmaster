#!/usr/bin/env python3
"""Replay captured master-list requests to distinguish transport from game processing."""
import argparse
from pathlib import Path
import socket
import re
ROOT=Path(__file__).resolve().parents[1]

def queries(session):
    groups={}
    for path in sorted(session.glob('*-send-*.bin')):
        groups.setdefault(path.name.split('-')[0],[]).append(path)
    return {handle:b''.join(p.read_bytes() for p in paths) for handle,paths in groups.items()}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--offline',action='store_true',help='Inspect captures without opening sockets')
    parser.add_argument('--since',type=float,default=0,help='Only consider sessions modified since this Unix timestamp')
    parser.add_argument('--session',type=Path,help='Inspect/replay one explicit trace directory')
    args=parser.parse_args()
    root=ROOT/'work/macos/network-trace'
    candidates=[args.session] if args.session else list(root.glob('*'))
    sessions=[p for p in candidates if p.is_dir() and p.stat().st_mtime>=args.since and queries(p)]
    if not sessions:
        print('No master-list request captured in this run.');return 2
    session=max(sessions,key=lambda p:p.stat().st_mtime)
    lines=['Session: '+str(session)]
    failed=False
    for handle,request in queries(session).items():
        games=sorted({m.decode('ascii') for m in re.findall(rb'flatout2(?:pc|zp)',request)})
        lines.append(f'{handle}: captured send={len(request)} B, game identifiers={games}')
        captured=b''.join(p.read_bytes() for p in sorted(session.glob(handle+'-recv-*.bin')))
        lines.append(f'{handle}: game received={len(captured)} B')
        if args.offline:continue
        response=bytearray()
        try:
            with socket.create_connection(('flatout2pc.ms8.openspy.net',28910),timeout=5) as connection:
                connection.sendall(request)
                while len(response)<262144:
                    try:packet=connection.recv(min(16384,262144-len(response)))
                    except socket.timeout:break
                    if not packet:break
                    response.extend(packet)
            (session/(handle+'-replay-response.bin')).write_bytes(response)
            lines.append(f'{handle}: standalone replay received={len(response)} B (encrypted list is not decoded)')
            if not response:failed=True
        except OSError as error:
            failed=True;lines.append(f'{handle}: replay failed: {error}')
    text='\n'.join(lines)+'\n'
    log=ROOT/'work/macos/openspy-replay.log';log.parent.mkdir(parents=True,exist_ok=True);log.write_text(text)
    if args.session:(session/'replay.log').write_text(text)
    print(text,end='');print('Log: work/macos/openspy-replay.log')
    return 1 if failed else 0
if __name__=='__main__':raise SystemExit(main())
