/* SPDX-License-Identifier: MIT */
#include "radar_crc32.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static uint32_t bitwise(uint32_t s,const void *data,size_t n)
{
    const unsigned char *p=data;unsigned b;
    while(n--) { s^=*p++;for(b=0;b<8U;++b)s=(s>>1)^(0xedb88320U&(0U-(s&1U))); }
    return s;
}
int main(int argc,char **argv)
{
    uint8_t *p=malloc(400000U);uint32_t seed=0x8a751U;size_t i,n;unsigned align;FILE *f;
    assert(p);for(i=0;i<400000U;++i) { seed=1664525U*seed+1013904223U;p[i]=(uint8_t)(seed>>24); }
    assert((radar_crc32_update(UINT32_MAX,"123456789",9)^UINT32_MAX)==0xcbf43926U);
    for(align=0;align<16U;++align)for(n=0;n<2048U;++n)
        assert(radar_crc32_update(seed,p+align,n)==bitwise(seed,p+align,n));
    for(n=1;n<=399360U;n=n*3U+1U)assert(radar_crc32_update(UINT32_MAX,p,n)==bitwise(UINT32_MAX,p,n));
    for(align=1;align<=4096U;align*=2U) {
        uint32_t crc=UINT32_MAX;size_t offset=0;
        while(offset<399360U) { n=399360U-offset;if(n>align)n=align;crc=radar_crc32_update(crc,p+offset,n);offset+=n; }
        assert(crc==bitwise(UINT32_MAX,p,399360U));
    }
    if(argc>1) {
        f=fopen(argv[1],"rb");assert(f);assert(fread(p,1,399360U,f)==399360U);assert(fgetc(f)==EOF);fclose(f);
        assert(radar_crc32_update(UINT32_MAX,p,399360U)==bitwise(UINT32_MAX,p,399360U));
        printf("RAW CRC=%08x\n",radar_crc32_update(UINT32_MAX,p,399360U)^UINT32_MAX);
    }
    free(p);puts("PASS CRC bitwise identity: offsets0..15, lengths0..2047, full frame, incremental chunks, known check");return 0;
}
