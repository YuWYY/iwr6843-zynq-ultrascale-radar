/* SPDX-License-Identifier: MIT
 * 1080p radar instrument surface. Pixels are little-endian RGBA8888.
 * All primitives clip to the fixed canvas; no allocation, IO or DMA occurs.
 * Original 5x7 glyphs reuse the project's existing display-test alphabet.
 */
#include "radar_scope_view.h"
#include "radar_range_quality.h"
#include "radar_font_zh.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define RGB(r,g,b) (0xff000000U | ((uint32_t)(b)<<16) | ((uint32_t)(g)<<8) | (r))
static const uint32_t canvas=RGB(11,16,23),surface=RGB(19,27,37);
static const uint32_t plot_bg=RGB(9,15,22),grid=RGB(40,54,68),border=RGB(47,64,80);
static const uint32_t primary=RGB(232,240,246),secondary=RGB(160,181,198);
static const uint32_t accent=RGB(70,216,222),warning=RGB(246,190,79),danger=RGB(255,115,117);
#define DB_LOW (-100.0f)
#define DB_HIGH (-20.0f)
#define RANGE_MAX_M 9.0f

typedef struct {uint32_t *p;unsigned stride;} Paint;
static const uint8_t digits[10][5]={
 {62,81,73,69,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
 {24,20,18,127,16},{47,73,73,73,49},{62,73,73,73,50},{1,113,9,5,3},
 {54,73,73,73,54},{38,73,73,73,62}};
static const uint8_t letters[26][5]={
 {126,9,9,9,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {3,4,120,4,3},{97,81,73,69,67}};
static void rect(Paint *p,int x,int y,int w,int h,uint32_t color)
{
    int x1=x+w,y1=y+h,xx,yy;
    if(x<0)x=0;
    if(y<0)y=0;
    if(x1>(int)RADAR_SCOPE_WIDTH)x1=RADAR_SCOPE_WIDTH;
    if(y1>(int)RADAR_SCOPE_HEIGHT)y1=RADAR_SCOPE_HEIGHT;
    for(yy=y;yy<y1;++yy)for(xx=x;xx<x1;++xx)p->p[(unsigned)yy*p->stride+(unsigned)xx]=color;
}
static void ascii_text(Paint *p,int x,int y,unsigned size,uint32_t color,const char *s)
{
    if(!s)return;
    while(*s && x<(int)RADAR_SCOPE_WIDTH){
        unsigned a,b;uint8_t col[5]={0,0,0,0,0};char c=*s++;
        if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
        if(c>='0'&&c<='9')memcpy(col,digits[c-'0'],5);
        else if(c>='A'&&c<='Z')memcpy(col,letters[c-'A'],5);
        else if(c=='-')memset(col,8,5);
        else if(c=='.')col[2]=64;
        else if(c==':')col[2]=36;
        else if(c=='/') {col[0]=64;col[1]=32;col[2]=16;col[3]=8;col[4]=4;}
        else if(c=='(') {col[2]=28;col[3]=34;col[4]=65;}
        else if(c==')') {col[0]=65;col[1]=34;col[2]=28;}
        else if(c=='+') {memset(col,8,5);col[2]=62;}
        else if(c=='=')memset(col,20,5);
        else if(c=='%') {col[0]=99;col[1]=19;col[2]=8;col[3]=100;col[4]=99;}
        else if(c=='_')memset(col,64,5);
        else if(c=='?'){col[0]=2;col[1]=1;col[2]=81;col[3]=9;col[4]=6;}
        for(a=0;a<5;++a)for(b=0;b<7;++b)if(col[a]&(1U<<b))rect(p,x+(int)(a*size),y+(int)(b*size),(int)size,(int)size,color);
        x+=(int)(size*6U);
    }
}

/* Chinese text is a prebuilt local subset, never a runtime font loader.
 * Numeric-only labels retain the original 5x7 renderer and scale. */
static unsigned font_pixels(unsigned size)
{
    static const unsigned px[]={13U,13U,18U,26U,32U,32U,32U};
    return px[size<7U?size:6U];
}
static int text_width(unsigned size,const char *s)
{
    if(!s)return 0;
    return radar_font_zh_has_utf8(s)?radar_font_zh_measure(font_pixels(size),s):(int)(strlen(s)*6U*size);
}
static void text(Paint *p,int x,int y,unsigned size,uint32_t color,const char *s)
{
    if(!s)return;
    if(radar_font_zh_has_utf8(s)){
        int width=(x>=1512?1848:1872)-x;
        radar_font_zh_draw(p->p,p->stride,RADAR_SCOPE_WIDTH,RADAR_SCOPE_HEIGHT,
                           x,y,font_pixels(size),color,s,width);
    }else ascii_text(p,x,y,size,color,s);
}

static int iabs(int x){return x<0?-x:x;}
static void line(Paint *p,int x0,int y0,int x1,int y1,uint32_t color)
{
    int dx=iabs(x1-x0),sx=x0<x1?1:-1,dy=-iabs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
    for(;;){int e2;rect(p,x0,y0,2,2,color);if(x0==x1&&y0==y1)break;e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}
}
static float clamp(float v,float low,float high){return v<low?low:v>high?high:v;}
static void label(Paint *p,int x,int y,unsigned size,uint32_t color,const char *s,unsigned max_chars)
{
    char b[96];
    if(!s)s="--";
    /* Do not truncate UTF-8 by byte count. The text painter clips by pixels. */
    if(radar_font_zh_has_utf8(s)){text(p,x,y,size,color,s);return;}
    if(max_chars>sizeof b-1U)max_chars=sizeof b-1U;
    (void)snprintf(b,sizeof b,"%.*s",(int)max_chars,s);
    if(max_chars>=3U && strlen(s)>max_chars){b[max_chars-3U]='.';b[max_chars-2U]='.';b[max_chars-1U]='.';}
    text(p,x,y,size,color,b);
}
static uint32_t heat(float db)
{
    /* Fixed, sequential luminance; never per-frame auto gain. */
    static const unsigned stops[5][3]={{9,20,34},{23,66,91},{31,125,141},{83,187,168},{244,232,138}};
    float q=clamp((db-DB_LOW)/(DB_HIGH-DB_LOW),0.0f,1.0f)*4.0f;
    unsigned i=(unsigned)q;float f;
    if(i>=4U)return RGB(244,232,138);
    f=q-(float)i;
    return RGB((unsigned)((float)stops[i][0]+((float)stops[i+1][0]-(float)stops[i][0])*f),
               (unsigned)((float)stops[i][1]+((float)stops[i+1][1]-(float)stops[i][1])*f),
               (unsigned)((float)stops[i][2]+((float)stops[i+1][2]-(float)stops[i][2])*f));
}
static void panel(Paint *p,int x,int y,int w,int h)
{
    rect(p,x,y,w,h,surface);rect(p,x,y,w,1,border);
}
static void number_text(Paint *p,int x,int y,unsigned size,uint32_t color,float value,unsigned decimals,const char *unit)
{
    char out[48];int scale,n;
    if(!isfinite(value)){text(p,x,y,size,secondary,"--");return;}
    scale=decimals==2U?100:decimals==1U?10:1;
    /* All visible quantities are bounded below this limit; do not convert
     * arbitrary floats into an undefined out-of-range integer. */
    if(fabsf(value)>(float)(100000000/scale)){text(p,x,y,size,secondary,"超出范围");return;}
    n=(int)(fabsf(value)*(float)scale+0.5f);
    if(decimals==2U)(void)snprintf(out,sizeof out,"%s%d.%02d%s",value<0.0f?"-":"",n/100,n%100,unit);
    else if(decimals==1U)(void)snprintf(out,sizeof out,"%s%d.%d%s",value<0.0f?"-":"",n/10,n%10,unit);
    else (void)snprintf(out,sizeof out,"%s%d%s",value<0.0f?"-":"",n,unit);
    text(p,x,y,size,color,out);
}
static float history_span(const RadarRangeFrame *h,unsigned count)
{
    unsigned i;for(i=0U;i<count;++i)if(h[i].time_untrusted)return 0.0f;
    if(count<2U||h[count-1U].timestamp_us<h[0].timestamp_us)return 0.0f;
    return (float)((double)(h[count-1U].timestamp_us-h[0].timestamp_us)/1000000.0);
}
static void capture_rate(Paint *p,int x,int y,unsigned size,const RadarRangeFrame *h,unsigned count)
{
    float span=history_span(h,count);
    if(count>1U&&span>0.0f)number_text(p,x,y,size,primary,(float)(count-1U)/span,2," HZ");
    else text(p,x,y,size,secondary,"-- HZ");
}
static void color_scale(Paint *p,int x,int y,int w)
{
    int i;for(i=0;i<w;++i)rect(p,x+i,y,1,16,heat(DB_LOW+(DB_HIGH-DB_LOW)*(float)i/(float)(w-1)));
    text(p,x,y+28,2,secondary,"-100");text(p,x+w-96,y+28,2,secondary,"-20 DBFS");
}
static void spectrum(Paint *p,int x,int y,int w,int h,const RadarRangeFrame *f,RadarRangeQuality quality)
{
    unsigned i;char b[24];
    rect(p,x,y,w,h,plot_bg);
    for(i=0;i<=4U;++i){int yy=y+(int)i*h/4;rect(p,x,yy,w,1,grid);(void)snprintf(b,sizeof b,"-%u",20U+i*20U);text(p,x-62,yy-6,2,secondary,b);}
    for(i=0;i<=9U;++i){int xx=x+(int)i*w/9;rect(p,xx,y,1,h,grid);(void)snprintf(b,sizeof b,"%u",i);text(p,xx-5,y+h+20,2,secondary,b);}
    text(p,x-62,y-30,2,secondary,"DBFS");text(p,x+w-108,y+h+50,2,secondary,"距离 / m");
    if(f&&f->valid&&f->bin_m>0.0f&&isfinite(f->bin_m)){
        int px=0,py=0,have=0;
        for(i=0;i<RADAR_RANGE_BINS;++i){float r=(float)i*f->bin_m,db=f->power_dbfs[i];int xx,yy;
            if(r>RANGE_MAX_M)break;
            if(!isfinite(db)){have=0;continue;}
            xx=x+(int)(r*(float)w/RANGE_MAX_M);yy=y+h-(int)((clamp(db,DB_LOW,DB_HIGH)-DB_LOW)*(float)h/(DB_HIGH-DB_LOW));
            if(have)line(p,px,py,xx,yy,accent);
            px=xx;py=yy;have=1;
        }
        if(quality==RADAR_RANGE_QUALITY_QUALIFIED&&f->peak_bin<RADAR_RANGE_BINS&&isfinite(f->peak_dbfs)){
            float r=(float)f->peak_bin*f->bin_m;
            if(r<=RANGE_MAX_M){int xx=x+(int)(r*(float)w/RANGE_MAX_M),yy=y+h-(int)((clamp(f->peak_dbfs,DB_LOW,DB_HIGH)-DB_LOW)*(float)h/(DB_HIGH-DB_LOW));rect(p,xx-4,yy-4,9,9,warning);}
        }
    }else text(p,x+w/2-252,y+h/2-10,3,secondary,"等待有效帧");
}
static void waterfall(Paint *p,int x,int y,int w,int h,const RadarRangeFrame *history,unsigned count)
{
    unsigned i;char b[32];
    rect(p,x,y,w,h,plot_bg);
    for(i=0;i<=3U;++i){int yy=y+h-(int)i*h/3;rect(p,x,yy,w,1,grid);(void)snprintf(b,sizeof b,"%u",i*3U);text(p,x-36,yy-6,2,secondary,b);}
    text(p,x-62,y-30,2,secondary,"距离 / m");
    if(count){
        unsigned first=0U,last=count;
        while(first<count&&history[first].time_untrusted)++first;
        while(last>first&&history[last-1U].time_untrusted)--last;
        if(first==last) {
            text(p,x+w/2-252,y+h/2-10,3,secondary,"采样时间不可用");
            text(p,x+w-432,y+h+50,2,secondary,"距首个可见采集帧的时间");return;
        }
        uint64_t t0=history[first].timestamp_us,t1=history[last-1U].timestamp_us;
        double span= t1>=t0?(double)(t1-t0)/1000000.0:0.0;
        double draw_span=span>0.0?span:1.0;int previous_x=-100;
        for(i=0;i<count;++i){unsigned k;int xx;const RadarRangeFrame *f=&history[i];
            if(!f->valid||f->time_untrusted||f->timestamp_us<t0||f->timestamp_us>t1||!isfinite(f->bin_m)||f->bin_m<=0.0f)continue;
            xx=x+(int)(((double)(f->timestamp_us-t0)/1000000.0)/draw_span*(double)(w-8));
            if(xx<previous_x)continue;
            previous_x=xx;
            /* Each measured capture occupies at most eight pixels. Intervals
             * between captures remain empty, including intentional pauses. */
            for(k=0;k<RADAR_RANGE_BINS;++k){float r0=(float)k*f->bin_m,r1=(float)(k+1U)*f->bin_m;int y0,y1;
                if(r0>=RANGE_MAX_M)break;
                if(!isfinite(f->power_dbfs[k]))continue;
                if(r1>RANGE_MAX_M)r1=RANGE_MAX_M;
                y0=y+h-(int)(r1*(float)h/RANGE_MAX_M);y1=y+h-(int)(r0*(float)h/RANGE_MAX_M);
                rect(p,xx,y0,8,y1-y0>0?y1-y0:1,heat(f->power_dbfs[k]));
            }
        }
        for(i=0;i<=(count>1U?4U:0U);++i){int xx=x+(int)i*(w-8)/4;rect(p,xx,y+h,1,7,border);number_text(p,xx-(i==4U?60:10),y+h+20,2,secondary,(float)(draw_span*(double)i/4.0),1," S");}
        if(count==1U)text(p,x+w/2-252,y+h/2-10,3,secondary,"首帧到达 / 开始记录历史");
    }else text(p,x+w/2-288,y+h/2-10,3,secondary,"历史随新采集帧填充");
    text(p,x+w-432,y+h+50,2,secondary,"距首个可见采集帧的时间");
}
static const char *range_check_label(RadarRangeQuality quality)
{
    switch(quality){
    case RADAR_RANGE_QUALITY_QUALIFIED:return "峰值检查通过";
    case RADAR_RANGE_QUALITY_RANGE_EDGE:return "距离无法确认";
    case RADAR_RANGE_QUALITY_CLIPPED:return "ADC 削顶";
    case RADAR_RANGE_QUALITY_DETECTOR_UNAVAILABLE:return "检查不可用";
    case RADAR_RANGE_QUALITY_UNCONFIRMED:return "峰值未确认";
    default:return "等待数据";
    }
}
static void peak_block(Paint *p,int x,int y,const RadarRangeFrame *latest,RadarRangeQuality quality)
{
    text(p,x,y,2,secondary,"最强回波");
    if(quality==RADAR_RANGE_QUALITY_QUALIFIED)
        number_text(p,x,y+40,6,primary,(float)latest->peak_bin*latest->bin_m,2," M");
    else text(p,x,y+40,6,quality==RADAR_RANGE_QUALITY_NO_DATA?secondary:warning,"-- M");
    /* Keep the measured spectrum and peak power as diagnostics. A failed
     * distance check never substitutes another bin or fabricates a distance. */
    if(latest&&latest->valid&&isfinite(latest->peak_dbfs))number_text(p,x,y+106,3,accent,latest->peak_dbfs,1," DBFS");
    else text(p,x,y+106,3,secondary,"暂无帧");
}
static void overview(Paint *p,const RadarRangeFrame *h,unsigned count,const RadarSpatialFrame *spatial)
{
    const RadarRangeFrame *f=count?&h[count-1U]:NULL;char b[64];
    RadarRangeQuality quality=radar_range_assess(f,spatial);
    panel(p,48,204,1440,356);panel(p,48,584,1440,396);panel(p,1512,204,360,776);
    text(p,80,232,3,primary,"距离波形图");text(p,890,236,2,secondary,"当前帧 / 平均功率");
    spectrum(p,144,296,1280,190,f,quality);
    text(p,80,612,3,primary,"距离随时间变化");text(p,950,616,2,secondary,"实测采集记录");
    waterfall(p,144,682,1280,208,h,count);
    peak_block(p,1540,236,f,quality);
    text(p,1540,396,2,quality==RADAR_RANGE_QUALITY_QUALIFIED?accent:warning,range_check_label(quality));
    text(p,1540,424,2,secondary,"不代表目标识别");
    rect(p,1540,464,304,1,border);text(p,1540,488,2,secondary,"采集率");capture_rate(p,1540,524,4,h,count);
    (void)snprintf(b,sizeof b,"历史 %u / %u",count,RADAR_SCOPE_HISTORY_MAX);text(p,1540,584,2,secondary,b);
    if(f){(void)snprintf(b,sizeof b,"采集编号 %u",f->session);text(p,1540,620,2,secondary,b);(void)snprintf(b,sizeof b,"削顶 %u",f->clipped_components);text(p,1540,656,2,f->clipped_components?warning:secondary,b);}
    else {text(p,1540,620,2,secondary,"采集编号 --");text(p,1540,656,2,secondary,"削顶 --");}
    rect(p,1540,700,304,1,border);text(p,1540,728,2,primary,"固定功率色标");color_scale(p,1540,766,304);
    text(p,1540,848,2,secondary,"256 点 FFT / 汉宁窗");text(p,1540,880,2,secondary,"距离格间隔 44.1 mm");text(p,1540,928,2,secondary,"距离尚未标定");
}
/* Spatial views consume an immutable result from the same capture session.
 * Unknown elevation never becomes an invented z=0 point. */
static unsigned point_count(const RadarSpatialFrame *s)
{
    return s && s->valid ? (s->count<RADAR_SPATIAL_MAX_POINTS?s->count:RADAR_SPATIAL_MAX_POINTS) : 0U;
}
static int xyz_valid(const RadarSpatialPoint *q)
{
    return (q->flags&RADAR_POINT_XYZ_VALID) && isfinite(q->x_m) &&
        isfinite(q->y_m) && isfinite(q->z_m) && fabsf(q->x_m)<=9.0f &&
        q->y_m>=0.0f && q->y_m<=9.0f && fabsf(q->z_m)<=9.0f &&
        q->x_m*q->x_m+q->y_m*q->y_m+q->z_m*q->z_m<=81.01f;
}
static unsigned xyz_count(const RadarSpatialFrame *s)
{
    unsigned i,n=0U;
    for(i=0;i<point_count(s);++i)if(xyz_valid(&s->points[i]))++n;
    return n;
}
static void marker(Paint *p,int x,int y,uint32_t color)
{
    rect(p,x-5,y-5,11,11,plot_bg);rect(p,x-4,y-4,9,2,color);
    rect(p,x-4,y+3,9,2,color);rect(p,x-4,y-2,2,5,color);
    rect(p,x+3,y-2,2,5,color);rect(p,x-1,y-1,3,3,primary);
}
static void spatial_empty(Paint *p,const RadarSpatialFrame *s,int x,int y,int need_xyz)
{
    const char *m=NULL;
    if(!s)m="等待匹配帧";
    else if(!s->valid)m=s->error?"空间处理错误":"空间数据不可用";
    else if(!point_count(s))m="暂无检出点";
    else if(need_xyz&&!xyz_count(s))m="高度不可用";
    if(m){int w=text_width(3U,m)+32;rect(p,x-16,y-12,w,48,plot_bg);text(p,x,y,3,s&&s->error?danger:secondary,m);}
}
static void spatial_sidebar(Paint *p,const RadarSpatialFrame *s,const RadarRangeFrame *h,
                            unsigned count,const RadarViewDiagnostics *d)
{
    char b[80];unsigned i,n=point_count(s),shown=n<3U?n:3U;
    panel(p,1512,204,360,776);
    text(p,1540,236,3,s&&s->calibrated?accent:warning,s&&s->calibrated?"已标定":"尚未标定");
    text(p,1540,278,2,secondary,s&&s->calibrated?"实测阵列":"标称阵列");
    label(p,1540,308,2,secondary,s&&s->calibrated&&s->calibration_id[0]?s->calibration_id:"需要相位标定",25U);
    if(s){(void)snprintf(b,sizeof b,"采集编号 %u",s->session);text(p,1540,350,2,primary,b);}
    else text(p,1540,350,2,secondary,"采集编号 --");
    text(p,1540,382,2,secondary,"绘制时数据年龄");
    if(d&&d->frame_age_valid&&s)number_text(p,1708,382,2,primary,(float)((double)d->frame_age_us/1000000.0),2," S");
    else text(p,1708,382,2,secondary,"--");
    text(p,1540,414,2,secondary,"采集率");capture_rate(p,1650,414,2,h,count);
    text(p,1540,446,2,secondary,"采样时刻");
    if(s&&count&&!h[count-1U].time_untrusted)number_text(p,1650,446,2,primary,(float)((double)s->timestamp_us/1000000.0),2," S");
    else text(p,1650,446,2,secondary,"--");
    rect(p,1540,484,304,1,border);
    (void)snprintf(b,sizeof b,"%u 回波点 / %u 空间点",n,xyz_count(s));text(p,1540,508,2,primary,b);
    for(i=0;i<shown;++i){const RadarSpatialPoint *q=&s->points[i];int y=552+(int)i*112;
        (void)snprintf(b,sizeof b,"%02u",i+1U);text(p,1540,y,2,accent,b);
        number_text(p,1588,y,2,primary,q->range_m,2," M");
        number_text(p,1720,y,2,secondary,q->snr_db,1," DB");
        text(p,1540,y+30,2,secondary,"投影角");
        if((q->flags&(RADAR_POINT_AZ_VALID|RADAR_POINT_BEARING_VALID))&&isfinite(q->horizontal_angle_deg))number_text(p,1636,y+30,2,primary,q->horizontal_angle_deg,1,"°");
        else text(p,1636,y+30,2,warning,"不可用");
        if(xyz_valid(q)){text(p,1540,y+60,2,secondary,"局部 Z");number_text(p,1650,y+60,2,primary,q->z_m,2," M");}
        else text(p,1540,y+60,2,warning,"高度不可用");
    }
    if(!n)text(p,1540,566,2,secondary,s&&s->valid?"暂无检出点":"暂无有效空间数据");
    rect(p,1540,906,304,1,border);
    if(s&&s->truncated_count){(void)snprintf(b,sizeof b,"舍弃 %u / 上限 %u",s->truncated_count,RADAR_SPATIAL_MAX_POINTS);label(p,1540,928,2,warning,b,25U);}
    else text(p,1540,928,2,secondary,"单帧检出结果");
    text(p,1540,956,2,secondary,"散射回波点");
}
static void azimuth_page(Paint *p,const RadarSpatialFrame *s)
{
    const int x=144,y=352,w=1280,h=492;unsigned r,a,i;char b[32];
    panel(p,48,204,1440,776);text(p,80,236,4,primary,"方位热图");
    text(p,80,286,2,secondary,"水平投影 / 标称 8 阵元阵列");
    rect(p,x,y,w,h,plot_bg);
    if(s&&s->valid)for(r=RADAR_RANGE_FIRST_BIN;r<RADAR_SPATIAL_MAP_RANGES;++r){
        float r0=(float)r*(float)RADAR_RANGE_BIN_METERS,r1=(float)(r+1U)*(float)RADAR_RANGE_BIN_METERS;int y0,y1;
        if(r0>=RANGE_MAX_M)break;
        if(r1>RANGE_MAX_M)r1=RANGE_MAX_M;
        y0=y+h-(int)(r1*(float)h/RANGE_MAX_M);y1=y+h-(int)(r0*(float)h/RANGE_MAX_M);
        for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a){float db=s->heatmap_dbfs[r][a];int x0=x+(int)a*w/(int)RADAR_SPATIAL_MAP_ANGLES,x1=x+(int)(a+1U)*w/(int)RADAR_SPATIAL_MAP_ANGLES;
            if(isfinite(db))rect(p,x0,y0,x1-x0,y1-y0,heat(db));
        }
    }
    for(i=0;i<=3U;++i){int yy=y+h-(int)i*h/3;rect(p,x,yy,w,1,grid);(void)snprintf(b,sizeof b,"%u",i*3U);text(p,x-44,yy-6,2,secondary,b);}
    for(i=0;i<=4U;++i){int xx=x+(int)i*w/4;rect(p,xx,y,1,h,grid);(void)snprintf(b,sizeof b,"%d",(int)i*30-60);text(p,xx-(i==4U?24:12),y+h+20,2,secondary,b);}
    text(p,80,320,2,secondary,"距离 / m");text(p,1052,892,2,secondary,"投影角 / °");
    rect(p,x,y+h-(int)((float)RADAR_RANGE_FIRST_BIN*(float)RADAR_RANGE_BIN_METERS*(float)h/RANGE_MAX_M),w,
         (int)((float)RADAR_RANGE_FIRST_BIN*(float)RADAR_RANGE_BIN_METERS*(float)h/RANGE_MAX_M),canvas);
    for(i=0;i<point_count(s);++i){const RadarSpatialPoint *q=&s->points[i];int xx,yy;
        if(!(q->flags&(RADAR_POINT_AZ_VALID|RADAR_POINT_BEARING_VALID))||!isfinite(q->horizontal_angle_deg)||!isfinite(q->range_m)||
           fabsf(q->horizontal_angle_deg)>60.0f||q->range_m<(float)RADAR_RANGE_FIRST_BIN*(float)RADAR_RANGE_BIN_METERS||q->range_m>9.0f)continue;
        xx=x+(int)((q->horizontal_angle_deg+60.0f)*(float)w/120.0f);yy=y+h-(int)(q->range_m*(float)h/9.0f);
        marker(p,xx,yy,(q->flags&RADAR_POINT_XYZ_VALID)?accent:warning);
    }
    spatial_empty(p,s,460,564,0);
    text(p,80,936,2,secondary,"屏蔽 0–5 距离格 / 1° 网格不代表角分辨率");
    color_scale(p,1016,924,408);
}
static void top_point(float x,float y,int *px,int *py)
{
    /* Equal scale on both axes: 18 m spans 1242 px; forward 9 m spans 621. */
    *px=782+(int)(x*69.0f);*py=941-(int)(y*69.0f);
}
/* Nominal azimuth reference, matching the current +/-60 degree search.
 * Nine metres is this plot's display radius, not a verified detection limit.
 * Fill before all measurements: observations outside this reference stay visible. */
static int top_fov_half_width(int forward_px)
{
    int radius_px=(int)(RANGE_MAX_M*69.0f),circle,angle;
    if(forward_px<0||forward_px>radius_px)return 0;
    circle=(int)sqrtf((float)(radius_px*radius_px-forward_px*forward_px));
    angle=(int)((float)forward_px*1.732050808f);
    return circle<angle?circle:angle;
}
static void top_page(Paint *p,const RadarSpatialFrame *s,int tracked)
{
    unsigned i,j;char b[32];int ax,ay,bx,by,yy;
    const uint32_t outside=RGB(7,11,17),inside=RGB(15,28,36);
    const uint32_t dim_grid=RGB(25,35,46),dim_arc=RGB(30,42,54),fov_edge=RGB(49,86,99);
    panel(p,48,204,1440,776);text(p,80,236,4,primary,"平面回波监测");
    text(p,80,286,2,secondary,tracked?"传感器局部坐标 / X 向右 / Y 向前 / 单位 m":"局部坐标 X 向右 / Y 向前 / 等比例坐标");
    rect(p,1120,289,10,10,fov_edge);text(p,1142,286,2,secondary,"参考视场 ±60°");
    rect(p,161,320,1242,621,outside);
    for(yy=320;yy<941;++yy){int half=top_fov_half_width(941-yy);rect(p,782-half,yy,2*half+1,1,inside);}
    for(i=0;i<=6U;++i){int xx=161+(int)i*207,dx=iabs(xx-782);
        int first=941-(int)sqrtf((float)(621*621-dx*dx));
        int last=941-(int)ceilf((float)dx/1.732050808f);
        rect(p,xx,320,1,621,dim_grid);
        if(last>940)last=940;
        if(first<=last)rect(p,xx,first,1,last-first+1,grid);
        (void)snprintf(b,sizeof b,"%d",(int)i*3-9);text(p,xx-12,952,2,secondary,b);
    }
    for(i=0;i<=3U;++i){int row=941-(int)i*207,half=top_fov_half_width(941-row);
        rect(p,161,row,1242,1,dim_grid);rect(p,782-half,row,2*half+1,1,grid);
        (void)snprintf(b,sizeof b,"%u",i*3U);text(p,115,row-7,2,secondary,b);
    }
    for(i=1;i<=3U;++i){float radius=(float)i*3.0f;
        for(j=0;j<72U;++j){float a=(float)j*3.14159265f/72.0f,c=(float)(j+1U)*3.14159265f/72.0f;
            top_point(radius*cosf(a),radius*sinf(a),&ax,&ay);top_point(radius*cosf(c),radius*sinf(c),&bx,&by);
            line(p,ax,ay,bx,by,j>=12U&&j<60U?border:dim_arc);
        }
    }
    top_point(-7.7942286f,4.5f,&ax,&ay);line(p,782,941,ax,ay,fov_edge);
    top_point(7.7942286f,4.5f,&ax,&ay);line(p,782,941,ax,ay,fov_edge);
    rect(p,781,320,2,621,border);marker(p,782,941,warning);
    for(i=0;i<point_count(s);++i)if(xyz_valid(&s->points[i])){
        top_point(s->points[i].x_m,s->points[i].y_m,&ax,&ay);
        if(tracked)rect(p,ax-2,ay-2,4,4,secondary);else marker(p,ax,ay,accent);
        if(!tracked&&i<8U){(void)snprintf(b,sizeof b,"%02u",i+1U);text(p,ax+10,ay-7,2,primary,b);}
    }
    if(!tracked)spatial_empty(p,s,468,580,1);
    text(p,80,338,2,secondary,"Y/M");text(p,1318,952,2,secondary,"X/M");
}
static const char *scene_label(uint32_t value)
{
    switch(value){case RADAR_SCENE_MOVING_OBJECT:return "运动物体";
    case RADAR_SCENE_STATIC_REFLECTOR:return "静态反射体";
    case RADAR_SCENE_HUMAN_CANDIDATE:return "疑似人体";default:return "未知";}
}
static int top_inside(float x,float y)
{
    return isfinite(x)&&isfinite(y)&&fabsf(x)<=8.9f&&y>=0.08f&&y<=8.9f;
}
static void dashed(Paint *p,int ax,int ay,int bx,int by,uint32_t color)
{
    int dx=bx-ax,dy=by-ay,steps=iabs(dx)>iabs(dy)?iabs(dx):iabs(dy),i;
    if(steps==0)return;
    for(i=0;i<=steps;i+=2)if((i/8)%2==0)rect(p,ax+dx*i/steps,ay+dy*i/steps,2,2,color);
}
/* A reflection is placed on this XY plot only if exactly one current, valid
 * XYZ observation matches it. A projected bearing alone cannot supply Y. */
static const RadarSpatialPoint *reflection_xy(const RadarStableReflection *r,const RadarSpatialFrame *s)
{
    const RadarSpatialPoint *found=NULL;unsigned i;
    if(!(r->flags&RADAR_REFLECTION_MEASURED)||r->state==RADAR_REFLECTION_AMBIGUOUS)return NULL;
    for(i=0;i<point_count(s);++i){const RadarSpatialPoint *q=&s->points[i];
        if(!xyz_valid(q)||!isfinite(q->range_m)||fabsf(q->range_m-r->range_m)>0.15f)continue;
        if(r->flags&RADAR_REFLECTION_U_VALID){
            if(!isfinite(q->horizontal_angle_deg)||fabsf(sinf(q->horizontal_angle_deg*0.01745329252f)-r->u)>0.12f)continue;
        }
        if(found)return NULL;
        found=q;
    }
    return found;
}
static int track_has_reflection(const RadarSceneTrack *t,const RadarSceneFrame *scene,const RadarSpatialFrame *s)
{
    unsigned i;if(t->predicted||t->label==RADAR_SCENE_MOVING_OBJECT||t->label==RADAR_SCENE_HUMAN_CANDIDATE)return 0;
    for(i=0;i<scene->reflection_count&&i<RADAR_SCENE_MAX_REFLECTIONS;++i){
        const RadarSpatialPoint *q=reflection_xy(&scene->reflections[i],s);
        if(q&&fabsf(q->x_m-t->x_m)<0.25f&&fabsf(q->y_m-t->y_m)<0.25f)return 1;
    }
    return 0;
}
static const char *reflection_state(const RadarStableReflection *r,int cached)
{
    if(cached)return "历史缓存 / 非当前";
    switch(r->state){case RADAR_REFLECTION_STABLE:return "稳定反射";
    case RADAR_REFLECTION_TENTATIVE:return "稳定性确认中";
    case RADAR_REFLECTION_STALE:return "暂无当前测量";
    case RADAR_REFLECTION_AMBIGUOUS:return "对应关系不明确";
    default:return "质量尚未确认";}
}
static const char *structure_reason(const RadarSceneFrame *scene)
{
    uint32_t f;if(!scene)return "等待场景数据";f=scene->diagnostics.reject_flags;
    if(f&RADAR_SCENE_REJECT_INTEGRITY)return "帧完整性未通过";
    if(f&RADAR_SCENE_REJECT_CLIPPED)return "输入削顶";
    if(f&RADAR_SCENE_REJECT_TIME)return "时间不可信";
    if(f&RADAR_SCENE_REJECT_BUDGET)return "处理预算告警";
    if(f&RADAR_SCENE_REJECT_WARMUP)return "反射地图学习中";
    if(scene->wall_count)return "仅为投影候选";
    if(f&RADAR_SCENE_REJECT_NO_FRESH)return "缺少新的结构证据";
    if(f&RADAR_SCENE_REJECT_POINT_RESPONSE)return "点反射解释存在歧义";
    if(f&RADAR_SCENE_REJECT_SUPPORT)return "支持证据不足";
    if(f&(RADAR_SCENE_REJECT_SPAN|RADAR_SCENE_REJECT_RESIDUAL|RADAR_SCENE_REJECT_OCCUPANCY|RADAR_SCENE_REJECT_GAP))return "几何条件未满足";
    return "暂无确认结构";
}
typedef struct {int x,y,w,h;} PlotLabelBox;
typedef struct {int x0,y0,x1,y1;} PlotLeader;
typedef struct {PlotLabelBox box[20];PlotLeader leader[20];unsigned count,leader_count;} PlotLabels;
static int label_boxes_overlap(PlotLabelBox a,PlotLabelBox b)
{
    return a.x<b.x+b.w+4&&a.x+a.w+4>b.x&&a.y<b.y+b.h+4&&a.y+a.h+4>b.y;
}
/* Test the same one-pixel path that is drawn. Leaders are short and bounded;
 * integer steps avoid floating-point intersection tolerances at text edges. */
static int leader_hits_box(PlotLeader l,PlotLabelBox b,int padding)
{
    int dx=iabs(l.x1-l.x0),sx=l.x0<l.x1?1:-1;
    int dy=-iabs(l.y1-l.y0),sy=l.y0<l.y1?1:-1,err=dx+dy;
    for(;;){int e2;
        if(l.x0>=b.x-padding&&l.x0<b.x+b.w+padding&&
           l.y0>=b.y-padding&&l.y0<b.y+b.h+padding)return 1;
        if(l.x0==l.x1&&l.y0==l.y1)return 0;
        e2=2*err;if(e2>=dy){err+=dy;l.x0+=sx;}if(e2<=dx){err+=dx;l.y0+=sy;}
    }
}
static void plot_leader(Paint *p,PlotLeader l,uint32_t color)
{
    int dx=iabs(l.x1-l.x0),sx=l.x0<l.x1?1:-1;
    int dy=-iabs(l.y1-l.y0),sy=l.y0<l.y1?1:-1,err=dx+dy;
    /* Half-brightness label hue: visible association, subordinate to the dot. */
    color=0xff000000U|(((color&0x00fefefeU)>>1)+((plot_bg&0x00fefefeU)>>1));
    for(;;){int e2;rect(p,l.x0,l.y0,1,1,color);
        if(l.x0==l.x1&&l.y0==l.y1)break;
        e2=2*err;if(e2>=dy){err+=dy;l.x0+=sx;}if(e2<=dx){err+=dx;l.y0+=sy;}
    }
}
static int label_position_free(const PlotLabels *labels,PlotLabelBox box,
                               const RadarSpatialFrame *s,const RadarSceneFrame *scene)
{
    unsigned i;
    if(box.x<170||box.x+box.w>1394||box.y<326||box.y+box.h>894)return 0;
    for(i=0;i<labels->count;++i)if(label_boxes_overlap(box,labels->box[i]))return 0;
    for(i=0;i<labels->leader_count;++i)if(leader_hits_box(labels->leader[i],box,3))return 0;
    /* Text may cross transparent grid lines, but must not cover measured dots. */
    for(i=0;i<point_count(s);++i)if(xyz_valid(&s->points[i])){
        int x,y;PlotLabelBox dot;
        top_point(s->points[i].x_m,s->points[i].y_m,&x,&y);
        dot=(PlotLabelBox){x-7,y-7,15,15};
        if(label_boxes_overlap(box,dot))return 0;
    }
    for(i=0;i<scene->track_count&&i<RADAR_SCENE_MAX_TRACKS;++i){
        int x,y;PlotLabelBox dot;const RadarSceneTrack *t=&scene->tracks[i];
        if(!top_inside(t->x_m,t->y_m))continue;
        top_point(t->x_m,t->y_m,&x,&y);dot=(PlotLabelBox){x-9,y-9,19,19};
        if(label_boxes_overlap(box,dot))return 0;
    }
    return 1;
}
static int leader_position_free(const PlotLabels *labels,PlotLeader l,int point_x,int point_y,
                                const RadarSpatialFrame *s,const RadarSceneFrame *scene)
{
    unsigned i;
    for(i=0;i<labels->count;++i)if(leader_hits_box(l,labels->box[i],3))return 0;
    for(i=0;i<point_count(s);++i)if(xyz_valid(&s->points[i])){
        int x,y;top_point(s->points[i].x_m,s->points[i].y_m,&x,&y);
        if(x==point_x&&y==point_y)continue;
        if(leader_hits_box(l,(PlotLabelBox){x-7,y-7,15,15},2))return 0;
    }
    for(i=0;i<scene->track_count&&i<RADAR_SCENE_MAX_TRACKS;++i){
        int x,y;const RadarSceneTrack *t=&scene->tracks[i];
        if(!top_inside(t->x_m,t->y_m)||track_has_reflection(t,scene,s))continue;
        top_point(t->x_m,t->y_m,&x,&y);
        if(x==point_x&&y==point_y)continue;
        if(leader_hits_box(l,(PlotLabelBox){x-9,y-9,19,19},2))return 0;
    }
    /* Separate nearby associations as well as their text. */
    for(i=0;i<labels->leader_count;++i){
        PlotLeader q=labels->leader[i];
        int dx=iabs(q.x1-q.x0),sx=q.x0<q.x1?1:-1;
        int dy=-iabs(q.y1-q.y0),sy=q.y0<q.y1?1:-1,err=dx+dy;
        for(;;){int e2;if(leader_hits_box(l,(PlotLabelBox){q.x0,q.y0,1,1},1))return 0;
            if(q.x0==q.x1&&q.y0==q.y1)break;
            e2=2*err;if(e2>=dy){err+=dy;q.x0+=sx;}if(e2<=dx){err+=dx;q.y0+=sy;}
        }
    }
    return 1;
}
static void plot_label(Paint *p,PlotLabels *labels,const RadarSpatialFrame *s,
                       const RadarSceneFrame *scene,int point_x,int point_y,
                       uint32_t color,const char *value,unsigned max_chars)
{
    unsigned step,side,reach;int width=text_width(2U,value),best_score=112*112+1,found=0,has_leader=0;
    PlotLabelBox best={0};PlotLeader best_leader={0};
    if(!radar_font_zh_has_utf8(value)&&strlen(value)>max_chars)width=(int)max_chars*12;
    if(labels->count>=sizeof labels->box/sizeof labels->box[0])return;
    for(step=0;step<9U;++step){
        int offset=step?((int)((step+1U)/2U)*24*(step%2U?1:-1)):0;
        for(reach=0;reach<5U;++reach)for(side=0;side<4U;++side){
            int gap=16+(int)reach*24;
            int near_x,near_y,dx,dy,score,needs_leader;PlotLeader leader={0};
            PlotLabelBox box={point_x+gap,point_y-11+offset,width,22};
            if(side==1U)box.x=point_x-width-gap;
            if(side>=2U){box.x=point_x-width/2+offset;box.y=side==2U?point_y-22-gap:point_y+gap;}
            near_x=point_x<box.x?box.x:point_x>=box.x+box.w?box.x+box.w-1:point_x;
            near_y=point_y<box.y?box.y:point_y>=box.y+box.h?box.y+box.h-1:point_y;
            dx=near_x-point_x;dy=near_y-point_y;score=dx*dx+dy*dy;
            if(score>=best_score)continue;
            if(!label_position_free(labels,box,s,scene))continue;
            needs_leader=score>18*18;
            if(needs_leader){
                float length=sqrtf((float)score);
                leader=(PlotLeader){point_x+(int)lroundf(11.0f*dx/length),point_y+(int)lroundf(11.0f*dy/length),
                    near_x-(int)lroundf(3.0f*dx/length),near_y-(int)lroundf(3.0f*dy/length)};
                if(!leader_position_free(labels,leader,point_x,point_y,s,scene))continue;
            }
            best=box;best_leader=leader;best_score=score;has_leader=needs_leader;found=1;
        }
    }
    if(found){
        if(has_leader){plot_leader(p,best_leader,color);labels->leader[labels->leader_count++]=best_leader;}
        label(p,best.x,best.y,2,color,value,max_chars);
        labels->box[labels->count++]=best;
    }
    /* An extremely dense plot keeps dots and the sidebar; never paint a
     * misleading relocated point or overwrite a previous tag to force a fit. */
}
static void scene_top(Paint *p,const RadarSpatialFrame *s,const RadarSceneFrame *scene,int cached)
{
    unsigned i,j,placed=0U;int ax,ay,bx,by;char b[80];
    PlotLabels labels={0};
    top_page(p,s,1);
    if(cached)text(p,1072,244,2,warning,"历史场景");
    if(!scene){text(p,430,580,3,secondary,"等待匹配场景");return;}
    if(scene->flags&RADAR_SCENE_TIME_UNTRUSTED){text(p,468,580,3,warning,"采样时间不可用");return;}
    for(i=0;i<scene->wall_count&&i<RADAR_SCENE_MAX_WALLS;++i){const RadarSceneWall *w=&scene->walls[i];
        if(!top_inside(w->x0_m,w->y0_m)||!top_inside(w->x1_m,w->y1_m))continue;
        top_point(w->x0_m,w->y0_m,&ax,&ay);top_point(w->x1_m,w->y1_m,&bx,&by);
        dashed(p,ax,ay,bx,by,warning);
        text(p,(ax+bx)/2-60,(ay+by)/2-24,2,warning,"结构候选？");
        if(labels.count<sizeof labels.box/sizeof labels.box[0])
            labels.box[labels.count++]=(PlotLabelBox){(ax+bx)/2-60,(ay+by)/2-24,text_width(2U,"结构候选？"),22};
    }
    for(i=0;i<scene->track_count&&i<RADAR_SCENE_MAX_TRACKS;++i){const RadarSceneTrack *t=&scene->tracks[i];
        uint32_t color=t->predicted?warning:t->confirmed?accent:secondary;
        for(j=1;j<t->trail_count&&j<RADAR_SCENE_TRAIL_POINTS;++j){const RadarSceneTrailPoint *a=&t->trail[j-1U],*c=&t->trail[j];
            if(!top_inside(a->x_m,a->y_m)||!top_inside(c->x_m,c->y_m))continue;
            top_point(a->x_m,a->y_m,&ax,&ay);top_point(c->x_m,c->y_m,&bx,&by);line(p,ax,ay,bx,by,border);
            rect(p,bx-1,by-1,3,3,secondary);
        }
        if(track_has_reflection(t,scene,s))continue;
        if(!top_inside(t->x_m,t->y_m))continue;
        top_point(t->x_m,t->y_m,&ax,&ay);
        if(t->predicted){const RadarSceneTrailPoint *last=t->trail_count?&t->trail[t->trail_count-1U]:NULL;
            if(last&&top_inside(last->x_m,last->y_m)){top_point(last->x_m,last->y_m,&bx,&by);dashed(p,bx,by,ax,ay,warning);}
            dashed(p,ax-8,ay-8,ax+8,ay-8,warning);dashed(p,ax-8,ay+8,ax+8,ay+8,warning);
            dashed(p,ax-8,ay-8,ax-8,ay+8,warning);dashed(p,ax+8,ay-8,ax+8,ay+8,warning);
        }else marker(p,ax,ay,color);
        if(scene->track_count>4U)(void)snprintf(b,sizeof b,"ID %u",t->id);
        else (void)snprintf(b,sizeof b,"ID %u  %s",t->id,t->predicted?"预测":t->confirmed?scene_label(t->label):"待确认");
        plot_label(p,&labels,s,scene,ax,ay,color,b,28U);
    }
    for(i=0;i<scene->reflection_count&&i<RADAR_SCENE_MAX_REFLECTIONS;++i){const RadarStableReflection *r=&scene->reflections[i];
        const RadarSpatialPoint *q=reflection_xy(r,s);uint32_t c=(!cached&&r->state==RADAR_REFLECTION_STABLE)?accent:warning;
        if(!q||!top_inside(q->x_m,q->y_m))continue;
        ++placed;
        top_point(q->x_m,q->y_m,&ax,&ay);marker(p,ax,ay,c);
        (void)snprintf(b,sizeof b,"R%u %.2f M",r->id,(double)r->range_m);
        plot_label(p,&labels,s,scene,ax,ay,c,b,20U);
    }
    if(!scene->track_count&&!scene->wall_count&&!placed)text(p,468,580,3,secondary,scene->bearing_count||scene->reflection_count?"仅视线或方位 / 无平面定位":"暂无确认的场景目标");
    if(scene->bearing_count){(void)snprintf(b,sizeof b,"%u 个仅方位 / 无平面坐标",scene->bearing_count);
        rect(p,1022,332,372,48,plot_bg);text(p,1036,345,2,warning,b);}
    if(scene->wall_count)text(p,178,916,2,warning,"投影结构候选 / 尚非实测平面");
}
static void scene_sidebar(Paint *p,const RadarSceneFrame *scene,const RadarRangeFrame *h,
                          unsigned count,const RadarViewDiagnostics *d,int cached)
{
    char b[80];unsigned i,n=scene?scene->reflection_count:0U;
    cached=cached||(d&&d->frame_age_valid&&d->frame_age_us>UINT64_C(500000))||
        (scene&&(scene->flags&RADAR_SCENE_TIME_UNTRUSTED));
    panel(p,1512,204,360,776);
    text(p,1540,232,2,warning,"实验性标签");
    text(p,1540,266,3,scene&&scene->calibrated?accent:warning,scene&&scene->calibrated?"已标定":"尚未标定");
    text(p,1540,308,2,secondary,"反射 / 视线距离");
    if(cached)(void)snprintf(b,sizeof b,"缓存 / 观测 %u",n);
    else (void)snprintf(b,sizeof b,"稳定 %u / 观测 %u",scene?scene->diagnostics.stable_count:0U,n);
    text(p,1540,340,2,primary,b);
    text(p,1540,372,2,secondary,"帧龄");
    if(d&&d->frame_age_valid&&scene)number_text(p,1600,372,2,primary,(float)((double)d->frame_age_us/1e6),2," S");
    else text(p,1600,372,2,secondary,"--");
    capture_rate(p,1740,372,2,h,count);
    text(p,1540,402,2,secondary,"采样时刻");
    if(scene&&!(scene->flags&RADAR_SCENE_TIME_UNTRUSTED))number_text(p,1648,402,2,primary,(float)((double)scene->timestamp_us/1e6),2," S");
    else text(p,1648,402,2,secondary,"--");
    rect(p,1540,432,304,1,border);
    for(i=0;i<n&&i<RADAR_SCENE_MAX_REFLECTIONS;++i){const RadarStableReflection *r=&scene->reflections[i];int y=450+(int)i*46;
        uint32_t color=(!cached&&r->state==RADAR_REFLECTION_STABLE)?accent:secondary;
        uint64_t age=scene->timestamp_us>=r->last_observed_us?scene->timestamp_us-r->last_observed_us:0U;
        if(d&&d->frame_age_valid)age+=d->frame_age_us;
        (void)snprintf(b,sizeof b,"R%u  %.2f M  %u MS",r->id,(double)r->range_m,(unsigned)(age/1000U));label(p,1540,y,2,color,b,25U);
        label(p,1540,y+22,1,cached||r->state==RADAR_REFLECTION_AMBIGUOUS?warning:secondary,reflection_state(r,cached),45U);
    }
    if(!n)text(p,1540,460,2,secondary,scene?"暂无合格反射":"场景数据不可用");
    rect(p,1540,824,304,1,border);
    text(p,1540,846,2,secondary,"结构证据");
    (void)snprintf(b,sizeof b,"%u 组 / %u 线段",scene?scene->diagnostics.independent_groups:0U,scene?scene->wall_count:0U);label(p,1540,874,2,secondary,b,25U);
    text(p,1540,902,1,secondary,"稳定不代表精度或类别");
    label(p,1540,928,2,warning,structure_reason(scene),25U);
    if(scene&&(scene->flags&(RADAR_SCENE_INPUT_TRUNCATED|RADAR_SCENE_CAPACITY)))text(p,1540,956,2,warning,"结果容量受限");
    else if(scene&&(scene->flags&RADAR_SCENE_AMBIGUOUS_ASSOCIATION))text(p,1540,956,2,warning,"对应关系不明确");
    else text(p,1540,956,2,secondary,"高度需有效空间坐标");
}
static void space_project(float x,float y,float z,int *px,int *py)
{
    /* Orthographic projection with equal world scale. Projection rows are
     * orthogonal and have equal norm; no perspective exaggerates height. */
    *px=500+(int)(24.0f*x+24.0f*y);
    *py=628+(int)(13.8564065f*x-13.8564065f*y-27.712813f*z);
}
static void space_line(Paint *p,float x0,float y0,float z0,float x1,float y1,float z1,uint32_t c)
{
    int ax,ay,bx,by;space_project(x0,y0,z0,&ax,&ay);space_project(x1,y1,z1,&bx,&by);line(p,ax,ay,bx,by,c);
}
static void space_page(Paint *p,const RadarSpatialFrame *s)
{
    unsigned i;int ax,ay;char b[24];
    panel(p,48,204,1440,776);text(p,80,236,4,primary,"空间分布");
    text(p,80,286,2,secondary,"固定三维视角 / 雷达局部坐标");
    rect(p,80,328,920,610,plot_bg);rect(p,1024,328,1,610,border);
    for(i=0;i<=6U;++i){float x=(float)((int)i*3-9),end=sqrtf(81.0f-x*x);space_line(p,x,0.0f,0.0f,x,end,0.0f,grid);}
    for(i=0;i<=3U;++i){float y=(float)i*3.0f,end=sqrtf(81.0f-y*y);space_line(p,-end,y,0.0f,end,y,0.0f,grid);}
    space_line(p,-9,0,0,9,0,0,border);space_line(p,0,0,0,0,9,0,border);space_line(p,0,0,-6,0,0,6,border);
    space_project(9,0,0,&ax,&ay);text(p,ax+12,ay,2,primary,"X");
    space_project(0,9,0,&ax,&ay);text(p,ax+12,ay-12,2,primary,"Y");
    space_project(0,0,6,&ax,&ay);text(p,ax-6,ay-28,2,primary,"Z");
    space_project(0,0,0,&ax,&ay);marker(p,ax,ay,warning);
    text(p,80,902,2,secondary,"网格 3 m / 固定视角 / 不显示跟踪");
    text(p,1060,328,2,primary,"侧视图 Y / Z");
    rect(p,1112,364,270,540,plot_bg);
    for(i=0;i<=6U;++i){int yy=634+((int)i-3)*90;rect(p,1112,yy,270,1,grid);(void)snprintf(b,sizeof b,"%d",9-(int)i*3);text(p,1068,yy-7,2,secondary,b);}
    for(i=0;i<=3U;++i){int xx=1112+(int)i*90;rect(p,xx,364,1,540,grid);(void)snprintf(b,sizeof b,"%u",i*3U);text(p,xx-5,920,2,secondary,b);}
    /* Side view uses 30 px/m on both axes; +Z is up and +Y is right. */
    rect(p,1112,634,270,1,border);marker(p,1112,634,warning);
    for(i=0;i<point_count(s);++i)if(xyz_valid(&s->points[i])){
        const RadarSpatialPoint *q=&s->points[i];space_project(q->x_m,q->y_m,q->z_m,&ax,&ay);marker(p,ax,ay,accent);
        if(i<8U){(void)snprintf(b,sizeof b,"%02u",i+1U);text(p,ax+10,ay-7,2,primary,b);}
        marker(p,1112+(int)(q->y_m*30.0f),634-(int)(q->z_m*30.0f),accent);
    }
    spatial_empty(p,s,288,396,1);
    text(p,80,952,2,secondary,"Z=0 为雷达平面 / 并非地面");text(p,1398,920,2,secondary,"Y/M");
}
/* Translate only the view. All callers and failure checks retain their ABI strings. */
static const char *display_state(const char *s)
{
    static const struct {const char *key,*value;} labels[]={
        {"LIVE","持续采集"},{"LIVE STARTING","采集启动中"},{"LIVE STOPPING","采集停止中"},
        {"LIVE STOPPED","已停止"},{"STOPPED","已停止"},{"FRAME READY","帧已就绪"},
        {"WAITING FOR FRAME","等待采集帧"},{"STREAM ERROR","连续采集错误"},
        {"STREAM ERROR / STOPPED","采集错误 / 已停止"},{"SAFE FAILED","安全停止失败"},
        {"FAILED","处理失败"},{"PAUSED","已暂停"},{"IDLE","空闲"},
        {"READY","就绪"},{"PROCESSING","数据处理中"},{"CAPTURING","正在采集"},
        {"WAIT PREFETCH","等待预取帧"},{"COMPLETE","已完成"},{"RETAINED FRAME","保留有效帧"},
        {"QUALIFY DMA","DMA 检查中"},{"CONNECTING","连接中"},{"QUERYING","查询中"},
        {"CANCELED","已取消"},{"CLEARED","已清除"},{"WAIT DISPLAY","等待显示"},
        {"MAP RESET","地图已清除"}};
    unsigned i;
    for(i=0;i<sizeof labels/sizeof labels[0];++i)if(!strcmp(s,labels[i].key))return labels[i].value;
    return s; /* Preserve unfamiliar diagnostic codes rather than invent a state. */
}
void radar_scope_render_scene(uint32_t *pixels,unsigned stride_words,const RadarRangeFrame *history,unsigned count,
                        const char *status,RadarViewPage page,const RadarViewDiagnostics *diagnostics,
                        const RadarSpatialFrame *spatial,const RadarSceneFrame *scene)
{
    static const char *const tabs[]={"系统总览","方位热图","平面回波监测","空间分布"};
    Paint p={pixels,stride_words};unsigned i;char b[96];uint32_t state_color=accent;const char *state_label;
    int failed,paused,live_mode;
    if(!pixels||stride_words<RADAR_SCOPE_WIDTH)return;
    if((unsigned)page>=RADAR_VIEW_COUNT)page=RADAR_VIEW_OVERVIEW;
    if(!history)count=0;
    if(count>RADAR_SCOPE_HISTORY_MAX){history+=count-RADAR_SCOPE_HISTORY_MAX;count=RADAR_SCOPE_HISTORY_MAX;}
    if(!status)status=count?"FRAME READY":"WAITING FOR FRAME";
    failed=strstr(status,"FAIL")!=NULL||strstr(status,"FAULT")!=NULL||strstr(status,"ERROR")!=NULL;
    paused=strstr(status,"STOP")!=NULL||strstr(status,"PAUSE")!=NULL;
    live_mode=strstr(status,"LIVE")!=NULL;
    if(failed)state_color=danger;else if(paused)state_color=warning;
    state_label=live_mode&&paused?"STOPPED":status;
    rect(&p,0,0,RADAR_SCOPE_WIDTH,RADAR_SCOPE_HEIGHT,canvas);
    rect(&p,48,34,6,44,accent);text(&p,76,36,5,primary,"RADAR");
    text(&p,76,92,2,secondary,"基于IWR6843ISK的LVDS信号与米联客F20开发板设计");
    if(diagnostics){(void)snprintf(b,sizeof b,"%u TX / %u RX",diagnostics->tx_count,diagnostics->rx_count);text(&p,940,44,3,primary,b);}
    else text(&p,940,44,3,secondary,"天线 --");
    rect(&p,1292,42,7,23,state_color);label(&p,1316,44,3,state_color,display_state(state_label),29U);
    radar_font_zh_draw(p.p,p.stride,RADAR_SCOPE_WIDTH,RADAR_SCOPE_HEIGHT,
        1872-radar_font_zh_measure(20U,"DP/1080P/60fps"),44,20U,secondary,"DP/1080P/60fps",400);
    text(&p,48,1020,2,secondary,"KEY3 短按切页 / 长按回首页");
    text(&p,1208,1020,2,secondary,"采集率不等于显示刷新率");
    if(failed)label(&p,48,1052,2,danger,display_state(status),145U);
    else if(paused)text(&p,48,1052,2,warning,"采集已暂停 / 保留最近有效数据");
    else if(live_mode)text(&p,48,1052,2,accent,"持续采集 / 切页随下一正常帧更新生效");
    else text(&p,48,1052,2,secondary,count?(page==RADAR_VIEW_OVERVIEW?"实测帧数据 / 本页不显示测角":"实测帧 / 图中为散射回波点"):"等待有效数据 / 无演示样本");
    for(i=0;i<RADAR_VIEW_COUNT;++i){int x=48+(int)i*456;
        rect(&p,x,140,456,40,i==(unsigned)page?surface:canvas);
        (void)snprintf(b,sizeof b,"0%u",i+1U);text(&p,x+16,151,2,i==(unsigned)page?accent:secondary,b);
        text(&p,x+64,151,2,i==(unsigned)page?primary:secondary,tabs[i]);
        rect(&p,x,180,456,2,i==(unsigned)page?accent:border);
    }
    if(page==RADAR_VIEW_OVERVIEW)overview(&p,history,count,spatial);
    else {
        /* A caller may still hold the previous frame while new processing is
         * under way. Never combine its spatial data with another session. */
        if(!count || !history[count-1U].valid || !spatial ||
           spatial->session!=history[count-1U].session ||
           spatial->timestamp_us!=history[count-1U].timestamp_us)spatial=NULL;
        if(page==RADAR_VIEW_AZIMUTH)azimuth_page(&p,spatial);
        else if(page==RADAR_VIEW_TOP){
            if(scene&&(!scene->valid||!spatial||scene->session!=spatial->session||scene->timestamp_us!=spatial->timestamp_us))scene=NULL;
            scene_top(&p,spatial,scene,paused||failed||(diagnostics&&diagnostics->frame_age_valid&&diagnostics->frame_age_us>UINT64_C(500000)));
        }
        else space_page(&p,spatial);
        if(page==RADAR_VIEW_TOP)scene_sidebar(&p,scene,history,count,diagnostics,paused||failed);
        else spatial_sidebar(&p,spatial,history,count,diagnostics);
    }
}
void radar_scope_render(uint32_t *pixels,unsigned stride_words,const RadarRangeFrame *history,unsigned count,
                        const char *status,RadarViewPage page,const RadarViewDiagnostics *diagnostics,
                        const RadarSpatialFrame *spatial)
{
    radar_scope_render_scene(pixels,stride_words,history,count,status,page,diagnostics,spatial,NULL);
}
