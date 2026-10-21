#pragma once
#include <stdint.h>
#include <stdbool.h>
// 色加工はスマホとPCでやる。基板は撮影だけ
typedef struct { const char *name; int size,quality,gain,saturation,contrast,effect; bool night,video,timelapse; unsigned burst,timer_s; } camera_mode;
// sizeは sensor.h の framesize_t
static const camera_mode camera_modes[8]={
    {"Standard",FRAMESIZE_UXGA,12,0,0,0,0,false,false,false,0,0},
    {"Fine",FRAMESIZE_UXGA,8,0,0,0,0,false,false,false,0,0},
    {"Burst",FRAMESIZE_UXGA,12,0,0,0,0,false,false,false,5,0},
    {"Self-timer",FRAMESIZE_UXGA,12,0,0,0,0,false,false,false,0,10},
    {"Time-lapse",FRAMESIZE_UXGA,12,0,0,0,0,false,true,true,0,0},
    {"Low light",FRAMESIZE_SVGA,12,2,0,0,0,true,false,false,0,0},
    {"Video",FRAMESIZE_VGA,16,1,0,0,0,false,true,false,0,0},
    {"Phone transfer",FRAMESIZE_SVGA,12,0,0,0,0,false,false,false,0,0}
};
// 実際のADC範囲0〜2048を8分割、ヒステリシスあり
static inline unsigned mode_from_adc(unsigned raw,unsigned maximum,unsigned previous){
    if(maximum<256)maximum=2048;
    unsigned bin=(raw*8)/(maximum+1);if(bin>7)bin=7;
    if(bin>previous&&previous<7&&raw<((previous+1)*(maximum+1))/8+32)return previous;
    if(bin<previous&&previous>0&&raw+32>previous*(maximum+1)/8)return previous;
    return bin;
}

typedef struct {unsigned filtered,anchor,candidate,stable;bool initialized,armed;} mode_selector;
static inline unsigned median_five(unsigned values[5]){
    for(int i=1;i<5;i++){unsigned v=values[i];int j=i;while(j&&values[j-1]>v){values[j]=values[j-1];j--;}values[j]=v;}
    return values[2];
}
static inline void selector_sample(mode_selector *s,unsigned value){
    s->filtered=s->initialized?(s->filtered*3+value+2)/4:value;s->initialized=true;
}
static inline void selector_begin(mode_selector *s,unsigned current){
    s->anchor=s->filtered;s->candidate=current;s->stable=0;s->armed=false;
}
static inline unsigned selector_choose(mode_selector *s,unsigned current,unsigned maximum){
    // 選択に入っただけでは変えない。ノブが動いてから
    unsigned movement=s->filtered>s->anchor?s->filtered-s->anchor:s->anchor-s->filtered;
    if(!s->armed){if(movement<48)return current;s->armed=true;}
    unsigned candidate=mode_from_adc(s->filtered,maximum,current);
    if(candidate==current){s->candidate=current;s->stable=0;return current;}
    if(s->candidate!=candidate){s->candidate=candidate;s->stable=1;}else{s->stable++;}
    if(s->stable<10)return current; // 20ms x 10回。境界付近の一瞬の揺れは無視
    s->stable=0;return candidate;
}
