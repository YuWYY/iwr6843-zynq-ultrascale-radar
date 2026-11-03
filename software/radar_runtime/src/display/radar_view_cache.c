/* SPDX-License-Identifier: MIT */
#include "radar_view_cache.h"
#include <string.h>
static int newer(uint32_t a,uint32_t b)
{ uint32_t delta=a-b;return delta && delta<UINT32_C(0x80000000); }
void radar_view_cache_init(RadarViewCache *c,RadarViewSnapshot *a,RadarViewSnapshot *b)
{ memset(c,0,sizeof *c);c->slot[0]=a;c->slot[1]=b;c->latest=c->leased=-1; }
int radar_view_cache_publish(RadarViewCache *c,const RadarRangeFrame *h,uint32_t count,
    const RadarSpatialFrame *s,const RadarViewDiagnostics *d,const char *status,uint32_t revision)
{
    RadarViewSnapshot *v;int index;
    if(!c || !h || !s || !d || !status || !revision || count>RADAR_VIEW_CACHE_HISTORY || !c->slot[0] || !c->slot[1])return -1;
    if(c->latest>=0 && !newer(revision,c->slot[c->latest]->revision))return -1;
    index=c->leased>=0?(c->leased^1):(c->latest<0?0:c->latest^1);
    v=c->slot[index];
    if(c->latest>=0 && c->latest!=c->leased &&
       newer(c->slot[c->latest]->revision,c->last_rendered_revision))++c->coalesced;
    memcpy(v->history,h,count*sizeof(*h));v->spatial=*s;v->diagnostics=*d;
    strncpy(v->status,status,sizeof v->status-1U);v->status[sizeof v->status-1U]=0;
    memset(&v->scene,0,sizeof v->scene);v->count=count;v->revision=revision;c->latest=index;++c->published;return 0;
}
const RadarViewSnapshot *radar_view_cache_acquire(RadarViewCache *c)
{
    if(!c || c->latest<0 || c->leased>=0)return NULL;
    c->leased=c->latest;return c->slot[c->leased];
}
int radar_view_cache_release(RadarViewCache *c,uint32_t revision)
{
    if(!c || c->leased<0 || c->slot[c->leased]->revision!=revision)return -1;
    c->last_rendered_revision=revision;c->leased=-1;++c->rendered;return 0;
}
void radar_view_cache_diagnostics(const RadarViewSnapshot *s,uint64_t now,RadarViewDiagnostics *d)
{
    *d=s->diagnostics;
    if(d->frame_age_valid) {
        if(now<d->sampled_us || now-d->sampled_us>UINT64_MAX-d->frame_age_us)d->frame_age_valid=0U;
        else d->frame_age_us+=now-d->sampled_us;
    }
}
