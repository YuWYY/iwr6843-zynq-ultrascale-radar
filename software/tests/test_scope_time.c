/* Production renderer and timestamp validity seams; no key/monitor tests. */
#include "../radar_runtime/src/display/radar_scope_view.c"
#include <assert.h>
#include <stdio.h>
static uint32_t pixels[1920U*1080U+2U];
static RadarRangeFrame history[3];
static RadarSpatialFrame spatial;
int main(void)
{
    unsigned i,p;RadarViewDiagnostics d={0};
    for(i=0;i<3U;++i) {
        history[i].valid=1U;history[i].session=i+1U;history[i].timestamp_us=i*100000U;
        history[i].bin_m=(float)RADAR_RANGE_BIN_METERS;
    }
    assert(fabsf(history_span(history,3U)-0.2f)<0.000001f);
    history[1].timestamp_us=0U;history[1].time_untrusted=1U;
    assert(history_span(history,3U)==0.0f);
    spatial.session=3U;spatial.timestamp_us=200000U;spatial.valid=1U;
    pixels[0]=0x12345678U;pixels[1920U*1080U+1U]=0x87654321U;
    for(p=0;p<4U;++p)radar_scope_render(pixels+1U,1920U,history,3U,"LIVE",(RadarViewPage)p,&d,&spatial);
    for(i=0;i<3U;++i)history[i].time_untrusted=1U;
    radar_scope_render(pixels+1U,1920U,history,3U,"LIVE",RADAR_VIEW_OVERVIEW,&d,&spatial);
    assert(pixels[0]==0x12345678U&&pixels[1920U*1080U+1U]==0x87654321U);
    puts("PASS production renderer excludes unknown sample times and preserves four-page bounds; not visual board evidence");
    return 0;
}
