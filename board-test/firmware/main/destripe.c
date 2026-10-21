#include "destripe.h"
#include "row_noise.h"
#include "jpeg_writer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>
#if CONFIG_JD_USE_ROM
#include "rom/tjpgd.h"
typedef UINT jd_size;typedef UINT jd_result;
#define DECODE_POOL 3100
#else
#include "tjpgd.h"
typedef size_t jd_size;typedef int jd_result;
#define DECODE_POOL (JD_FASTDECODE==2?65472:3100)
#endif

// 2コアで流れ作業にする（順番にやるのと結果は同じ）
//   this core:   decode JPEG -> YUV422 ...................... wait ... encode corrected RGB rows
//   other core:  measure row noise as rows arrive -> solve -> correct rows into a ring for the encoder
// YUV422で持つ（UXGAで3.84MB）。次の撮影用にPSRAMを残すため
// 元も出力も4:2:2なので色の解像度は落ちない
#define RGB_RING 32 // corrected rows waiting for the encoder

static inline uint8_t clamp8(int v){return v<0?0:v>255?255:v;}
// BT.601。往復で平均0.5以内
static inline void to_rgb(int y,int u,int v,uint8_t *rgb){
    int c=298*(y-16),d=u-128,e=v-128;
    rgb[0]=clamp8((c+409*e+128)>>8);rgb[1]=clamp8((c-100*d-208*e+128)>>8);rgb[2]=clamp8((c+516*d+128)>>8);
}
static inline uint8_t luma(const uint8_t *p){return clamp8(((66*p[0]+129*p[1]+25*p[2]+128)>>8)+16);}
static inline void chroma(const uint8_t *a,const uint8_t *b,uint8_t *u,uint8_t *v){
    int r=a[0]+b[0],g=a[1]+b[1],bl=a[2]+b[2];
    *u=clamp8(((-38*r-74*g+112*bl+256)>>9)+128);*v=clamp8(((112*r-94*g-18*bl+256)>>9)+128);
}

typedef struct {
    const uint8_t *jpeg;size_t len,read;unsigned width,height;
    uint8_t *yuv,*rgb_ring;
    float *seg,*offset;uint8_t *line;
    unsigned decoded,produced,consumed;bool abort; // progress, accessed with atomics
    TaskHandle_t main_task,helper_task;SemaphoreHandle_t done;
    float peak;int64_t measured_at;
} pipeline;
// 通知はindex 1（ドライバはindex 0）。古い通知が来ても条件を見直すので大丈夫
#define WAKE 1
static inline unsigned get(unsigned *v){return __atomic_load_n(v,__ATOMIC_ACQUIRE);}
static inline void put(unsigned *v,unsigned value,TaskHandle_t wake){__atomic_store_n(v,value,__ATOMIC_RELEASE);if(wake)xTaskNotifyGiveIndexed(wake,WAKE);}
static inline bool aborted(pipeline *p){return __atomic_load_n(&p->abort,__ATOMIC_ACQUIRE);}
static void stop(pipeline *p){__atomic_store_n(&p->abort,true,__ATOMIC_RELEASE);xTaskNotifyGiveIndexed(p->main_task,WAKE);xTaskNotifyGiveIndexed(p->helper_task,WAKE);}
#define WAIT_UNTIL(p,cond) while(!(cond)&&!aborted(p))ulTaskNotifyTakeIndexed(WAKE,pdTRUE,1)
// 優先度はidle+1。idleと同じだと半分の速度になるので、160行ごと(約0.5秒)に1tick休んでWDTを満たす
#define WORK_PRIORITY (tskIDLE_PRIORITY+1)
static inline void breathe(unsigned row){if(row%160==159)vTaskDelay(1);}

static jd_size decode_input(JDEC *jd,uint8_t *buf,jd_size n){
    pipeline *p=jd->device;if(n>p->len-p->read)n=p->len-p->read;
    if(buf)memcpy(buf,p->jpeg+p->read,n);
    p->read+=n;return n;
}
static jd_result decode_output(JDEC *jd,void *bitmap,JRECT *rect){
    pipeline *p=jd->device;const uint8_t *rgb=bitmap;unsigned w=rect->right-rect->left+1;
    if(((rect->left|w)&1)||aborted(p))return 0; // MCUは16px幅なので画素ペアがまたがらない
    for(unsigned y=rect->top;y<=rect->bottom;y++){
        uint8_t *out=p->yuv+((size_t)y*p->width+rect->left)*2;
        for(unsigned x=0;x<w;x+=2,rgb+=6,out+=4){out[0]=luma(rgb);out[2]=luma(rgb+3);chroma(rgb,rgb+3,&out[1],&out[3]);}
    }
    if(rect->right+1u==p->width){put(&p->decoded,rect->bottom+1u,p->helper_task);breathe(rect->bottom);} // 行のまとまりが完成
    return 1;
}
static void yuv_row(const pipeline *p,unsigned y,uint8_t *line){
    const uint8_t *s=p->yuv+(size_t)y*p->width*2;
    for(unsigned x=0;x<p->width;x+=2,s+=4,line+=6){to_rgb(s[0],s[1],s[3],line);to_rgb(s[2],s[1],s[3],line+3);}
}
static void load_decoded_row(const void *image,unsigned y,uint8_t *line){
    pipeline *p=(pipeline*)image;
    WAIT_UNTIL(p,get(&p->decoded)>y);
    yuv_row(p,y,line);breathe(y);
}
static float level[256];static uint8_t inverse[4096]; // destripe_initで1回だけ作る（両コアで読むだけ）
static void help(pipeline *p){
    uint16_t lin[256];row_noise_lut(lin);
    row_noise_measure(load_decoded_row,p,p->width,p->height,lin,p->seg,p->line);
    if(!aborted(p)){
        p->peak=row_noise_solve(p->seg,p->width,p->height,p->offset);p->measured_at=esp_timer_get_time();
        for(unsigned y=0;y<p->height&&!aborted(p);y++){
            WAIT_UNTIL(p,y-get(&p->consumed)<RGB_RING);
            uint8_t *row=p->rgb_ring+(size_t)(y%RGB_RING)*p->width*3;
            yuv_row(p,y,row);
            row_noise_correct_row(p->offset,y,p->width,p->height,level,inverse,row);
            put(&p->produced,y+1,p->main_task);breathe(y);
        }
    }
}
// 補助タスクはコアごとに常駐。写真ごとに作って消すと、消した後に通知が来て解放済みメモリに書く
static TaskHandle_t helpers[2];static QueueHandle_t helper_jobs[2];
static SemaphoreHandle_t one_photo; // 同時に1枚だけ（2つ動くとお互いを待って止まる）
static void helper(void *arg){
    QueueHandle_t jobs=arg;pipeline *p;
    while(1)if(xQueueReceive(jobs,&p,portMAX_DELAY)==pdTRUE){help(p);xSemaphoreGive(p->done);}
}
esp_err_t destripe_init(void){
    if(!(one_photo=xSemaphoreCreateMutex()))return ESP_ERR_NO_MEM;
    row_noise_levels(level);row_noise_inverse(level,inverse);
    for(int core=0;core<2;core++){
        helper_jobs[core]=xQueueCreate(1,sizeof(pipeline*));
        if(!helper_jobs[core]||xTaskCreatePinnedToCore(helper,"destripe",8192,helper_jobs[core],WORK_PRIORITY,&helpers[core],core)!=pdPASS)return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static const uint8_t *corrected_row(void *arg,unsigned y){
    pipeline *p=arg;
    put(&p->consumed,y,p->helper_task); // yより前の行はjpgeがコピー済み
    breathe(y);
    WAIT_UNTIL(p,get(&p->produced)>y);
    return aborted(p)?NULL:p->rgb_ring+(size_t)(y%RGB_RING)*p->width*3;
}
typedef struct {uint8_t *data;size_t len,capacity;} jpeg_sink;
static bool sink_write(void *arg,const void *data,int len){
    jpeg_sink *s=arg;
    if(s->len+len>s->capacity){
        size_t grown=(s->len+len)*3/2;uint8_t *next=heap_caps_realloc(s->data,grown,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!next)return false;
        s->data=next;s->capacity=grown;
    }
    memcpy(s->data+s->len,data,len);s->len+=len;return true;
}
static void *psram(size_t n){return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
static void *fast(size_t n){return heap_caps_malloc(n,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);}
static esp_err_t run(const uint8_t *jpeg,size_t len,unsigned width,unsigned height,int quality,int helper_core,
                     uint8_t **out,size_t *out_len,float *peak,unsigned ms[4]);
esp_err_t destripe_jpeg(const uint8_t *jpeg,size_t len,unsigned width,unsigned height,int quality,int helper_core,
                        uint8_t **out,size_t *out_len,float *peak,unsigned ms[4]){
    *out=NULL;*out_len=0;*peak=0;
    if(!one_photo)return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(one_photo,portMAX_DELAY);
    esp_err_t e=run(jpeg,len,width,height,quality,helper_core,out,out_len,peak,ms);
    xSemaphoreGive(one_photo);return e;
}
static esp_err_t run(const uint8_t *jpeg,size_t len,unsigned width,unsigned height,int quality,int helper_core,
                     uint8_t **out,size_t *out_len,float *peak,unsigned ms[4]){
    if(width<ROW_NOISE_SEGMENTS*8||width%2||height<2*ROW_NOISE_RADIUS+2)return ESP_ERR_INVALID_SIZE;
    int64_t start=esp_timer_get_time(),decoded_at=start;
    pipeline p={.jpeg=jpeg,.len=len,.width=width,.height=height,.main_task=xTaskGetCurrentTaskHandle()};
    p.yuv=psram((size_t)width*height*2);p.rgb_ring=psram((size_t)RGB_RING*width*3);
    p.seg=psram((size_t)height*ROW_NOISE_SEGMENTS*3*sizeof(float));p.offset=psram((size_t)height*3*sizeof(float));
    p.line=fast(width*3);
    void *pool=malloc(DECODE_POOL);p.done=xSemaphoreCreateBinary();
    jpeg_sink sink={.capacity=len*3/2};sink.data=psram(sink.capacity);
    esp_err_t e=p.yuv&&p.rgb_ring&&p.seg&&p.offset&&p.line&&pool&&p.done&&sink.data?ESP_OK:ESP_ERR_NO_MEM;
    bool started=false;
    // 処理中は補助タスクと同じ優先度。カメラやUSBを邪魔しない
    UBaseType_t priority=uxTaskPriorityGet(NULL);vTaskPrioritySet(NULL,WORK_PRIORITY);
    if(e==ESP_OK&&(helper_core<0||helper_core>1||!helpers[helper_core]))e=ESP_ERR_INVALID_STATE;
    if(e==ESP_OK){
        pipeline *job=&p;p.helper_task=helpers[helper_core];
        if(xQueueSend(helper_jobs[helper_core],&job,portMAX_DELAY)!=pdTRUE)e=ESP_FAIL;else started=true;
    }
    if(e==ESP_OK){
        JDEC jd;
        if(jd_prepare(&jd,decode_input,pool,DECODE_POOL,&p)!=JDR_OK||jd.width!=width||jd.height!=height||jd_decomp(&jd,decode_output,0)!=JDR_OK)
            e=ESP_ERR_INVALID_RESPONSE;
        else if(get(&p.decoded)!=height)e=ESP_ERR_INVALID_SIZE;
        decoded_at=esp_timer_get_time();
    }
    free(pool);
    if(e==ESP_OK&&!jpeg_write_rows(width,height,quality,corrected_row,&p,sink_write,&sink))e=ESP_FAIL;
    if(e!=ESP_OK&&started)stop(&p);
    if(started)xSemaphoreTake(p.done,portMAX_DELAY);
    if(e==ESP_OK&&aborted(&p))e=ESP_FAIL;
    vTaskPrioritySet(NULL,priority);
    int64_t end=esp_timer_get_time();
    if(e==ESP_OK){ // デコード、推定、補正+圧縮、合計
        ms[0]=(decoded_at-start)/1000;ms[1]=p.measured_at>decoded_at?(p.measured_at-decoded_at)/1000:0;
        ms[2]=(end-(p.measured_at>decoded_at?p.measured_at:decoded_at))/1000;ms[3]=(end-start)/1000;
        *peak=p.peak;
    }
    free(p.yuv);free(p.rgb_ring);free(p.seg);free(p.offset);free(p.line);
    if(p.done)vSemaphoreDelete(p.done);
    if(e==ESP_OK){*out=sink.data;*out_len=sink.len;}else free(sink.data);
    return e;
}
