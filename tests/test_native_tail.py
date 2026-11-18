"""Exercise the real ARM64 transfer macros on a long guest loop without a GPU."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCE=r'''
#define RECOMP_GENERATED_CODE
#define RECOMP_LOCAL_REGS
#include "arm64_guest.h"
uint32_t g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi,g_cur_func,nfs_icall_va;
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE],g_icall_trace_idx,g_icall_count;
unsigned char memory[1024]; ptrdiff_t g_mem_base=(ptrdiff_t)memory;
static uintptr_t low=UINTPTR_MAX,high;
static void record(uintptr_t p){if(p<low)low=p;if(p>high)high=p;}
void first(void);void second(void);
recomp_func_t recomp_lookup_manual(uint32_t va){return 0;}
recomp_func_t recomp_lookup_import(uint32_t va){return 0;}
recomp_func_t recomp_lookup_cacheable(uint32_t va){return 0;}
recomp_func_t recomp_lookup(uint32_t va){return va==1?first:va==2?second:0;}
#define LOCALS uint32_t _flag_k=0,_flag_a=0,_flag_b=0,_cf=0;RECOMP_REGS_LOCALS
__attribute__((noinline)) void first(void){
 LOCALS;RECOMP_ENTER(1);volatile char marker;record((uintptr_t)&marker);
 if(eax){eax--;RECOMP_ITAIL(2);RECOMP_RETURN();}
 // A stdcall return removes the caller's slot and one DWORD argument.
 esp+=8;RECOMP_RETURN();
}
__attribute__((noinline)) void second(void){
 LOCALS;RECOMP_ENTER(2);volatile char marker;record((uintptr_t)&marker);
 ecx+=3;RECOMP_ITAIL(1);RECOMP_RETURN();
}
int main(int argc,char**argv){
 g_eax=argc>1?1000:1000000;uint32_t count=g_eax;g_ecx=0;g_esp=512;
 first();
 if(g_eax||g_ecx!=count*3||g_esp!=520)return 1;
 if(high-low>4096)return 2;
 return 0;
}
'''
class NativeTailTest(unittest.TestCase):
    def test_million_transfers_keep_native_stack_constant(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'loop.c').write_text(SOURCE)
            for tail in [False,True]:
                binary=p/('tail' if tail else 'calls')
                subprocess.run(['/opt/homebrew/opt/llvm/bin/clang','-O2',
                    *(['-DNFS_NATIVE_TAIL'] if tail else []),
                    '-I'+str(ROOT/'runtime/guest'),'-I'+str(ROOT/'work/macos/include'),
                    str(p/'loop.c'),'-o',str(binary)],check=True,capture_output=True)
                result=subprocess.run([str(binary),*(['short'] if not tail else [])],timeout=20)
                # The old transfer already grows the stack over 4 KB in 1000 steps.
                self.assertEqual(result.returncode,0 if tail else 2)
if __name__=='__main__':unittest.main()
