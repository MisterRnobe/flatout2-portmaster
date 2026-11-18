"""Catalog mode must preserve recovery of direct/indirect branches and callbacks."""
import contextlib
import io
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'work/pcrecomp/tools/disasm'))
from disasm32 import Disassembler

class CatalogMemoryTest(unittest.TestCase):
    def test_catalog_matches_full_operands(self):
        base = 0x400000
        start = base + 0x1000
        text = bytearray(b'\xcc' * 0x200)
        def put(offset, code): text[offset:offset+len(code)] = code
        # Branch join, a directly called function, an indirect jump, a callback
        # passed in an immediate, and a memory operand with an address-like value.
        put(0, b'\x55\x8b\xec\x85\xc0\x74\x05\xe8' + (0x40-12).to_bytes(4,'little',signed=True) + b'\x68' + (start+0x80).to_bytes(4,'little') + b'\x5d\xc3')
        put(0x40, b'\x8b\x44\x24\x04\x83\xc0\x01\xc3')
        put(0x80, b'\x8b\x44\x24\x04\x83\xc0\x02\xc3')
        put(0xc0, b'\xff\x25' + (start+0x180).to_bytes(4,'little'))
        put(0xe0, b'\xff\x24\x85' + (start+0x180).to_bytes(4,'little'))
        put(0x180, (start+0x80).to_bytes(4,'little') + (start+0x100).to_bytes(4,'little') + b'\x00'*4)
        put(0x100, b'\xc7\x05' + (start+0x180).to_bytes(4,'little') + (start+0x80).to_bytes(4,'little') + b'\xc3')
        section=SimpleNamespace(name='.text',virtual_address=0x1000,raw_offset=0x400,raw_size=len(text),virtual_size=len(text),is_code=True)
        def recover(compact):
            dis=Disassembler(b'\x00'*0x400+text,base,[section])
            with contextlib.redirect_stdout(io.StringIO()):
                funcs=dis.find_functions(start,start+len(text),{},seeds=[start,start+0xc0,start+0xe0,start+0x100],release_operands=compact)
            return {a:(f.end,f.size,f.entry_kind,f.is_thunk,sorted(f.calls_to),sorted(f.jump_targets),sorted((b.start,b.end,sorted(b.successors),b.is_exit) for b in f.blocks.values())) for a,f in funcs.items()}
        full=recover(False)
        self.assertEqual(full,recover(True))
        self.assertIn(start+0x40,full)
        self.assertIn(start+0x80,full)

if __name__=='__main__':unittest.main()
