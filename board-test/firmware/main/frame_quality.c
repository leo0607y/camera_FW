#include "frame_quality.h"
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

esp_err_t measure_frame(camera_fb_t *frame, frame_quality *quality){
    if(!frame||!frame->buf||frame->len<1024||frame->width<160||frame->height<120||frame->width>1600||frame->height>1200||
       frame->buf[0]!=0xff||frame->buf[1]!=0xd8||frame->buf[frame->len-2]!=0xff||frame->buf[frame->len-1]!=0xd9)
        return ESP_ERR_INVALID_RESPONSE;
    const size_t capacity=((frame->width+7)/8)*((frame->height+7)/8)*3;
    uint8_t *rgb=heap_caps_malloc(capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!rgb)return ESP_ERR_NO_MEM;
    esp_jpeg_image_cfg_t config={.indata=frame->buf,.indata_size=frame->len,
        .outbuf=rgb,.outbuf_size=capacity,.out_format=JPEG_IMAGE_FORMAT_RGB888,.out_scale=JPEG_IMAGE_SCALE_1_8};
    esp_jpeg_image_output_t info={0};esp_err_t e=esp_jpeg_decode(&config,&info);
    if(e==ESP_OK&&info.output_len>0&&info.output_len<=capacity&&info.output_len%3==0){
        uint32_t sums[3]={0};
        for(size_t i=0;i<info.output_len;i++)sums[i%3]+=rgb[i];
        for(int c=0;c<3;c++)quality->rgb[c]=(float)sums[c]/(info.output_len/3);
        quality->luma=.2126f*quality->rgb[0]+.7152f*quality->rgb[1]+.0722f*quality->rgb[2];
    }else if(e==ESP_OK)e=ESP_ERR_INVALID_SIZE;
    free(rgb);return e;
}
