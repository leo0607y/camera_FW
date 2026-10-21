#include <assert.h>
#include <stdio.h>
enum{FRAMESIZE_UXGA,FRAMESIZE_SVGA,FRAMESIZE_VGA};
#include "main/camera_modes.h"
int main(void){
    for(unsigned i=0;i<8;i++)assert(mode_from_adc(i*256+128,2048,0)==i);
    assert(mode_from_adc(4095,2048,7)==7);assert(mode_from_adc(0,2048,7)==0);
    assert(mode_from_adc(257,2048,0)==0);assert(mode_from_adc(289,2048,0)==1);
    assert(mode_from_adc(250,2048,1)==1);assert(mode_from_adc(223,2048,1)==0);
    assert(!camera_modes[5].video&&camera_modes[6].video&&!camera_modes[7].video);
    assert(camera_modes[2].burst==5&&!camera_modes[2].video&&camera_modes[3].timer_s==10&&!camera_modes[3].video);
    assert(camera_modes[4].video&&camera_modes[4].timelapse&&!camera_modes[6].timelapse);
    // 色加工はアプリ側に移したので、センサーエフェクトを使うモードはない
    for(unsigned i=0;i<8;i++)assert(camera_modes[i].effect==0&&camera_modes[i].saturation==0);
    unsigned spike[5]={1024,1025,4095,1023,1024};assert(median_five(spike)==1024);
    mode_selector selector={0};selector_sample(&selector,1800);selector_begin(&selector,3);
    for(int i=0;i<100;i++){selector_sample(&selector,1800+(i%3)-1);assert(selector_choose(&selector,3,2048)==3);}
    unsigned selected=3;
    for(int i=0;i<9;i++){selector_sample(&selector,0);selected=selector_choose(&selector,selected,2048);assert(selected==3);}
    for(int i=0;i<100;i++){selector_sample(&selector,0);selected=selector_choose(&selector,selected,2048);}
    assert(selected==0);
    // 8つとも選べて、境界のノイズでは切り替わらない
    for(unsigned mode=0;mode<8;mode++){
        for(int i=0;i<100;i++){selector_sample(&selector,mode*256+128);selected=selector_choose(&selector,selected,2048);}
        assert(selected==mode);
    }
    selector_sample(&selector,512);selector_begin(&selector,2);
    for(int i=0;i<100;i++){selector_sample(&selector,512+(i%7)-3);assert(selector_choose(&selector,2,2048)==2);}
    puts("PASS: 8 ADC bins, endpoints, hysteresis, still/video mapping, burst/timer/time-lapse");
}
