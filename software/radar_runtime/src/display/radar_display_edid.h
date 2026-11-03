/* SPDX-License-Identifier: MIT
 * Narrow EDID gates for CEA 720p60/1080p60 and standard SVGA60 diagnostics, not a
 * general EDID parser. Never infer a mode just from the monitor range.
 * Unrecognized capabilities never cause a silent timing substitution. */
#ifndef RADAR_DISPLAY_EDID_H
#define RADAR_DISPLAY_EDID_H
#include <stdint.h>
#include <stddef.h>
static int radar_edid_checksum(const uint8_t *p)
{
    unsigned i, sum = 0U;
    for (i = 0U; i < 128U; ++i) { sum += p[i]; }
    return (sum & 255U) == 0U;
}
static int radar_edid_dtd_720(const uint8_t *d)
{
    unsigned clock = ((unsigned)d[0] | ((unsigned)d[1] << 8)) * 10000U;
    unsigned h = d[2] | ((unsigned)(d[4] & 0xf0U) << 4);
    unsigned hb = d[3] | ((unsigned)(d[4] & 0x0fU) << 8);
    unsigned v = d[5] | ((unsigned)(d[7] & 0xf0U) << 4);
    unsigned vb = d[6] | ((unsigned)(d[7] & 0x0fU) << 8);
    return h == 1280U && v == 720U && hb == 370U && vb == 30U &&
           clock >= 74170000U && clock <= 74250000U && (d[17] & 0x80U) == 0U;
}
/* 1=supported, 0=not advertised in supplied blocks, -1=malformed/truncated. */
static int radar_edid_720p60(const uint8_t *p, size_t bytes)
{
    static const uint8_t magic[8] = {0,255,255,255,255,255,255,0};
    size_t b, i, available;
    int supported = 0;
    if (bytes < 128U || (bytes % 128U) != 0U) { return -1; }
    for (i = 0U; i < 8U; ++i) { if (p[i] != magic[i]) { return -1; } }
    available = bytes / 128U;
    if (available > (size_t)p[126] + 1U) { return -1; }
    for (b = 0U; b < available; ++b) {
        const uint8_t *e = p + b * 128U;
        if (!radar_edid_checksum(e)) { return -1; }
        if (b == 0U) {
            for (i = 54U; i + 18U <= 126U; i += 18U) {
                if (radar_edid_dtd_720(e + i)) { supported = 1; }
            }
        } else if (e[0] == 2U) {
            size_t end = e[2];
            if (end == 0U) { continue; }
            if (end < 4U || end > 127U) { return -1; }
            for (i = 4U; i < end;) {
                unsigned len = e[i] & 31U, tag = e[i] >> 5;
                size_t j;
                if (i + 1U + len > end) { return -1; }
                if (tag == 2U) {
                    for (j = 1U; j <= len; ++j) {
                        if ((e[i + j] & 127U) == 4U) { supported = 1; }
                    }
                }
                i += 1U + len;
            }
            for (i = end; i + 18U <= 127U; i += 18U) {
                if (radar_edid_dtd_720(e + i)) { supported = 1; }
            }
        }
    }
    return supported;
}
static int radar_edid_dtd_1080p60(const uint8_t *d)
{
    unsigned clock=((unsigned)d[0]|((unsigned)d[1]<<8))*10000U;
    unsigned h=d[2]|((unsigned)(d[4]&0xf0U)<<4);
    unsigned hb=d[3]|((unsigned)(d[4]&0x0fU)<<8);
    unsigned v=d[5]|((unsigned)(d[7]&0xf0U)<<4);
    unsigned vb=d[6]|((unsigned)(d[7]&0x0fU)<<8);
    unsigned hf=d[8]|((unsigned)(d[11]&0xc0U)<<2);
    unsigned hs=d[9]|((unsigned)(d[11]&0x30U)<<4);
    unsigned vf=(d[10]>>4)|((unsigned)(d[11]&0x0cU)<<2);
    unsigned vs=(d[10]&0x0fU)|((unsigned)(d[11]&3U)<<4);
    /* CEA 1920x1080p60: 2200x1125 total, +H/+V separate sync. */
    return clock==148500000U && h==1920U && hb==280U && v==1080U && vb==45U &&
           hf==88U && hs==44U && vf==4U && vs==5U && d[17]==0x1eU;
}
/* 1=advertised, 0=not advertised, -1=malformed supplied EDID blocks.
 * Require VIC16 or the exact progressive CEA DTD. A generic 1920x1080
 * standard timing, 1080i VIC, or range descriptor does not identify this
 * pixel clock/blanking. Only supplied, checksum-checked blocks are used. */
static int radar_edid_1080p60(const uint8_t *p,size_t bytes)
{
    size_t b,i;
    int supported=0;
    if (p==NULL || radar_edid_720p60(p,bytes)<0) { return -1; }
    for (b=0U;b<bytes/128U;++b) {
        const uint8_t *e=p+b*128U;
        size_t first=54U,limit=126U;
        if (b!=0U) {
            if (e[0]!=2U || e[2]==0U) { continue; }
            first=e[2]; limit=127U;
            /* Common validation above has checked the CTA block bounds. */
            for (i=4U;i<first;) {
                unsigned len=e[i]&31U,tag=e[i]>>5;
                size_t j;
                if (tag==2U) {
                    for (j=1U;j<=len;++j) {
                        if ((e[i+j]&127U)==16U) { supported=1; }
                    }
                }
                i+=1U+len;
            }
        }
        for (i=first;i+18U<=limit;i+=18U) {
            if (radar_edid_dtd_1080p60(e+i)) { supported=1; }
        }
    }
    return supported;
}
static int radar_edid_dtd_800x600_60(const uint8_t *d)
{
    unsigned clock=((unsigned)d[0]|((unsigned)d[1]<<8))*10000U;
    unsigned h=d[2]|((unsigned)(d[4]&0xf0U)<<4);
    unsigned hb=d[3]|((unsigned)(d[4]&0x0fU)<<8);
    unsigned v=d[5]|((unsigned)(d[7]&0xf0U)<<4);
    unsigned vb=d[6]|((unsigned)(d[7]&0x0fU)<<8);
    unsigned hf=d[8]|((unsigned)(d[11]&0xc0U)<<2);
    unsigned hs=d[9]|((unsigned)(d[11]&0x30U)<<4);
    unsigned vf=(d[10]>>4)|((unsigned)(d[11]&0x0cU)<<2);
    unsigned vs=(d[10]&0x0fU)|((unsigned)(d[11]&3U)<<4);
    return clock==40000000U && h==800U && hb==256U && v==600U && vb==28U &&
           hf==40U && hs==128U && vf==1U && vs==4U && d[17]==0x1eU;
}
/* VESA DMT 800x600 nominal 60Hz (40MHz / 1056 / 628, about 60.317Hz).
 * 1=advertised, 0=not advertised, -1=malformed supplied EDID blocks.
 * Accept established timing bit35.0, a 4:3 standard timing at nominal60,
 * or a complete matching progressive DTD. Range limits alone are not proof. */
static int radar_edid_800x600_60(const uint8_t *p,size_t bytes)
{
    size_t b,i;
    int supported=0;
    /* Reuse the common header/checksum/CEA bounds checks without requiring
     * the independent 720p mode to be present. */
    if (radar_edid_720p60(p,bytes)<0) { return -1; }
    if ((p[35]&1U)!=0U) { supported=1; }
    for (i=38U;i<54U;i+=2U) {
        /* (0x45+31)*8=800; aspect4:3; refresh60+0. */
        if (p[i]==0x45U && p[i+1U]==0x40U) { supported=1; }
    }
    for (b=0U;b<bytes/128U;++b) {
        const uint8_t *e=p+b*128U;
        size_t first=(b==0U) ? 54U : ((e[0]==2U) ? e[2] : 0U);
        size_t limit=(b==0U) ? 126U : 127U;
        if (first==0U) { continue; }
        for (i=first;i+18U<=limit;i+=18U) {
            if (radar_edid_dtd_800x600_60(e+i)) { supported=1; }
            /* EDID additional standard-timing descriptor, six pairs. */
            if (b==0U && e[i]==0U && e[i+1U]==0U && e[i+2U]==0U &&
                e[i+3U]==0xfaU && e[i+4U]==0U) {
                size_t j;
                for (j=5U;j<17U;j+=2U) {
                    if (e[i+j]==0x45U && e[i+j+1U]==0x40U) { supported=1; }
                }
            }
        }
    }
    return supported;
}
/* The custom timing also needs a base EDID monitor range descriptor that
 * explicitly covers its rates. Unsupported extended range encoding fails
 * closed; a VIC4 declaration alone does not approve custom timing. */
static int radar_edid_custom_720_range(const uint8_t *p, size_t bytes, uint32_t pixel_hz)
{
    size_t i;
    uint64_t pixel=pixel_hz, total=(uint64_t)1667U*750U;
    if (radar_edid_720p60(p,bytes)!=1) { return 0; }
    for (i=54U;i+18U<=126U;i+=18U) {
        const uint8_t *d=p+i;
        if (d[0]!=0U || d[1]!=0U || d[2]!=0U || d[3]!=0xfdU || d[4]!=0U) { continue; }
        if (d[5]==0U || d[7]==0U || d[9]==0U || d[5]>d[6] || d[7]>d[8]) { continue; }
        if (pixel>=(uint64_t)d[5]*total && pixel<=(uint64_t)d[6]*total &&
            pixel>=(uint64_t)d[7]*1000U*1667U && pixel<=(uint64_t)d[8]*1000U*1667U &&
            pixel<=(uint64_t)d[9]*10000000U) { return 1; }
    }
    return 0;
}
#endif
