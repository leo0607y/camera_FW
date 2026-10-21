#pragma once
#include <stdbool.h>
#include <math.h>

typedef struct { float rgb[3], luma; } frame_quality;
typedef struct { frame_quality previous; unsigned stable, frames; bool has_previous; } quality_gate;

// 時間の経過と安定したフレームの連続の両方が必要。タイムアウトは呼び出し側で扱う
static inline bool quality_gate_check(quality_gate *gate, const frame_quality *q,
                                       unsigned elapsed_ms, unsigned minimum_ms,
                                       unsigned minimum_frames, unsigned stable_frames) {
    float previous_total=0,total=0,colour_delta=0;
    for(int c=0;c<3;c++){previous_total+=gate->previous.rgb[c];total+=q->rgb[c];}
    if(gate->has_previous){
        for(int c=0;c<3;c++)colour_delta+=fabsf(q->rgb[c]/fmaxf(1,total)-gate->previous.rgb[c]/fmaxf(1,previous_total));
        bool stable=fabsf(q->luma-gate->previous.luma)<=fmaxf(3,q->luma*.03f)&&colour_delta<=.025f;
        gate->stable=stable?gate->stable+1:0;
    }
    gate->previous=*q;gate->has_previous=true;gate->frames++;
    return elapsed_ms>=minimum_ms && gate->frames>=minimum_frames && gate->stable>=stable_frames;
}
static inline bool quality_gate_update(quality_gate *gate, const frame_quality *q,
                                       unsigned elapsed_ms, unsigned minimum_ms) {
    return quality_gate_check(gate,q,elapsed_ms,minimum_ms,12,3);
}
