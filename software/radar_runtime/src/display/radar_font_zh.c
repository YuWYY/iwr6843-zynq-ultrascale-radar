/* SPDX-License-Identifier: MIT
 * Renderer for the separately licensed Radar UI Bitmap Subset (OFL-1.1).
 * Font pixels use 4-bit coverage, high nibble first, no row padding.
 * Framebuffer format is 0xAABBGGRR. Origins are top-of-CJK-body coordinates.
 */
#include "radar_font_zh.h"
#include <limits.h>
#include <stddef.h>

typedef struct {
    uint32_t codepoint, offset;
    uint16_t length;
    uint8_t px, width, height, advance;
    int8_t left, top;
} RadarFontGlyph;
#include "radar_font_zh_data.h"

static uint32_t next_codepoint(const char **cursor)
{
    const unsigned char *p=(const unsigned char *)*cursor;
    uint32_t cp; unsigned n,i;
    if(p[0]<0x80U){*cursor+=p[0]!=0U;return p[0];}
    if(p[0]>=0xc2U&&p[0]<=0xdfU){cp=p[0]&0x1fU;n=2U;}
    else if(p[0]>=0xe0U&&p[0]<=0xefU){cp=p[0]&0x0fU;n=3U;}
    else if(p[0]>=0xf0U&&p[0]<=0xf4U){cp=p[0]&0x07U;n=4U;}
    else {++*cursor;return (uint32_t)'?';}
    for(i=1U;i<n;++i){
        /* Reading stops at the first zero, including a truncated sequence. */
        if((p[i]&0xc0U)!=0x80U){++*cursor;return (uint32_t)'?';}
        cp=(cp<<6)|(p[i]&0x3fU);
    }
    if((n==3U&&cp<0x800U)||(n==4U&&cp<0x10000U)||
       cp>0x10ffffU||(cp>=0xd800U&&cp<=0xdfffU)){
        ++*cursor;return (uint32_t)'?';
    }
    *cursor+=(int)n;return cp;
}
static const RadarFontGlyph *find_glyph(unsigned px,uint32_t cp)
{
    unsigned lo=0U,hi=(unsigned)(sizeof radar_font_glyphs/sizeof radar_font_glyphs[0]);
    while(lo<hi){
        unsigned m=lo+(hi-lo)/2U;const RadarFontGlyph *g=&radar_font_glyphs[m];
        if(g->px<px||(g->px==px&&g->codepoint<cp))lo=m+1U;
        else hi=m;
    }
    if(lo<sizeof radar_font_glyphs/sizeof radar_font_glyphs[0]&&
       radar_font_glyphs[lo].px==px&&radar_font_glyphs[lo].codepoint==cp)return &radar_font_glyphs[lo];
    return NULL;
}
int radar_font_zh_has_utf8(const char *s)
{
    if(s)while(*s)if((unsigned char)*s++>=0x80U)return 1;
    return 0;
}
int radar_font_zh_has_glyph(unsigned px,uint32_t cp){return find_glyph(px,cp)!=NULL;}
static const RadarFontGlyph *visible_glyph(unsigned px,uint32_t cp)
{
    const RadarFontGlyph *g=find_glyph(px,cp);
    return g?g:find_glyph(px,(uint32_t)'?');
}
int radar_font_zh_measure(unsigned px,const char *s)
{
    int total=0;
    if(s)while(*s){
        const RadarFontGlyph *g=visible_glyph(px,next_codepoint(&s));
        if(!g)break;
        if(total>INT_MAX-(int)g->advance)return INT_MAX;
        total+=(int)g->advance;
    }
    return total;
}
static uint32_t blend(uint32_t bg,uint32_t fg,unsigned a)
{
    unsigned inv=15U-a;
    unsigned r=((fg&255U)*a+(bg&255U)*inv+7U)/15U;
    unsigned g=(((fg>>8)&255U)*a+((bg>>8)&255U)*inv+7U)/15U;
    unsigned b=(((fg>>16)&255U)*a+((bg>>16)&255U)*inv+7U)/15U;
    return 0xff000000U|(b<<16)|(g<<8)|r;
}
int radar_font_zh_draw(uint32_t *pixels,unsigned stride,unsigned width,
                      unsigned height,int x,int y,unsigned px,uint32_t color,
                      const char *s,int max_width)
{
    int advance=0;
    if(!pixels||!s||!width||!height||stride<width||max_width<=0||
       width>(unsigned)INT_MAX||height>(unsigned)INT_MAX)return 0;
    while(*s){
        const RadarFontGlyph *g=visible_glyph(px,next_codepoint(&s));
        unsigned row,col;
        if(!g||(int)g->advance>max_width-advance)break;
        for(row=0U;row<g->height;++row)for(col=0U;col<g->width;++col){
            unsigned k=row*g->width+col;
            unsigned a=(radar_font_coverage[g->offset+k/2U]>>(k%2U?0U:4U))&15U;
            int64_t xx=(int64_t)x+advance+g->left+(int)col;
            int64_t yy=(int64_t)y+g->top+(int)row;
            if(a&&xx>=0&&yy>=0&&xx<(int64_t)width&&yy<(int64_t)height&&
               xx>=(int64_t)x&&xx<(int64_t)x+max_width){
                size_t index=(size_t)yy*stride+(size_t)xx;
                pixels[index]=a==15U?color:blend(pixels[index],color,a);
            }
        }
        advance+=(int)g->advance;
    }
    return advance;
}
