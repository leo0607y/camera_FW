#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main/row_noise.h"
static unsigned W=320,H=240;
static void load_rgb(const void *image,unsigned y,uint8_t *line){memcpy(line,(const uint8_t*)image+(size_t)y*W*3,W*3);}
// PPMを入れると縞除去したPPMを出す（PC版との比較用）。引数なしなら自己テスト
int main(int argc,char **argv){
    uint8_t *rgb;
    if(argc==3){FILE *f=fopen(argv[1],"rb");int max;assert(fscanf(f,"P6 %u %u %d",&W,&H,&max)==3);fgetc(f);
        rgb=malloc(W*H*3);assert(fread(rgb,1,W*H*3,f)==W*H*3);fclose(f);}
    else{rgb=malloc(W*H*3);float level[256];static uint8_t inverse[4096];row_noise_levels(level);row_noise_inverse(level,inverse);
        for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++)for(int c=0;c<3;c++){ // センサーと同じカーブで作る
            float l=level[y>120&&x<W*3/4?160:60]+(y%7==3&&c==2?150*row_noise_lens(x,y,W,H):0); // 3/4幅の横線 + 青の縞（周辺ほど強い）
            rgb[(y*W+x)*3+c]=inverse[l>4095?4095:(int)lrintf(l)];}}
    uint16_t lin[256];row_noise_lut(lin);
    float *seg=malloc(H*ROW_NOISE_SEGMENTS*3*sizeof(float)),*off=malloc(H*3*sizeof(float));uint8_t *line=malloc(W*3);
    row_noise_measure(load_rgb,rgb,W,H,lin,seg,line);float peak=row_noise_solve(seg,W,H,off);
    row_noise_apply(rgb,W,H,off);
    if(argc==3){FILE *f=fopen(argv[2],"wb");fprintf(f,"P6 %u %u 255\n",W,H);fwrite(rgb,1,W*H*3,f);fclose(f);printf("peak %.1f",peak);int b[5]={0};float t[5]={10,20,40,80,160};for(unsigned i=0;i<H*3;i++)for(int k=0;k<5;k++)b[k]+=fabsf(off[i])>t[k];printf(" >10:%d >20:%d >40:%d >80:%d >160:%d\n",b[0],b[1],b[2],b[3],b[4]);return 0;}
    for(unsigned y=0;y<H;y++)for(unsigned x=10;x<W;x+=W-20){if(y+ROW_NOISE_RADIUS+1>=120&&y<=121+ROW_NOISE_RADIUS)continue;int b=rgb[(y*W+x)*3+2],g=rgb[(y*W+x)*3+1];assert(abs(b-g)<=5);} // stripes removed away from the edge
    assert(rgb[(119*W+10)*3+1]<70&&rgb[(125*W+10)*3+1]>150); // 本物の線は残る
    unsigned char flat[256];for(int v=0;v<256;v++)flat[v]=v;
    float zero[3]={0};uint8_t one[3*256];for(int v=0;v<256;v++)one[v*3]=one[v*3+1]=one[v*3+2]=v;
    row_noise_apply(one,256,1,zero);for(int v=0;v<256;v++)assert(one[v*3]==flat[v]); // 補正なしの行はそのまま
    puts("PASS: stripes removed, horizontal edge kept, zero offset is lossless");
}
