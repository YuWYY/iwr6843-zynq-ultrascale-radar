/* Simulation-only bridge to AMD's supplied bit-accurate FFT C model.
 * No transform arithmetic is implemented here. The independent vendor HDL
 * remains the source of truth for cycle/handshake/event verification. */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <windows.h>
#define NT 1
#include "xfft_v9_1_bitacc_cmodel.h"

static struct xilinx_ip_xfft_v9_1_state *states[2];
static double real_in[2][256], imag_in[2][256], real_out[2][256], imag_out[2][256];
typedef struct xilinx_ip_xfft_v9_1_state *(*create_fn)(struct xilinx_ip_xfft_v9_1_generics);
typedef int (*simulate_fn)(struct xilinx_ip_xfft_v9_1_state *,struct xilinx_ip_xfft_v9_1_inputs,struct xilinx_ip_xfft_v9_1_outputs *);
static create_fn vendor_create;
static simulate_fn vendor_simulate;
#ifdef __cplusplus
extern "C" {
#endif
void radar_vendor_fft_put(int n, int index, int re, int im) {
    int c = n == 256 ? 0 : 1;
    real_in[c][index] = re / 8388608.0;
    imag_in[c][index] = im / 8388608.0;
}
int radar_vendor_fft_run(int n, int schedule, int direction) {
    int c = n == 256 ? 0 : 1, lg = n == 256 ? 8 : 5, j, scales[8];
    struct xilinx_ip_xfft_v9_1_generics gen = {0};
    struct xilinx_ip_xfft_v9_1_inputs in = {0};
    struct xilinx_ip_xfft_v9_1_outputs out = {0};
    if (n != 256 && n != 32) return -100;
    if (!vendor_create) {
        char path[2048], directory[2048]; HMODULE dll; int end=0;
        if (!GetEnvironmentVariableA("RADAR_XFFT_CMODEL_DLL",path,sizeof(path)))return -103;
        for(j=0;path[j];j++){if(path[j]=='/')path[j]='\\';directory[j]=path[j];if(path[j]=='\\')end=j;}
        directory[end]=0;SetDllDirectoryA(directory);
        dll=LoadLibraryExA(path,0,LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll){fprintf(stderr,"AMD DLL load error %lu path=%s\n",(unsigned long)GetLastError(),path);return -104;}
        vendor_create=(create_fn)GetProcAddress(dll,"xilinx_ip_xfft_v9_1_create_state");
        vendor_simulate=(simulate_fn)GetProcAddress(dll,"xilinx_ip_xfft_v9_1_bitacc_simulate");
        if(!vendor_create||!vendor_simulate)return -105;
    }
    if (!states[c]) {
        gen.C_NFFT_MAX=lg;gen.C_ARCH=2;gen.C_INPUT_WIDTH=24;gen.C_TWIDDLE_WIDTH=24;
        gen.C_HAS_SCALING=1;gen.C_HAS_ROUNDING=1;gen.C_NSSR=1;
        states[c]=vendor_create(gen);
        if (!states[c]) return -101;
    }
    for(j=0;j<lg;j++)scales[j]=(schedule>>(2*j))&3;
    in.nfft=lg;in.xn_re=real_in[c];in.xn_re_size=n;in.xn_im=imag_in[c];in.xn_im_size=n;
    in.scaling_sch=scales;in.scaling_sch_size=lg;in.direction=direction;
    out.xk_re=real_out[c];out.xk_re_size=n;out.xk_im=imag_out[c];out.xk_im_size=n;
    if(vendor_simulate(states[c],in,&out))return -102;
    return out.overflow;
}
unsigned long long radar_vendor_fft_get(int n, int index) {
    int c=n==256?0:1;
    int64_t r=(int64_t)llround(real_out[c][index]*8388608.0);
    int64_t q=(int64_t)llround(imag_out[c][index]*8388608.0);
    return ((uint64_t)r&0xffffffu)|(((uint64_t)q&0xffffffu)<<24);
}
#ifdef __cplusplus
}
#endif
