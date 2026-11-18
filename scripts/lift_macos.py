#!/usr/bin/env python3
"""POSIX equivalent of catalog.ps1, catalog_module.ps1 and lw.ps1."""
import os, signal, subprocess, sys, threading
from pathlib import Path
root = Path(__file__).resolve().parent.parent
os.chdir(root)
py = sys.executable
pc = Path(os.environ.get('PCRECOMP', root / 'work/pcrecomp')).resolve()
os.environ['PCRECOMP'] = str(pc)
gen = root / 'work/gen'
children = set()
children_lock = threading.Lock()
def run(script, *args):
    process = subprocess.Popen([py, '-u', '-I', str(script), *map(str,args)], start_new_session=True)
    with children_lock: children.add(process)
    try:
        result = process.wait()
        if result: raise subprocess.CalledProcessError(result, process.args)
    finally:
        with children_lock: children.discard(process)
def stop_children():
    with children_lock:
        for process in children:
            if process.poll() is None:
                try: os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError: pass
def catalog(work, exe, prefix=''):
    previous = -1
    for _ in range(4):
        count = int(subprocess.check_output([py, '-u', '-I', 'scripts/merge_seeds.py', str(work)]))
        if count == previous: break
        previous = count
        run('scripts/run_lift.py', 'catalog', exe, work/'seeds_all.json', work/'catalog.json')
        run('scripts/jump_table_seeds.py', exe, work/'catalog.json', work/'jumptable_seeds.json')
    if not prefix:
        run('scripts/sled_seeds.py', exe, work/'catalog.json')
        run('scripts/data_pointer_rescue.py', exe, work/'catalog.json')
    run('scripts/run_lift.py', 'lift', exe, work/'catalog.json', gen, *(['--prefix',prefix,'--bounded'] if prefix else []))
work = root/'work/lift'
def lift_exe():
    if not (gen/'lift_stats.json').exists():
        run(pc/'tools/cpp/vtable_scan.py', work/'FlatOut2.exe', '--seeds', work/'vtable_seeds.json')
        run(pc/'tools/cpp/rtti.py', work/'FlatOut2.exe', '--seeds', work/'rtti_seeds.json')
        catalog(work,work/'FlatOut2.exe')
def lift_dll():
    if not (gen/'d3dx_lift_stats.json').exists():
        dll = root/'work/d3dx'; dll.mkdir(exist_ok=True)
        run('scripts/prepare_module.py', work/'d3dx9_30.dll', '0x00A00000', dll/'d3dx9_30.dll', dll/'reloc_seeds.json')
        catalog(dll,dll/'d3dx9_30.dll','d3dx_')
# Each module has independent catalogs and prefixed generated files.
from concurrent.futures import ThreadPoolExecutor
pool = ThreadPoolExecutor(max_workers=2)
try:
    futures=[pool.submit(lift_exe),pool.submit(lift_dll)]
    for future in futures: future.result()
except BaseException:
    stop_children()
    raise
finally:
    pool.shutdown(wait=True, cancel_futures=True)
info = gen/'nfs_lw_info.json'
run('scripts/lw/annotate.py','strip',gen)
run('scripts/lw/analyze.py',gen,info)
run('scripts/lw/annotate.py','apply',gen,info)
run('scripts/lw/annotate.py','header',gen,info,gen/'nfs_lw.h')
run('scripts/x87float.py',gen,info,'config/x87_float.txt')
