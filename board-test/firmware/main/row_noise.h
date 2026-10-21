#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
// OV2640は一部の行に横幅全体のオフセットを足す（暗部の青・オレンジの縞）
// トーンカーブの前に足されるので、リニアな光量で推定して引く
// 横16区間の平均を、上下48行(自分を含む97行)の中央値と比べる。縞は8行から数十行の帯になるので、
// 平均と比べると帯の隣の行が逆向きに引っ張られる。中央値なら帯にも全幅の段差・なだらかな明暗にも引っ張られない
// 区間の半分以上がそろってずれている行だけ補正する（半分より短い本物の横線は残る）
#define ROW_NOISE_RADIUS 48
#define ROW_NOISE_SEGMENTS 16
#define ROW_NOISE_SUPPORT 8 // この数の区間が同じ向きにずれていないと補正しない
#define ROW_NOISE_STEP 4 // pixel stride for the statistics (25 samples per segment at 1600 px)
static inline float row_noise_median(float *v,unsigned n){
    unsigned lo=0,hi=n-1,k=n/2;
    while(lo<hi){
        float pivot=v[(lo+hi)/2];unsigned i=lo,j=hi;
        while(i<=j){while(v[i]<pivot)i++;while(v[j]>pivot)j--;if(i<=j){float t=v[i];v[i]=v[j];v[j]=t;i++;if(j)j--;else break;}}
        if(k<=j)hi=j;else if(k>=i)lo=i;else break;
    }
    return v[k];
}
// 出力レベルごとのリニア光量(0..1)。この基板のOV2640で実測(2026-10-21)
// ゲイン1倍・WB固定で露光を4〜1200まで振り、Debevec & Malik法で緑チャンネルから求めた
// 黒は5くらい、80くらいまでほぼ直線。row_noise_row_mapのため単調増加にしておく
// ROW_NOISE_GAMMA=2.2f でただのべき乗にできる（ホストでの比較用）
static const float row_noise_curve[256]={
    0.000000f, 0.000001f, 0.000003f, 0.000004f, 0.000005f, 0.000007f, 0.001402f, 0.003081f,
    0.004760f, 0.006439f, 0.008118f, 0.009797f, 0.010079f, 0.011846f, 0.013714f, 0.015662f,
    0.017664f, 0.019699f, 0.021744f, 0.023781f, 0.025792f, 0.027765f, 0.029687f, 0.031553f,
    0.033357f, 0.035096f, 0.036770f, 0.038382f, 0.039933f, 0.041429f, 0.042875f, 0.044275f,
    0.045637f, 0.046967f, 0.048270f, 0.049554f, 0.050824f, 0.052085f, 0.053344f, 0.054605f,
    0.055873f, 0.057151f, 0.058444f, 0.059755f, 0.061087f, 0.062442f, 0.063822f, 0.065228f,
    0.066663f, 0.068126f, 0.069619f, 0.071141f, 0.072693f, 0.074273f, 0.075881f, 0.077517f,
    0.079178f, 0.080864f, 0.082572f, 0.084301f, 0.086049f, 0.087814f, 0.089593f, 0.091384f,
    0.093185f, 0.094993f, 0.096807f, 0.098623f, 0.100439f, 0.102253f, 0.104064f, 0.105868f,
    0.107665f, 0.109451f, 0.111227f, 0.112991f, 0.114741f, 0.116476f, 0.118197f, 0.119903f,
    0.121593f, 0.123268f, 0.124928f, 0.126573f, 0.128206f, 0.129825f, 0.131434f, 0.133034f,
    0.134625f, 0.136211f, 0.137794f, 0.139375f, 0.140957f, 0.142544f, 0.144137f, 0.145739f,
    0.147354f, 0.148985f, 0.150634f, 0.152305f, 0.154001f, 0.155726f, 0.157482f, 0.159273f,
    0.161103f, 0.162974f, 0.164890f, 0.166854f, 0.168869f, 0.170937f, 0.173063f, 0.175249f,
    0.177498f, 0.179812f, 0.182194f, 0.184646f, 0.187172f, 0.189772f, 0.192448f, 0.195204f,
    0.198040f, 0.200957f, 0.203958f, 0.207042f, 0.210212f, 0.213466f, 0.216807f, 0.220233f,
    0.223746f, 0.227343f, 0.231025f, 0.234791f, 0.238640f, 0.242569f, 0.246578f, 0.250664f,
    0.254825f, 0.259058f, 0.263361f, 0.267730f, 0.272162f, 0.276654f, 0.281201f, 0.285799f,
    0.290446f, 0.295135f, 0.299862f, 0.304624f, 0.309415f, 0.314231f, 0.319067f, 0.323919f,
    0.328781f, 0.333649f, 0.338519f, 0.343387f, 0.348249f, 0.353101f, 0.357939f, 0.362761f,
    0.367564f, 0.372345f, 0.377102f, 0.381835f, 0.386542f, 0.391223f, 0.395877f, 0.400506f,
    0.405110f, 0.409691f, 0.414251f, 0.418793f, 0.423321f, 0.427838f, 0.432349f, 0.436859f,
    0.441372f, 0.445896f, 0.450436f, 0.454998f, 0.459591f, 0.464221f, 0.468896f, 0.473624f,
    0.478413f, 0.483272f, 0.488208f, 0.493230f, 0.498347f, 0.503566f, 0.508896f, 0.514344f,
    0.519917f, 0.525624f, 0.531470f, 0.537461f, 0.543603f, 0.549901f, 0.556358f, 0.562977f,
    0.569761f, 0.576709f, 0.583822f, 0.591098f, 0.598534f, 0.606125f, 0.613866f, 0.621748f,
    0.629763f, 0.637899f, 0.646143f, 0.654481f, 0.662897f, 0.671372f, 0.679887f, 0.688421f,
    0.696952f, 0.705456f, 0.713909f, 0.722286f, 0.730562f, 0.738713f, 0.746714f, 0.754544f,
    0.762182f, 0.769611f, 0.776816f, 0.783789f, 0.790525f, 0.797026f, 0.803302f, 0.809372f,
    0.815264f, 0.821016f, 0.826681f, 0.832323f, 0.838026f, 0.843886f, 0.850023f, 0.856576f,
    0.863712f, 0.871623f, 0.880536f, 0.890716f, 0.902470f, 0.916162f, 0.924545f, 0.932929f,
    0.941313f, 0.949697f, 0.958081f, 0.966465f, 0.974848f, 0.983232f, 0.991616f, 1.000000f,
};
static inline float row_noise_linear(int v){
#if defined(ROW_NOISE_LINEAR)
    return ROW_NOISE_LINEAR(v); // ホストで別のカーブを試す用
#elif defined(ROW_NOISE_GAMMA)
    return powf(v/255.f,ROW_NOISE_GAMMA);
#else
    return row_noise_curve[v];
#endif
}
static inline void row_noise_lut(uint16_t to_linear[256]){
    for(int i=0;i<256;i++)to_linear[i]=(uint16_t)(row_noise_linear(i)*4095.f+.5f);
}
// seg: height*SEGMENTS*3（区間ごとのリニア光量の平均）, line: width*3 bytes。各行は1回だけ読む
typedef void (*row_noise_loader)(const void *image,unsigned y,uint8_t *line); // fills width*3 RGB bytes
static inline void row_noise_measure(row_noise_loader load,const void *image,unsigned width,unsigned height,const uint16_t *lin,float *seg,uint8_t *line){
    const unsigned span=width/ROW_NOISE_SEGMENTS;
    for(unsigned y=0;y<height;y++){
        load(image,y,line);
        for(unsigned k=0;k<ROW_NOISE_SEGMENTS;k++){
            uint32_t sum[3]={0};unsigned n=0;
            for(unsigned x=(k*span+ROW_NOISE_STEP-1)/ROW_NOISE_STEP*ROW_NOISE_STEP;x<(k+1)*span;x+=ROW_NOISE_STEP,n++)
                for(unsigned c=0;c<3;c++)sum[c]+=lin[line[x*3+c]];
            for(unsigned c=0;c<3;c++)seg[((size_t)y*ROW_NOISE_SEGMENTS+k)*3+c]=(float)sum[c]/n;
        }
    }
}
// OV2640は縞が乗ったあとにレンズ補正（周辺ほどゲインを上げる）をかけるので、縞も周辺ほど強い（中央の2倍くらい）
// 形は 1+GAIN*r^2（rは中心からの距離を横幅の半分で割ったもの）。この基板で実測(2026-10-21)。
// レンズ補正(CTRL1 bit1)を切ると縞は横に平らになり、当てはめもGAIN=0になる
#define ROW_NOISE_LENS_GAIN 1.7f
#define ROW_NOISE_LENS_CX 0.55f // 180度回転したあとの画像での中心
#define ROW_NOISE_LENS_CY 0.5f
static inline float row_noise_lens_row(unsigned y,unsigned width,unsigned height){
    float d=((float)y-ROW_NOISE_LENS_CY*height)/(width*.5f);return 1+ROW_NOISE_LENS_GAIN*d*d;
}
static inline float row_noise_lens(float x,unsigned y,unsigned width,unsigned height){
    float d=(x-ROW_NOISE_LENS_CX*width)/(width*.5f);return row_noise_lens_row(y,width,height)+ROW_NOISE_LENS_GAIN*d*d;
}
// offset: height*3。行ごとの縞の量（レンズ補正のゲイン1のところでのリニア光量）。戻り値は最大の補正量
static inline float row_noise_solve(const float *seg,unsigned width,unsigned height,float *offset){
    const unsigned R=ROW_NOISE_RADIUS,span=width/ROW_NOISE_SEGMENTS;float window[2*ROW_NOISE_RADIUS+1],dev[ROW_NOISE_SEGMENTS],spread[ROW_NOISE_SEGMENTS],peak=0;
    for(unsigned y=0;y<height;y++){
        unsigned lo=y>R?y-R:0,hi=y+R<height?y+R:height-1;
        for(unsigned c=0;c<3;c++){
            for(unsigned k=0;k<ROW_NOISE_SEGMENTS;k++){
                for(unsigned j=lo;j<=hi;j++)window[j-lo]=seg[((size_t)j*ROW_NOISE_SEGMENTS+k)*3+c];
                dev[k]=(seg[((size_t)y*ROW_NOISE_SEGMENTS+k)*3+c]-row_noise_median(window,hi-lo+1))/row_noise_lens((k+.5f)*span,y,width,height);
                spread[k]=dev[k];
            }
            float o=row_noise_median(spread,ROW_NOISE_SEGMENTS);
            // SUPPORT番目に弱い区間の分までしか補正しない。半分以上の区間が平らなら本物の線なので0になる
            for(unsigned k=0;k<ROW_NOISE_SEGMENTS;k++){float a=dev[k]*(o<0?-1:1);spread[k]=a<0?0:a;}
            float support=0;for(int r=0;r<ROW_NOISE_SUPPORT;r++){unsigned m=0;for(unsigned k=1;k<ROW_NOISE_SEGMENTS;k++)if(spread[k]<spread[m])m=k;support=spread[m];spread[m]=INFINITY;}
            float limited=fabsf(o)<support?o:copysignf(support,o);
            offset[y*3+c]=limited;if(fabsf(limited)>peak)peak=fabsf(limited);
        }
    }
    return peak;
}
// 補正量が画素ごとに違うので、リニア光量(0..4095)から一番近いレベルを引く表を作っておく（powfを使わない）
static inline void row_noise_levels(float level[256]){for(int v=0;v<256;v++)level[v]=row_noise_linear(v)*4095.f;}
static inline void row_noise_inverse(const float level[256],uint8_t inverse[4096]){
    for(int t=0;t<4096;t++){
        int lo=0,hi=255;
        while(lo<hi){int mid=(lo+hi)/2;if(level[mid]<t)lo=mid+1;else hi=mid;}
        if(lo>0&&t-level[lo-1]<level[lo]-t)lo--;
        inverse[t]=lo;
    }
}
// 1行(RGB)を補正する。補正量の小さい色はそのまま。どの色も補正しなければfalse
static inline bool row_noise_correct_row(const float *offset,unsigned y,unsigned width,unsigned height,const float level[256],const uint8_t inverse[4096],uint8_t *row){
    const float *o=offset+y*3;bool on[3];
    for(int c=0;c<3;c++)on[c]=fabsf(o[c])>=.25f;
    if(!on[0]&&!on[1]&&!on[2])return false;
    const float base=row_noise_lens_row(y,width,height),cx=ROW_NOISE_LENS_CX*width,k=ROW_NOISE_LENS_GAIN/(width*.5f*width*.5f);
    for(unsigned x=0;x<width;x++,row+=3){
        float d=x-cx,g=base+k*d*d;
        for(int c=0;c<3;c++)if(on[c]){int t=(int)(level[row[c]]-o[c]*g+.5f);row[c]=inverse[t<0?0:t>4095?4095:t];}
    }
    return true;
}
static inline void row_noise_apply(uint8_t *rgb,unsigned width,unsigned height,const float *offset){
    static float level[256];static uint8_t inverse[4096];row_noise_levels(level);row_noise_inverse(level,inverse);
    for(unsigned y=0;y<height;y++)row_noise_correct_row(offset,y,width,height,level,inverse,rgb+(size_t)y*width*3);
}
