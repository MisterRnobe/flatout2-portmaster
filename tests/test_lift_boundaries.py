"""Execute full and bounded lifted code across a conditional and a fallthrough."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
os.environ['PCRECOMP']=str(ROOT/'work/pcrecomp')
sys.path.insert(0,str(ROOT/'scripts'))
import run_lift
from lift32 import Lifter
from capstone import Cs,CS_ARCH_X86,CS_MODE_32

class LiftBoundaryTest(unittest.TestCase):
    def test_guest_results_and_stack_match(self):
        base=0x400000; start=base+0x1000
        rawimage=b'\x00'*0x1000+bytes.fromhex('83 f8 00 74 0b b8 07 00 00 00 eb 0c cc cc cc cc b8 0b 00 00 00 83 c0 03 c3')
        def descriptor(address,end,blocks):
            return dict(address=address,end=end,size=end-address,is_thunk=False,entry_kind='start',jump_targets=[],calls_to=[],blocks=blocks)
        entries=[descriptor(start,start+21,[[start,start+5,[start+5,start+16],0],[start+5,start+12,[start+24],0],[start+16,start+25,[],1]]),
                 descriptor(start+5,start+12,[[start+5,start+12,[start+24],0]]),
                 descriptor(start+16,start+25,[[start+16,start+25,[],1]]),
                 descriptor(start+21,start+25,[[start+21,start+25,[],1]]),
                 descriptor(start+24,start+25,[[start+24,start+25,[],1]])]
        declarations='\n'.join(f'void sub_{f["address"]:08X}(void);' for f in entries)
        harness=r'''
uint32_t g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi,g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE],g_icall_trace_idx,g_icall_count;
unsigned char memory[1024]; ptrdiff_t g_mem_base=(ptrdiff_t)memory;
recomp_func_t recomp_lookup_manual(uint32_t va){return 0;}
recomp_func_t recomp_lookup_import(uint32_t va){return 0;}
recomp_func_t recomp_lookup(uint32_t va){switch(va){
case 0x401005:return sub_00401005;case 0x401010:return sub_00401010;case 0x401015:return sub_00401015;case 0x401018:return sub_00401018;default:return 0;}}
int main(void){for(int i=-1;i<2;++i){g_eax=(uint32_t)i;g_esp=512;sub_00401000();
if(g_eax!=(i==0?14u:7u)||g_esp!=516)return 1;}return 0;}
'''
        cases=[('83f800740b','i==0',0), ('909090e30b','i==0',0),
               ('909090e20b','i!=1',0), ('83fa00e10b','i!=1',0),
               ('83fa00e10b','0',1), ('83fa00e00b','0',0),
               ('83fa00e00b','i!=1',1)]
        for prefix, condition, edx in cases:
            image=rawimage[:0x1000]+bytes.fromhex(prefix)+rawimage[0x1005:]
            case_harness=harness.replace('i==0?14u:7u',f'({condition})?14u:7u')
            case_harness=case_harness.replace('g_esp=512;',f'g_ecx=(uint32_t)i;g_edx={edx};g_esp=512;')
            for boundary in [5,21]:
                entries[0]["end"]=start+boundary
                entries[0]["size"]=boundary
                for bounded in [False,True]:
                    md=Cs(CS_ARCH_X86,CS_MODE_32);md.detail=True
                    lifter=Lifter(lifted={f['address'] for f in entries},hook_sites=set(),precise_carry=True)
                    code='\n'.join(lifter.lift_function(run_lift.rebuild(md,image,base,fd,bounded=bounded)) for fd in entries)
                    with tempfile.TemporaryDirectory() as tmp:
                        source=Path(tmp)/'check.c';binary=Path(tmp)/'check'
                        source.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'+declarations+'\n'+code+'\n'+case_harness)
                        subprocess.run(['clang','-O2','-DRECOMP_LOCAL_REGS','-I'+str(ROOT/'work/macos/include'),str(source),'-o',str(binary)],check=True,capture_output=True)
                        subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main()
