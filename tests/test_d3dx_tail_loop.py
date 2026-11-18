"""Replay the exact lifted D3DX loop from the macOS stack-overflow report."""
import pathlib
import re
import signal
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class D3DXTailLoopTest(unittest.TestCase):
    def test_real_lifted_loop_with_a_million_accumulations(self):
        generated=(ROOT/'work/gen/d3dx_recomp_0015.c').read_text()
        addresses=[0xad70f2,0xad7107,0xad711a,0xad7169,0xad7179,0xad7187]
        code=[]
        for address in addresses:
            match=re.search(r'void sub_%08X\(void\) \{.*?\n\}'%address,generated,re.S)
            self.assertIsNotNone(match)
            code.append(match[0])
        declarations='\n'.join('void sub_%08X(void);'%a for a in addresses)
        lookup=''.join('case 0x%x:return sub_%08X;'%(a,a) for a in addresses)
        source=r'''
#define RECOMP_GENERATED_CODE
#define RECOMP_LOCAL_REGS
#define RECOMP_TLS_STRUCT
#define RECOMP_TLS __thread
#define RECOMP_X87_WINDOW
#include "arm64_guest.h"
__thread RecompCpu g_cpu;
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE],g_icall_trace_idx,g_icall_count;
unsigned char memory[1<<20]; ptrdiff_t g_mem_base=(ptrdiff_t)memory;
DECLARATIONS
void finish(void){g_esp+=4;}
recomp_func_t recomp_lookup_manual(uint32_t va){return 0;}
recomp_func_t recomp_lookup_import(uint32_t va){return 0;}
recomp_func_t recomp_lookup_cacheable(uint32_t va){return 0;}
recomp_func_t recomp_lookup(uint32_t va){switch(va){LOOKUP case 0xad7431:return finish;default:abort();}}
@@CODE@@
static void storeFloat(uint32_t address,float value){memcpy(memory+address,&value,4);}
int main(int argc,char**argv){
 g_cpu.fp_sp=128;
 const unsigned count=argc>1?8:1024;
 const uint32_t records=0x1000,indices=0x10000,matrix=0x20000,output=0x21000,inputs=0x30000,frame=0x80000;
 for(unsigned i=0;i<count;++i){
  MEM32(records+12*i)=12;MEM32(records+12*i+4)=0;storeFloat(records+12*i+8,0.5f);
  MEM32(indices+8*i)=0;storeFloat(indices+8*i+4,1.0f);
  for(unsigned c=0;c<4;++c)storeFloat(inputs+16*i+4*c,c+1);
 }
 MEM32(matrix)=output;
 MEM32(frame-0x2c)=indices;MEM32(frame-0x10)=indices+count*8;
 MEM32(frame-0x28)=records+count*12;MEM32(frame-8)=matrix;
 g_eax=records;g_edx=inputs+8;g_ebp=frame;g_esp=512;
 sub_00AD70F2();
 if(g_esp!=516)return 1;
 for(unsigned c=0;c<4;++c){float value;memcpy(&value,memory+output+c*4,4);
  if(value!=count*count*0.5f*(c+1))return 2;}
 return 0;
}
'''.replace('DECLARATIONS',declarations).replace('LOOKUP',lookup).replace('@@CODE@@','\n'.join(code))
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp)
            begin=source.index('recomp_func_t recomp_lookup_manual')
            end=source.index('\nvoid sub_00AD70F2(void) {',begin)
            resolver=source[begin:end]
            (p/'lookup.c').write_text('#include "arm64_guest.h"\n'+declarations+'\nvoid finish(void);\n'+resolver)
            (p/'replay.c').write_text(source[:begin]+source[end:])
            for tail in [False,True]:
                binary=p/('tail' if tail else 'calls')
                subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-O2',
                    *(['-DNFS_NATIVE_TAIL'] if tail else []),
                    '-I'+str(ROOT/'runtime/guest'),'-I'+str(ROOT/'work/macos/include'),
                    str(p/'replay.c'),str(p/'lookup.c'),'-fno-strict-aliasing','-fexceptions','-ffp-contract=off','-o',str(binary)],check=True)
                subprocess.run([str(binary),*(['short'] if not tail else [])],check=True,timeout=30)
                if not tail:
                    failed=subprocess.run([str(binary)],timeout=30,capture_output=True)
                    self.assertIn(failed.returncode,[-signal.SIGBUS,-signal.SIGSEGV])
if __name__=='__main__':unittest.main()
