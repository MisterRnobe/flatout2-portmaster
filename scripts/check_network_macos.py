#!/usr/bin/env python3
"""Check the actual address adapter; optionally exercise loopback and OpenSpy TCP."""
import argparse
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--offline',action='store_true',help='ABI and local stream tests; no external network')
parser.add_argument('--openspy',action='store_true',help='Also resolve/connect to the OpenSpy master; no server-list request')
args=parser.parse_args()
if args.offline and args.openspy:parser.error('--offline and --openspy are mutually exclusive')
out=ROOT/'work/macos';out.mkdir(parents=True,exist_ok=True)
binary=out/'network-check'
subprocess.run(['clang++','-std=c++17','-O2','-I'+str(ROOT/'runtime/guest'),
                str(ROOT/'tests/socket_address_check.cpp'),'-o',str(binary)],check=True)
mode='--offline' if args.offline else '--openspy' if args.openspy else '--loopback'
log=out/('network'+mode[1:]+'.log')
try:
    result=subprocess.run([str(binary),mode],capture_output=True,text=True,timeout=25)
    output=result.stdout+result.stderr
    log.write_text(output);print(output,end='');print('Log:',log)
    raise SystemExit(result.returncode)
except subprocess.TimeoutExpired as error:
    log.write_bytes((error.stdout or b'')+(error.stderr or b'')+b'FAIL timeout after 25 seconds\n')
    print('Network check timed out. Log:',log)
    raise SystemExit(1)
