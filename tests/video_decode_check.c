/* Run the production VP3 decoder and guest frame queue without SDL or a GPU. */
#include "recomp_types.h"
#include <stdio.h>
uint32_t g_eax,g_esp;
ptrdiff_t g_mem_base;
extern int fo2_video_available(void);
extern void fo2_video_decode_frame(void);
extern void fo2_video_close(uint32_t player);
static uint32_t readWord(FILE* f){uint32_t v;if(fread(&v,4,1,f)!=1)exit(3);return v;}
int main(int argc,char**argv){
    if(argc!=2)return 3;
    FILE* f=fopen(argv[1],"rb");if(!f)return 3;
    unsigned w=readWord(f),h=readWord(f),tag=readWord(f),frames=readWord(f);
    if(w>1920||h>1080)return 3;
    unsigned char* memory=calloc(1,16<<20);g_mem_base=(ptrdiff_t)memory;
    const uint32_t player=0x1000,queue=player+0x5c,data=0x10000,out=0x400000;
    unsigned pixels=w*h*3;unsigned nonblack=0,changed=0;uint32_t previous=0;
    if(!fo2_video_available())return 4;
    MEM32(player+0xa8)=w;MEM32(player+0xac)=h;MEM32(player+0xb4)=tag;
    MEM32(player+0x2b8)=w;MEM32(player+0x2bc)=h;MEM32(player+0x2dc)=out;
    MEM32(queue+0x70)=1;
    for(unsigned frame=0;frame<frames;++frame){
        unsigned size=readWord(f);if(size>0x300000)return 3;
        if(fread(memory+data,1,size,f)!=size)return 3;
        memset(memory+data+size,0,64);
        MEM32(queue+0x74)=1;MEM32(queue+0x78)=0;
        MEM32(queue+0x7c)=data;MEM32(queue+0x80)=size;
        MEM32(out+pixels)=0xfeedbeef;g_eax=player;g_esp=512;
        fo2_video_decode_frame();
        if(g_eax!=1||g_esp!=516||MEM32(queue+0x74)||MEM32(out+pixels)!=0xfeedbeef)return 5;
        uint32_t hash=2166136261u;unsigned sum=0;
        for(unsigned p=0;p<pixels;++p){sum+=memory[out+p];hash=(hash^memory[out+p])*16777619u;}
        nonblack+=sum>pixels;changed+=frame&&hash!=previous;previous=hash;
    }
    fo2_video_close(player);fclose(f);free(memory);
    printf("Decoded %u VP3 frames (%ux%u): %u nonblack, %u changed; queue/stack/output guard passed\n",frames,w,h,nonblack,changed);
    return nonblack&&changed?0:6;
}
