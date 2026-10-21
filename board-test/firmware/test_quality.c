#include <assert.h>
#include <stdio.h>
#include "main/quality_policy.h"
int main(void){
    quality_gate gate={0};frame_quality dark={{35,40,20},37},normal={{110,105,100},106};
    for(unsigned i=0;i<11;i++)assert(!quality_gate_update(&gate,&dark,i*160,4000));
    assert(!quality_gate_update(&gate,&normal,4200,4000)); // changed exposure
    assert(!quality_gate_update(&gate,&normal,4400,4000));
    assert(!quality_gate_update(&gate,&normal,4600,4000));
    assert(quality_gate_update(&gate,&normal,4800,4000));
    quality_gate fast={0};for(unsigned i=0;i<30;i++)assert(!quality_gate_update(&fast,&normal,i*100,4000));
    frame_quality green={{60,180,30},145};assert(!quality_gate_update(&gate,&green,5000,4000));
    quality_gate ready={.previous=normal,.has_previous=true};
    assert(quality_gate_check(&ready,&normal,0,0,1,1));
    quality_gate changed={.previous=normal,.has_previous=true};
    assert(!quality_gate_check(&changed,&green,0,0,1,1));
    assert(!quality_gate_check(&changed,&green,400,0,3,2));
    assert(quality_gate_check(&changed,&green,800,0,3,2));
    puts("PASS: minimum warmup, frame minimum, exposure shift, colour shift, stability gate");
}
