/* SPDX-License-Identifier: MIT
 * Synthetic monitor capability fixtures; this does not prove a DP link. */
#include "../radar_runtime/src/display/radar_display_edid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void checksum(uint8_t *p)
{
    unsigned i,sum=0U;
    for (i=0U;i<127U;++i) { sum+=p[i]; }
    p[127]=(uint8_t)(0U-sum);
}
static void base(uint8_t *p)
{
    memset(p,0,256U);
    memset(p+1,255,6U);
    p[18]=1U; p[19]=4U;
    checksum(p);
}
static void svga_dtd(uint8_t *d)
{
    memset(d,0,18U);
    d[0]=0xa0U; d[1]=0x0fU; /* EDID nominal40MHz, not an oscillator measurement. */
    d[2]=0x20U; d[3]=0x00U; d[4]=0x31U;
    d[5]=0x58U; d[6]=0x1cU; d[7]=0x20U;
    d[8]=40U; d[9]=128U; d[10]=0x14U; d[17]=0x1eU;
}
static void full_hd_dtd(uint8_t *d)
{
    /* CEA VIC16 1080p60, common monitor DTD encoding. */
    static const uint8_t timing[18]={
        0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,0x58,0x2c,0x45,0x00,
        0x00,0x00,0x00,0x00,0x00,0x1e
    };
    memcpy(d,timing,sizeof timing);
}
int main(void)
{
    uint8_t e[256];
    base(e);
    assert(radar_edid_720p60(e,128U)==0);
    assert(radar_edid_720p60(e,127U)==-1);
    assert(radar_edid_720p60(e,256U)==-1);
    e[54]=0x01; e[55]=0x1d; /* 74.25MHz */
    e[56]=0x00; e[57]=0x72; e[58]=0x51; /* active1280 + blank370 */
    e[59]=0xd0; e[60]=0x1e; e[61]=0x20; /* active720 + blank30 */
    checksum(e);
    assert(radar_edid_720p60(e,128U)==1);
    e[71]=0x80U; checksum(e); /* Interlaced is not our mode. */
    assert(radar_edid_720p60(e,128U)==0);
    base(e); e[126]=1; checksum(e);
    e[128]=2U; e[129]=3U; e[130]=6U; e[132]=0x41U; e[133]=0x84U;
    checksum(e+128);
    assert(radar_edid_720p60(e,256U)==1); /* CEA native VIC4 */
    e[133]=16U; checksum(e+128);
    assert(radar_edid_720p60(e,256U)==0); /* 1080p is not 720p */
    e[133]=4U;
    assert(radar_edid_720p60(e,256U)==-1); /* corrupted checksum */
    e[132]=0x43U; checksum(e+128);
    assert(radar_edid_720p60(e,256U)==-1); /* data block overrun */
    e[130]=3U; checksum(e+128);
    assert(radar_edid_720p60(e,256U)==-1); /* malformed CEA offset */
    e[0]=1U; checksum(e);
    assert(radar_edid_720p60(e,256U)==-1); /* bad header */
    base(e); e[126]=1; checksum(e);
    e[128]=2U; e[129]=3U; e[130]=6U; e[132]=0x41U; e[133]=4U;
    checksum(e+128);
    assert(!radar_edid_custom_720_range(e,256U,74999250U));
    e[57]=0xfdU; e[59]=45U; e[60]=75U; e[61]=30U; e[62]=90U; e[63]=15U;
    checksum(e);
    assert(radar_edid_custom_720_range(e,256U,74999250U));
    e[59]=60U; checksum(e);
    assert(!radar_edid_custom_720_range(e,256U,74999250U)); /* Below60Hz */
    e[59]=45U; e[63]=7U; checksum(e);
    assert(!radar_edid_custom_720_range(e,256U,74999250U)); /* Above70MHz */
    e[63]=15U; e[61]=45U; checksum(e);
    assert(!radar_edid_custom_720_range(e,256U,74999250U)); /* Below45kHz */
    e[61]=30U; e[58]=1U; checksum(e);
    assert(!radar_edid_custom_720_range(e,256U,74999250U)); /* Unsupported range encoding */
    base(e);
    assert(radar_edid_800x600_60(e,128U)==0);
    assert(radar_edid_800x600_60(e,127U)==-1);
    e[35]=1U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==1); /* Established SVGA60 */
    e[35]=2U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* SVGA56 is a different mode */
    e[35]=0U; e[38]=0x45U; e[39]=0x40U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==1); /* Standard timing800x600 nominal60 */
    e[39]=0x4bU; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* nominal71Hz */
    e[39]=0xc0U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* Same width, wrong aspect */
    base(e); svga_dtd(e+54U); checksum(e);
    assert(radar_edid_800x600_60(e,128U)==1);
    e[54]=0x90U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* Wrong pixel clock */
    svga_dtd(e+54U); e[57]=1U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* Wrong Htotal */
    svga_dtd(e+54U); e[60]=0x1dU; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* Wrong Vtotal */
    svga_dtd(e+54U); e[71]|=0x80U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* Interlaced */
    svga_dtd(e+54U); e[62]=41U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==0); /* Nonstandard porch */
    base(e); e[126]=1U; checksum(e);
    e[128]=2U; e[129]=3U; e[130]=4U; svga_dtd(e+132U); checksum(e+128U);
    assert(radar_edid_800x600_60(e,256U)==1); /* CEA detailed timing */
    e[150]=1U;
    assert(radar_edid_800x600_60(e,256U)==-1); /* Corrupted extension */
    base(e); e[57]=0xfaU; e[59]=0x45U; e[60]=0x40U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==1); /* Additional standard timings */
    e[0]=1U; checksum(e);
    assert(radar_edid_800x600_60(e,128U)==-1); /* Corrupted base header */
    base(e);
    assert(radar_edid_1080p60(e,128U)==0);
    assert(radar_edid_1080p60(NULL,128U)==-1);
    assert(radar_edid_1080p60(e,127U)==-1);
    assert(radar_edid_1080p60(e,256U)==-1); /* Unadvertised trailing block */
    full_hd_dtd(e+54U); checksum(e);
    assert(radar_edid_1080p60(e,128U)==1);
    e[54]=0xf3U; e[55]=0x39U; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* 59.94-only DTD, not exact60 */
    full_hd_dtd(e+54U); e[57]=0x19U; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* Wrong Htotal */
    full_hd_dtd(e+54U); e[60]=0x2eU; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* Wrong Vtotal */
    full_hd_dtd(e+54U); e[62]=89U; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* Wrong H porch */
    full_hd_dtd(e+54U); e[64]=0x55U; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* Wrong V porch */
    full_hd_dtd(e+54U); e[71]|=0x80U; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* Interlaced DTD */
    full_hd_dtd(e+54U); e[71]=0x1cU; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* Wrong polarity */
    base(e); e[126]=1U; checksum(e);
    e[128]=2U; e[129]=3U; e[130]=6U; e[132]=0x41U; e[133]=0x90U;
    checksum(e+128U);
    assert(radar_edid_1080p60(e,256U)==1); /* Native VIC16 */
    e[133]=16U; checksum(e+128U);
    assert(radar_edid_1080p60(e,256U)==1); /* Non-native VIC16 */
    e[133]=5U; checksum(e+128U);
    assert(radar_edid_1080p60(e,256U)==0); /* 1080i60 VIC5 */
    e[133]=31U; checksum(e+128U);
    assert(radar_edid_1080p60(e,256U)==0); /* 1080p50 VIC31 */
    e[133]=16U; e[132]=0x43U; checksum(e+128U);
    assert(radar_edid_1080p60(e,256U)==-1); /* Overrun despite valid VIC */
    e[132]=0x41U; e[130]=3U; checksum(e+128U);
    assert(radar_edid_1080p60(e,256U)==-1); /* CTA offset below header */
    e[130]=4U; full_hd_dtd(e+132U); checksum(e+128U);
    assert(radar_edid_1080p60(e,256U)==1); /* CTA DTD */
    e[150]=1U;
    assert(radar_edid_1080p60(e,256U)==-1); /* Checksum invalid */
    checksum(e+128U); e[0]=1U; checksum(e);
    assert(radar_edid_1080p60(e,256U)==-1); /* Bad header */
    base(e); e[38]=209U; e[39]=0xc0U; checksum(e);
    assert(radar_edid_1080p60(e,128U)==0); /* Unspecified 1080 blanking */
    puts("PASS display EDID: 56 capability/corruption/bounds/custom-range/SVGA/1080p checks; host fixtures only");
    return 0;
}
