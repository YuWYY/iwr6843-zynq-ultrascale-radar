/* SPDX-License-Identifier: MIT */
#include "radar_spatial.h"
#include <math.h>
#include <string.h>

#define SPATIAL_MAGIC UINT32_C(0x53504154)
#define PI_D 3.1415926535897932384626433832795
#define PI_F 3.14159265358979323846f
#define NOISE_FLOOR 1.0e-16f
#define CFAR_FACTOR 15.848931924611133f /* +12 dB engineering threshold */
enum { IDLE=0, COLLECT, DOPPLER, HEATMAP, CFAR, ANGLES, COMPLETE, FAILED, PL_IMPORT };
static void prepare_angle(RadarSpatialProcessor *p);
static const unsigned tx_sequence[3]={0U,2U,1U};
static const unsigned physical_tx_slot[3]={0U,2U,1U};
static const unsigned horizontal_channels[8]={0U,1U,2U,3U,8U,9U,10U,11U};
/* Physical TX-major ISK geometry in units of2.5mm. x is negative here;
 * z=-1 for TX0/TX2 and z=0 for TX1. The renderer declares a nominal frame.
 * Synthetic incident phase exp(+j*2pi*(x*u+z*v)/lambda), conjugate steering. */
static const unsigned minus_x[12]={0U,1U,2U,3U,2U,3U,4U,5U,4U,5U,6U,7U};

static RadarComplex multiply(RadarComplex a,RadarComplex b)
{
    RadarComplex c={a.re*b.re-a.im*b.im,a.re*b.im+a.im*b.re}; return c;
}
static float magnitude2(RadarComplex a) { return a.re*a.re+a.im*a.im; }
static float power_db(float x) { return x>NOISE_FLOOR ? 10.0f*log10f(x) : -160.0f; }
static int signed_bin(unsigned k) { return k<16U ? (int)k : (int)k-32; }
static int failure(RadarSpatialProcessor *p,int code)
{
    p->stage=FAILED; p->error=code; p->frame.error=code; p->frame.valid=0U; return code;
}
static int bounded_string(const char *s,size_t n)
{
    return s && s[0] && memchr(s,0,n)!=NULL;
}

void radar_spatial_init(RadarSpatialProcessor *p)
{
    unsigned n,tx,a,e;
    float range_sum=0.0f;
    const double q=2.0*PI_D*RADAR_SPATIAL_PITCH_M*RADAR_SPATIAL_FC_HZ/299792458.0;
    if(!p) return;
    memset(p,0,sizeof(*p));
    for(n=0U;n<256U;++n)
        range_sum+=0.5f-0.5f*cosf(2.0f*PI_F*(float)n/255.0f);
    p->range_scale=1.0f/(32768.0f*range_sum);
    for(n=0U;n<32U;++n) {
        p->slow_window[n]=0.5f-0.5f*cosf(2.0f*PI_F*(float)n/31.0f);
        p->slow_sum+=p->slow_window[n];
        p->slow_energy+=p->slow_window[n]*p->slow_window[n];
    }
    p->rd_scale=1.0f/(12.0f*p->slow_sum*p->slow_sum);
    /* Parseval: sum|DFT(w*x)|^2 = N*sum|w*x|^2. Unit-amplitude bin tone,
     * steered exactly, has unit integrated beam power independent of N/M. */
    p->heat_scale=1.0f/(64.0f*32.0f*p->slow_energy);
    for(n=0U;n<16U;++n) {
        double phase=-2.0*PI_D*(double)n/32.0;
        p->slow_twiddle[n].re=(float)cos(phase);
        p->slow_twiddle[n].im=(float)sin(phase);
    }
    for(tx=0U;tx<3U;++tx) for(n=0U;n<32U;++n) {
        double phase=-2.0*PI_D*(double)signed_bin(n)*(double)physical_tx_slot[tx]/96.0;
        p->tdm[tx][n].re=(float)cos(phase); p->tdm[tx][n].im=(float)sin(phase);
    }
    for(a=0U;a<RADAR_SPATIAL_MAP_ANGLES;++a) {
        double az=((double)a-60.0)*PI_D/180.0;
        p->horizontal_root[a].re=(float)cos(q*sin(az));
        p->horizontal_root[a].im=(float)sin(q*sin(az));
        for(e=0U;e<RADAR_SPATIAL_ELEVATIONS;++e) {
            double el=((double)e-15.0)*PI_D/180.0;
            double phase=q*sin(az)*cos(el);
            p->steering_x[e][a].re=(float)cos(phase);
            p->steering_x[e][a].im=(float)sin(phase);
        }
    }
    for(e=0U;e<RADAR_SPATIAL_ELEVATIONS;++e) {
        double phase=q*sin(((double)e-15.0)*PI_D/180.0);
        p->steering_z[e].re=(float)cos(phase); p->steering_z[e].im=(float)sin(phase);
    }
    p->initialized=SPATIAL_MAGIC;
}

int radar_spatial_cache_weights(RadarSpatialProcessor *p,RadarSpatialWeights *w)
{
    unsigned a,e,c,l;
    if(!p || !w || p->initialized!=SPATIAL_MAGIC || p->stage!=IDLE)return RADAR_SPATIAL_E_STATE;
    for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a) {
        double rr=1.0,ii=0.0;
        RadarComplex root=p->horizontal_root[a];
        for(l=1;l<8U;++l) {
            double next=rr*root.re-ii*root.im;
            ii=rr*root.im+ii*root.re;rr=next;
            w->heat[a][l-1U][0]=rr;w->heat[a][l-1U][1]=ii;
        }
        for(e=0;e<RADAR_SPATIAL_ELEVATIONS;++e) {
            RadarComplex powers[8];powers[0].re=1.0f;powers[0].im=0.0f;
            for(c=1;c<8U;++c)powers[c]=multiply(powers[c-1U],p->steering_x[e][a]);
            for(c=0;c<12U;++c) {
                RadarComplex v=powers[minus_x[c]];
                if(c<4U || c>=8U)v=multiply(v,p->steering_z[e]);
                w->angle[a][e][c]=v;
            }
        }
    }
    p->weights=w;return 0;
}

int radar_spatial_begin(RadarSpatialProcessor *p,uint32_t session,uint64_t timestamp_us,
                        const RadarArrayCalibration *cal)
{ return radar_spatial_begin_config(p,session,timestamp_us,cal,RADAR_RANGE_CFG_SHA256); }
int radar_spatial_begin_config(RadarSpatialProcessor *p,uint32_t session,uint64_t timestamp_us,
                        const RadarArrayCalibration *cal,const char *cfg_sha256)
{
    unsigned c,r,a;
    if(!p) return RADAR_SPATIAL_E_ARGUMENT;
    if(p->initialized!=SPATIAL_MAGIC ||
       (p->stage!=IDLE && p->stage!=COMPLETE && p->stage!=FAILED))
        return RADAR_SPATIAL_E_STATE;
    memset(&p->frame,0,sizeof(p->frame));
    p->frame.session=session; p->frame.timestamp_us=timestamp_us;
    p->error=0;
    if(!session || !cfg_sha256 || strlen(cfg_sha256)!=64U) return failure(p,RADAR_SPATIAL_E_ARGUMENT);
    memset(&p->calibration,0,sizeof(p->calibration));
    if(cal) {
        if(!bounded_string(cal->array_id,sizeof(cal->array_id)) ||
           !bounded_string(cal->cfg_sha256,sizeof(cal->cfg_sha256)) ||
           !bounded_string(cal->calibration_id,sizeof(cal->calibration_id)) ||
           strcmp(cal->array_id,RADAR_SPATIAL_ARRAY_ID) ||
           strcmp(cal->cfg_sha256,cfg_sha256) ||
           !isfinite(cal->range_bias_m) || cal->measured>1U ||
           (cal->measured && !strcmp(cal->calibration_id,"UNCALIBRATED")))
            return failure(p,RADAR_SPATIAL_E_CALIBRATION);
        for(c=0U;c<12U;++c) {
            double norm=(double)cal->channel[c].re*cal->channel[c].re+
                        (double)cal->channel[c].im*cal->channel[c].im;
            if(!isfinite(cal->channel[c].re) || !isfinite(cal->channel[c].im) ||
               !isfinite(norm) || norm<=0.0)
                return failure(p,RADAR_SPATIAL_E_CALIBRATION);
        }
        p->calibration=*cal;
    } else {
        for(c=0U;c<12U;++c) p->calibration.channel[c].re=1.0f;
        strcpy(p->calibration.array_id,RADAR_SPATIAL_ARRAY_ID);
        strcpy(p->calibration.cfg_sha256,cfg_sha256);
        strcpy(p->calibration.calibration_id,"UNCALIBRATED");
    }
    for(c=0;c<12U;++c)for(a=0;a<32U;++a)
        p->combined[c][a]=multiply(p->calibration.channel[c],p->tdm[c/4U][a]);
    p->frame.calibrated=p->calibration.measured;
    memcpy(p->frame.calibration_id,p->calibration.calibration_id,sizeof(p->frame.calibration_id));
    /* Excluded near-range rows stay explicitly missing; they are not samples. */
    for(r=0U;r<RADAR_SPATIAL_MAP_RANGES;++r)
        for(a=0U;a<RADAR_SPATIAL_MAP_ANGLES;++a) p->frame.heatmap_dbfs[r][a]=NAN;
    memset(p->rd_power,0,sizeof(p->rd_power));
    memset(p->candidate_power,0,sizeof(p->candidate_power));
    p->received_ffts=p->cursor=p->point_index=p->angle_index=0U;
    p->pl_import=0U;
    p->stage=COLLECT;
    return RADAR_SPATIAL_BUSY;
}

int radar_spatial_accept_fft(void *context,uint32_t chirp,uint32_t rx,
                             const float *re,const float *im)
{
    RadarSpatialProcessor *p=(RadarSpatialProcessor *)context;
    unsigned r,c,loop;
    if(!p) return RADAR_SPATIAL_E_ARGUMENT;
    if(p->initialized!=SPATIAL_MAGIC) return RADAR_SPATIAL_E_STATE;
    if(p->stage!=COLLECT) return failure(p,RADAR_SPATIAL_E_STATE);
    if(!re || !im) return failure(p,RADAR_SPATIAL_E_ARGUMENT);
    if(chirp>=96U || rx>=4U || chirp*4U+rx!=p->received_ffts)
        return failure(p,RADAR_SPATIAL_E_SEQUENCE);
    c=tx_sequence[chirp%3U]*4U+rx; loop=chirp/3U;
    for(r=0U;r<256U;++r) {
        if(!isfinite(re[r]) || !isfinite(im[r])) return failure(p,RADAR_SPATIAL_E_NUMERIC);
        p->cube[r][c][loop].re=re[r]*p->range_scale;
        p->cube[r][c][loop].im=im[r]*p->range_scale;
        if(!isfinite(p->cube[r][c][loop].re) || !isfinite(p->cube[r][c][loop].im))
            return failure(p,RADAR_SPATIAL_E_NUMERIC);
    }
    ++p->received_ffts;
    if(p->received_ffts==384U) { p->stage=DOPPLER; p->cursor=0U; }
    return 0;
}

static void fft32(RadarComplex *x,const RadarComplex *twiddle)
{
    unsigned i,j=0U,length;
    for(i=1U;i<32U;++i) {
        unsigned bit=16U;
        while(j&bit) { j^=bit;bit>>=1; }
        j^=bit;
        if(i<j) { RadarComplex temp=x[i];x[i]=x[j];x[j]=temp; }
    }
    for(length=2U;length<=32U;length<<=1) {
        unsigned half=length>>1,stride=32U/length;
        for(i=0U;i<32U;i+=length) for(j=0U;j<half;++j) {
            RadarComplex t=multiply(twiddle[j*stride],x[i+j+half]);
            RadarComplex lower=x[i+j];
            x[i+j].re=lower.re+t.re; x[i+j].im=lower.im+t.im;
            x[i+j+half].re=lower.re-t.re; x[i+j+half].im=lower.im-t.im;
        }
    }
}

static int doppler_one(RadarSpatialProcessor *p)
{
    unsigned r=p->cursor/12U,c=p->cursor%12U,k;
    RadarComplex *x=p->cube[r][c];
    for(k=0U;k<32U;++k) { x[k].re*=p->slow_window[k];x[k].im*=p->slow_window[k]; }
    fft32(x,p->slow_twiddle);
    for(k=0U;k<32U;++k) {
        float v=magnitude2(x[k]);
        if(!isfinite(v)) return failure(p,RADAR_SPATIAL_E_NUMERIC);
        /* Detector power remains ADC-relative before channel calibration.
         * Unit-magnitude TDM correction cannot alter this detector power. */
        p->rd_power[r][k]+=v*p->rd_scale;
        x[k]=multiply(x[k],p->combined[c][k]);
        if(!isfinite(x[k].re) || !isfinite(x[k].im))
            return failure(p,RADAR_SPATIAL_E_NUMERIC);
    }
    if(++p->cursor==256U*12U) { p->stage=HEATMAP;p->cursor=RADAR_RANGE_FIRST_BIN; }
    return RADAR_SPATIAL_BUSY;
}

static int heatmap_one(RadarSpatialProcessor *p)
{
    unsigned r=p->cursor,k,a,b,l,column;
    double cr[8]={0},ci[8]={0};
    for(k=0U;k<32U;++k) for(a=0U;a<8U;++a) {
        RadarComplex x=p->cube[r][horizontal_channels[a]][k];
        cr[0]+=(double)x.re*x.re+(double)x.im*x.im;
        for(b=0U;b<a;++b) {
            RadarComplex y=p->cube[r][horizontal_channels[b]][k];
            l=a-b;
            cr[l]+=(double)x.re*y.re+(double)x.im*y.im;
            ci[l]+=(double)x.im*y.re-(double)x.re*y.im;
        }
    }
    if(!isfinite(cr[0])) return failure(p,RADAR_SPATIAL_E_NUMERIC);
    for(column=0U;column<RADAR_SPATIAL_MAP_ANGLES;++column) {
        RadarComplex root=p->horizontal_root[column];
        double rr=1.0,ii=0.0,power=cr[0];
        for(l=1U;l<8U;++l) {
            if(p->weights) {
                rr=p->weights->heat[column][l-1U][0];ii=p->weights->heat[column][l-1U][1];
            } else {
                double next=rr*root.re-ii*root.im;
                ii=rr*root.im+ii*root.re;rr=next;
            }
            power+=2.0*(cr[l]*rr-ci[l]*ii);
        }
        if(!isfinite(power) || power < -1.0e-4*cr[0])
            return failure(p,RADAR_SPATIAL_E_NUMERIC);
        if(power<0.0) power=0.0;
        p->frame.heatmap_dbfs[r][column]=power_db((float)(power*(double)p->heat_scale));
    }
    if(++p->cursor>RADAR_RANGE_LAST_BIN) {
        if(p->pl_import) {
            if(!p->frame.count) { p->frame.valid=1U;p->stage=COMPLETE;return RADAR_SPATIAL_DONE; }
            p->stage=ANGLES;p->point_index=0U;prepare_angle(p);
        } else { p->stage=CFAR;p->cursor=RADAR_RANGE_FIRST_BIN; }
    }
    return RADAR_SPATIAL_BUSY;
}

static int local_maximum(const RadarSpatialProcessor *p,unsigned r,unsigned k,float v)
{
    int dr,dk;
    for(dr=-1;dr<=1;++dr) for(dk=-1;dk<=1;++dk) {
        int rr=(int)r+dr;
        unsigned kk=(unsigned)((int)k+dk+32)%32U;
        float other;
        if(rr<(int)RADAR_RANGE_FIRST_BIN || rr>(int)RADAR_RANGE_LAST_BIN ||
           (dr==0 && dk==0)) continue;
        other=p->rd_power[rr][kk];
        if(other>v || (other==v && ((unsigned)rr<r || ((unsigned)rr==r && kk<k)))) return 0;
    }
    return 1;
}

static void add_candidate(RadarSpatialProcessor *p,unsigned r,unsigned k,float power,float noise)
{
    RadarSpatialPoint point;
    unsigned i,j,old_count=p->frame.count;
    ++p->frame.candidate_count;
    for(i=0U;i<old_count;++i) {
        const RadarSpatialPoint *old=&p->frame.points[i];
        unsigned old_k=(unsigned)(old->doppler_bin+32)%32U;
        if(power>p->candidate_power[i] ||
           (power==p->candidate_power[i] && (r<old->range_bin || (r==old->range_bin && k<old_k)))) break;
    }
    if(i>=RADAR_SPATIAL_MAX_POINTS) return;
    if(p->frame.count<RADAR_SPATIAL_MAX_POINTS) ++p->frame.count;
    for(j=p->frame.count-1U;j>i;--j) {
        p->frame.points[j]=p->frame.points[j-1U];p->candidate_power[j]=p->candidate_power[j-1U];
    }
    memset(&point,0,sizeof(point));
    point.range_bin=r; point.doppler_bin=signed_bin(k);
    point.range_m=(float)((double)r*RADAR_RANGE_BIN_METERS)-(float)p->calibration.range_bias_m;
    point.velocity_mps=(float)((double)point.doppler_bin*RADAR_SPATIAL_VELOCITY_BIN_MPS);
    point.snr_db=10.0f*log10f(power/noise); point.power_dbfs=power_db(power);
    point.azimuth_deg=point.elevation_deg=point.horizontal_angle_deg=NAN;
    point.x_m=point.y_m=point.z_m=NAN;
    p->frame.points[i]=point;p->candidate_power[i]=power;
}

static void prepare_angle(RadarSpatialProcessor *p)
{
    const RadarSpatialPoint *point=&p->frame.points[p->point_index];
    unsigned c,k=(unsigned)(point->doppler_bin+32)%32U;
    p->angle_index=0U; p->best_angle_power=-1.0f; p->angle_energy=0.0f;
    p->best_azimuth=-60;p->best_elevation=-15;
    for(c=0U;c<12U;++c) p->angle_energy+=magnitude2(p->cube[point->range_bin][c][k]);
}

int radar_spatial_import_pl_begin(RadarSpatialProcessor *p,
    const RadarDspCandidateHeader *header,const RadarDspCandidate *candidates,RadarDspBackend backend)
{
    unsigned i;
    const double scale=(256.0/127.5)*(256.0/127.5)*(32.0/15.5)*(32.0/15.5)/(17592186044416.0*12.0);
    if(!p || !header || !candidates)return RADAR_SPATIAL_E_ARGUMENT;
    if(p->initialized!=SPATIAL_MAGIC || p->stage!=COLLECT || p->received_ffts)
        return RADAR_SPATIAL_E_STATE;
    if(header->error || header->count>32U || header->total<header->count ||
       header->truncated!=header->total-header->count)return failure(p,RADAR_SPATIAL_E_NUMERIC);
    for(i=0;i<header->count;++i) {
        const RadarDspCandidate *c=&candidates[i];float power,noise;unsigned j;
        RadarSpatialPoint *point=&p->frame.points[i];
        if(c->range<RADAR_RANGE_FIRST_BIN || c->range>RADAR_RANGE_LAST_BIN || c->doppler>=32U ||
           !c->noise_count || c->noise_count>216U || !c->power)
            return failure(p,RADAR_SPATIAL_E_NUMERIC);
        if(backend==PL_BASELINE && i && (c->power>candidates[i-1U].power ||
           (c->power==candidates[i-1U].power && (c->range<candidates[i-1U].range ||
           (c->range==candidates[i-1U].range && c->doppler<=candidates[i-1U].doppler)))))
            return failure(p,RADAR_SPATIAL_E_SEQUENCE);
        for(j=0;j<i;++j)if(c->range==candidates[j].range && c->doppler==candidates[j].doppler)
            return failure(p,RADAR_SPATIAL_E_SEQUENCE);
        power=(float)((double)c->power*scale);
        noise=(float)(((double)c->noise_sum/(double)c->noise_count)*scale);
        if(noise<NOISE_FLOOR)noise=NOISE_FLOOR;
        if(!isfinite(power) || !isfinite(noise))return failure(p,RADAR_SPATIAL_E_NUMERIC);
        /* Enhanced ranking is score-based, while these quantities must remain
         * current raw power/noise. Preserve engine order rather than rerank. */
        memset(point,0,sizeof *point);point->range_bin=c->range;point->doppler_bin=signed_bin(c->doppler);
        point->range_m=(float)((double)c->range*RADAR_RANGE_BIN_METERS)-(float)p->calibration.range_bias_m;
        point->velocity_mps=(float)((double)point->doppler_bin*RADAR_SPATIAL_VELOCITY_BIN_MPS);
        point->snr_db=10.0f*log10f(power/noise);point->power_dbfs=power_db(power);
        point->azimuth_deg=point->elevation_deg=point->horizontal_angle_deg=NAN;
        point->x_m=point->y_m=point->z_m=NAN;p->candidate_power[i]=power;++p->frame.count;
    }
    p->frame.candidate_count=header->total;p->frame.truncated_count=header->truncated;
    p->cursor=0U;p->pl_import=1U;p->stage=PL_IMPORT;return RADAR_SPATIAL_BUSY;
}
int radar_spatial_import_pl_row(RadarSpatialProcessor *p,uint32_t r,
    const int32_t *words,const uint64_t *power)
{
    unsigned c,k;
    const double cube_scale=(256.0/127.5)*32.0/4194304.0;
    const double rd_scale=(256.0/127.5)*(256.0/127.5)*(32.0/15.5)*(32.0/15.5)/(17592186044416.0*12.0);
    if(!p || !words || !power)return RADAR_SPATIAL_E_ARGUMENT;
    if(p->initialized!=SPATIAL_MAGIC || p->stage!=PL_IMPORT || r!=p->cursor || r>=256U)
        return RADAR_SPATIAL_E_SEQUENCE;
    for(c=0;c<12U;++c)for(k=0;k<32U;++k) {
        unsigned n=(c*32U+k)*2U;RadarComplex x;
        if(words[n]<-8388608 || words[n]>8388607 || words[n+1U]<-8388608 || words[n+1U]>8388607)
            return failure(p,RADAR_SPATIAL_E_NUMERIC);
        x.re=(float)((double)words[n]*cube_scale);x.im=(float)((double)words[n+1U]*cube_scale);
        x=multiply(x,p->combined[c][k]);
        if(!isfinite(x.re)||!isfinite(x.im))return failure(p,RADAR_SPATIAL_E_NUMERIC);
        p->cube[r][c][k]=x;
    }
    for(k=0;k<32U;++k) {
        p->rd_power[r][k]=(float)((double)power[k]*rd_scale);
        if(!isfinite(p->rd_power[r][k]))return failure(p,RADAR_SPATIAL_E_NUMERIC);
    }
    if(++p->cursor==256U) { p->received_ffts=384U;p->cursor=RADAR_RANGE_FIRST_BIN;p->stage=HEATMAP;return RADAR_SPATIAL_DONE; }
    return RADAR_SPATIAL_BUSY;
}

static int cfar_one(RadarSpatialProcessor *p)
{
    unsigned r=p->cursor,k;
    for(k=0U;k<32U;++k) {
        double sum=0.0;
        unsigned count=0U;
        float power=p->rd_power[r][k],noise;
        int dr,dk;
        if(!isfinite(power)) return failure(p,RADAR_SPATIAL_E_NUMERIC);
        if(power<=CFAR_FACTOR*NOISE_FLOOR || !local_maximum(p,r,k,power)) continue;
        for(dr=-10;dr<=10;++dr) {
            int rr=(int)r+dr;
            if(rr<(int)RADAR_RANGE_FIRST_BIN || rr>(int)RADAR_RANGE_LAST_BIN) continue;
            for(dk=-5;dk<=5;++dk) {
                unsigned kk=(unsigned)((int)k+dk+32)%32U;
                if(dr>=-2 && dr<=2 && dk>=-1 && dk<=1) continue;
                sum+=p->rd_power[rr][kk]; ++count;
            }
        }
        if(!count || !isfinite(sum)) return failure(p,RADAR_SPATIAL_E_NUMERIC);
        noise=(float)(sum/(double)count);
        if(noise<NOISE_FLOOR) noise=NOISE_FLOOR;
        if(power>CFAR_FACTOR*noise) add_candidate(p,r,k,power,noise);
    }
    if(++p->cursor>RADAR_RANGE_LAST_BIN) {
        p->frame.truncated_count=p->frame.candidate_count-p->frame.count;
        if(!p->frame.count) { p->frame.valid=1U;p->stage=COMPLETE;return RADAR_SPATIAL_DONE; }
        p->stage=ANGLES;p->point_index=0U;prepare_angle(p);
    }
    return RADAR_SPATIAL_BUSY;
}

static int finish_point(RadarSpatialProcessor *p)
{
    RadarSpatialPoint *point=&p->frame.points[p->point_index];
    float quality=p->angle_energy>0.0f ? p->best_angle_power/(12.0f*p->angle_energy) : 0.0f;
    int doppler_edge=point->doppler_bin>=15 || point->doppler_bin<=-15;
    int az_edge=p->best_azimuth==-60 || p->best_azimuth==60;
    int el_edge=p->best_elevation==-15 || p->best_elevation==15;
    if(!isfinite(quality) || quality<0.0f || quality>1.0001f)
        return failure(p,RADAR_SPATIAL_E_NUMERIC);
    point->coherence=quality>1.0f ? 1.0f : quality;
    if(doppler_edge) point->flags|=RADAR_POINT_DOPPLER_EDGE;
    if(quality<0.5f) point->flags|=RADAR_POINT_LOW_COHERENCE;
    if(az_edge || el_edge) point->flags|=RADAR_POINT_ANGLE_EDGE;
    if(!doppler_edge && quality>=0.5f) {
        if(!az_edge) { point->flags|=RADAR_POINT_AZ_VALID;point->azimuth_deg=(float)p->best_azimuth; }
        if(!el_edge) { point->flags|=RADAR_POINT_EL_VALID;point->elevation_deg=(float)p->best_elevation; }
        if(!az_edge && !el_edge) {
            double az=(double)p->best_azimuth*PI_D/180.0;
            double el=(double)p->best_elevation*PI_D/180.0;
            double u=sin(az)*cos(el);
            point->horizontal_angle_deg=(float)(asin(u)*180.0/PI_D);
            if(isfinite(point->range_m) && point->range_m>0.0f) {
                point->x_m=(float)((double)point->range_m*u);
                point->y_m=(float)((double)point->range_m*cos(az)*cos(el));
                point->z_m=(float)((double)point->range_m*sin(el));
                point->flags|=RADAR_POINT_XYZ_VALID; ++p->frame.xyz_count;
            }
        }
    }
    /* A failed elevation estimate is not a zero-height target. Independently
     * test the horizontal subarray for a projected bearing, with no XY/Z
     * position claim. The accepted 3D solution and old overview stay intact. */
    if(!doppler_edge && !(point->flags&RADAR_POINT_XYZ_VALID)) {
        unsigned a,c,k=(unsigned)(point->doppler_bin+32)%32U,best=0U;
        float energy=0.0f,peak=-1.0f;
        for(c=0;c<8U;++c)energy+=magnitude2(p->cube[point->range_bin][horizontal_channels[c]][k]);
        for(a=0;a<RADAR_SPATIAL_MAP_ANGLES;++a) {
            RadarComplex sum={0.0f,0.0f},w={1.0f,0.0f};float power;
            for(c=0;c<8U;++c) {
                RadarComplex v=multiply(p->cube[point->range_bin][horizontal_channels[c]][k],w);
                sum.re+=v.re;sum.im+=v.im;w=multiply(w,p->horizontal_root[a]);
            }
            power=magnitude2(sum);if(power>peak){peak=power;best=a;}
        }
        if(energy>0.0f && isfinite(peak) && peak/(8.0f*energy)>=0.5f && best>0U && best<120U) {
            point->horizontal_angle_deg=(float)best-60.0f;
            point->flags|=RADAR_POINT_BEARING_VALID;
        }
    }
    return RADAR_SPATIAL_BUSY;
}

static int angle_one(RadarSpatialProcessor *p)
{
    RadarSpatialPoint *point=&p->frame.points[p->point_index];
    unsigned a=p->angle_index,e,c,k=(unsigned)(point->doppler_bin+32)%32U;
    for(e=0U;e<RADAR_SPATIAL_ELEVATIONS;++e) {
        RadarComplex powers[8],sum={0.0f,0.0f};
        float power;
        powers[0].re=1.0f;powers[0].im=0.0f;
        if(!p->weights)for(c=1U;c<8U;++c) powers[c]=multiply(powers[c-1U],p->steering_x[e][a]);
        for(c=0U;c<12U;++c) {
            RadarComplex w,v;
            if(p->weights)w=p->weights->angle[a][e][c];
            else { w=powers[minus_x[c]];if(c<4U || c>=8U) w=multiply(w,p->steering_z[e]); }
            v=multiply(p->cube[point->range_bin][c][k],w);
            sum.re+=v.re;sum.im+=v.im;
        }
        power=magnitude2(sum);
        if(!isfinite(power)) return failure(p,RADAR_SPATIAL_E_NUMERIC);
        if(power>p->best_angle_power) {
            p->best_angle_power=power;
            p->best_azimuth=(int32_t)a-60;p->best_elevation=(int32_t)e-15;
        }
    }
    if(++p->angle_index==RADAR_SPATIAL_MAP_ANGLES) {
        if(finish_point(p)<0) return p->error;
        if(++p->point_index==p->frame.count) {
            p->frame.valid=1U;p->stage=COMPLETE;return RADAR_SPATIAL_DONE;
        }
        prepare_angle(p);
    }
    return RADAR_SPATIAL_BUSY;
}

int radar_spatial_step(RadarSpatialProcessor *p)
{
    if(!p) return RADAR_SPATIAL_E_ARGUMENT;
    if(p->initialized!=SPATIAL_MAGIC) return RADAR_SPATIAL_E_STATE;
    switch(p->stage) {
    case DOPPLER:return doppler_one(p);
    case HEATMAP:return heatmap_one(p);
    case CFAR:return cfar_one(p);
    case ANGLES:return angle_one(p);
    case COMPLETE:return RADAR_SPATIAL_DONE;
    case FAILED:return p->error;
    case COLLECT:return failure(p,RADAR_SPATIAL_E_INCOMPLETE);
    default:return RADAR_SPATIAL_E_STATE;
    }
}
void radar_spatial_cancel(RadarSpatialProcessor *p)
{
    if(!p || p->initialized!=SPATIAL_MAGIC) return;
    p->stage=IDLE;p->frame.valid=0U;p->frame.error=0;p->error=0;p->received_ffts=0U;
}
const RadarSpatialFrame *radar_spatial_result(const RadarSpatialProcessor *p)
{
    return p && p->initialized==SPATIAL_MAGIC && p->stage==COMPLETE && p->frame.valid ? &p->frame : NULL;
}
const char *radar_spatial_error_name(int error)
{
    switch(error) {
    case RADAR_SPATIAL_BUSY:return "BUSY";
    case RADAR_SPATIAL_DONE:return "DONE";
    case RADAR_SPATIAL_E_ARGUMENT:return "INVALID_ARGUMENT";
    case RADAR_SPATIAL_E_STATE:return "PROCESSOR_STATE";
    case RADAR_SPATIAL_E_CALIBRATION:return "CALIBRATION_IDENTITY_OR_NUMERIC";
    case RADAR_SPATIAL_E_SEQUENCE:return "FFT_ORDER_OR_DUPLICATE";
    case RADAR_SPATIAL_E_NUMERIC:return "NONFINITE_SPATIAL_VALUE";
    case RADAR_SPATIAL_E_INCOMPLETE:return "INCOMPLETE_FFT_INPUT";
    case RADAR_SPATIAL_E_TEMPORAL_INPUT:return "UNTRUSTED_TEMPORAL_INPUT";
    default:return "UNKNOWN";
    }
}
