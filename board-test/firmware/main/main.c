#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/spi_common.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "usb_transport.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "frame_quality.h"
#include "esp_rom_crc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "camera_modes.h"
#include "avi_writer.h"
#include "destripe.h"
#include "debug_log.h"
// 空きメモリ（合計/最大ブロック）をログに残す。失敗の多くはPSRAMか内部RAMの不足
#define DLOG_HEAP(where) dlog("%s heap int=%u/%u psram=%u/%u",where,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),\
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),\
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM))
static inline unsigned ms_since(int64_t started){return (esp_timer_get_time()-started)/1000;}

static const int pins[] = {46,38,45,21,1}; // D4 .. D8, active LOW
static bool lights[5], camera_ready;
static adc_oneshot_unit_handle_t adc;
static bool stable[2], candidate[2];
static unsigned counts[2], changed[2], tick;
static unsigned camera_clock = 20000000;
static int camera_profile;
// 細かい景色でJPEGが受信バッファに入らないとフレームが来ない。そのときだけ品質の数字を上げる（モード切替・再準備で0に戻す）
static int quality_boost;
static int64_t camera_started;
static const char *sd_stage = "not_tested";
static int sd_errno;
static QueueHandle_t jobs;
// SDのSPIクロック。20MHzで19枚CRC確認OK(2026-10-20)。マウントできなければその起動中は4MHz
#define SD_KHZ_FAST 20000
#define SD_KHZ_SAFE 4000
static unsigned sd_khz=SD_KHZ_FAST;
static SemaphoreHandle_t sd_lock; // 縞除去タスクと共有
static QueueHandle_t cleanup_jobs;
static volatile unsigned cleanup_pending,cleanup_done,cleanup_failed,cleanup_last_ms,cleanup_steps_ms[5]; // デコード、推定、補正、圧縮、SD差し替え
static volatile esp_err_t cleanup_last=ESP_OK;
static SemaphoreHandle_t usb_mutex;
static portMUX_TYPE state_lock=portMUX_INITIALIZER_UNLOCKED;
static volatile bool busy=true, capture_ready=false, sd_ready=false;
static const char *phase="starting";
static esp_err_t last_error=ESP_OK;
static bool capture_failed; // 直前の撮影・保存の結果（準備状態とは別）
static unsigned warmed_frames, warmup_ms, photos, next_photo=1;
static frame_quality latest_quality;
static char last_photo[96];
static char session_folder[64];
static esp_err_t ensure_session_folder(void);
static bool rotate_sensor=true;
static bool image_settled;
static int gain_limit=0; // 2倍上限: 室内の固定シーンで縞が少なかった
static int sensor_gain, sensor_exposure;
static volatile unsigned active_mode,selected_mode;
static volatile bool selecting,recording,stop_recording,countdown; // stop_recordingはセルフタイマーの中止も兼ねる
static volatile int64_t countdown_end;
static int init_size=-1,init_profile;static bool init_video;static unsigned init_clock;
static unsigned rv_max=2048;
static mode_selector rv_selector;
static volatile unsigned record_frames,record_ms;
static unsigned video_count;
static char last_video[96];
static uint32_t last_video_crc,last_video_bytes;
static void execute_request(cJSON *req);
static bool submit(cJSON *req);
static bool ap_is_running(void);
static void add_ap_status(cJSON *r);
static void mode_leds(unsigned number,bool visible){
    portENTER_CRITICAL(&state_lock);
    for(int i=2;i<5;i++){lights[i]=visible&&((number>>(4-i))&1);gpio_set_level(pins[i],!lights[i]);}
    portEXIT_CRITICAL(&state_lock);
}
static esp_err_t save_mode(void){
    nvs_handle_t h;esp_err_t e=nvs_open("camera",NVS_READWRITE,&h);
    if(e==ESP_OK){e=nvs_set_u8(h,"mode",active_mode);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}return e;
}
static void select_begin(void){
    portENTER_CRITICAL(&state_lock);
    if(!busy&&!recording){selecting=true;selected_mode=active_mode;selector_begin(&rv_selector,active_mode);capture_ready=false;phase="selecting";}
    portEXIT_CRITICAL(&state_lock);
}
static void finish_job(esp_err_t e);
static void select_cancel(void){
    if(selecting&&!busy){selecting=false;mode_leds(active_mode,true);finish_job((active_mode==7?ap_is_running():image_settled)&&sd_ready?ESP_OK:ESP_ERR_INVALID_STATE);}
}

static void prepare_camera(void);

// LEDは10msごとに状態から決める
// 緑: 点灯=準備OK, ゆっくり=処理中, 速い=録画中/セルフタイマー, 消灯=選択中/停止
// 赤: 点灯=直前の失敗, 点滅=準備失敗で停止
static const char *ready_pattern(void){return countdown||(recording&&strcmp(phase,"finalizing"))?"fast":selecting?"off":busy?"slow":capture_ready?"on":"off";}
static bool halted(void){return !busy&&!selecting&&!recording&&!countdown&&!capture_ready;}
static const char *error_pattern(void){return halted()?"blink":capture_failed?"on":"off";}
static bool pattern_lit(const char *p,int64_t ms){return !strcmp(p,"on")||((!strcmp(p,"slow")||!strcmp(p,"blink"))&&ms%1000<500)||(!strcmp(p,"fast")&&ms%250<125);}
static void update_indicators(void){
    int64_t ms=esp_timer_get_time()/1000;
    portENTER_CRITICAL(&state_lock);
    lights[0]=pattern_lit(ready_pattern(),ms);lights[1]=pattern_lit(error_pattern(),ms);
    gpio_set_level(46,!lights[0]);gpio_set_level(38,!lights[1]);
    portEXIT_CRITICAL(&state_lock);
}

static void set_phase(const char *name, bool ready, esp_err_t error){
    portENTER_CRITICAL(&state_lock);
    phase=name;capture_ready=ready;last_error=error;
    portEXIT_CRITICAL(&state_lock);
    dlog("phase %s",name);
}
static void finish_job(esp_err_t e){
    portENTER_CRITICAL(&state_lock);
    busy=false;phase=e==ESP_OK?(selecting?"selecting":active_mode==7?"ap_ready":"ready"):"error";capture_ready=e==ESP_OK&&!selecting;last_error=e;
    portEXIT_CRITICAL(&state_lock);
    dlog("finish %s -> %s",esp_err_to_name(e),phase);
}

static int usb_write(const void *data, size_t size, void *context) {
    (void)context;
    return usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(500));
}
// 画像(base64)はJSON文字列に組み込まず、外して先頭に直接書く。全体を1本の文字列にすると
// 画像の数倍の連続したPSRAMが要り、縞除去中だと取れずにNO_MEMになる
static const char *const image_keys[2]={"jpeg","jpeg_raw"};
static void send_json(cJSON *j) {
    xSemaphoreTake(usb_mutex,portMAX_DELAY);
    cJSON *images[2];for(int i=0;i<2;i++)images[i]=cJSON_DetachItemFromObject(j,image_keys[i]);
    char *s=cJSON_PrintUnformatted(j);
    if(s){
        // 再接続時に古い応答の残りが来ることがあるので改行から始める
        usb_serial_jtag_write_bytes("\n",1,pdMS_TO_TICKS(100));
        bool good=true,any=false;
        for(int i=0;i<2&&good;i++){
            if(!cJSON_IsString(images[i]))continue;
            char key[16];int n=snprintf(key,sizeof(key),"%s\"%s\":\"",any?",":"{",image_keys[i]);any=true;
            good=board_usb_send(key,n,usb_write,NULL)&&board_usb_send(images[i]->valuestring,strlen(images[i]->valuestring),usb_write,NULL)&&board_usb_send("\"",1,usb_write,NULL);
        }
        // 残りは '{' を飛ばしてつなぐ（idやstatusが必ずあるので空にはならない）
        if(good)good=board_usb_send(any?",":"{",1,usb_write,NULL)&&board_usb_send(s+1,strlen(s+1),usb_write,NULL);
        if(!good)dlog("usb: reply write failed (PC not reading?) image=%d",any);
        // 途中で切れても行を閉じる（次の要求で同期できるように）
        usb_serial_jtag_write_bytes("\n",1,pdMS_TO_TICKS(100)); free(s);
    }else{ // 内部RAMが足りずJSONが作れなくても、idだけ付けてエラーを返す（黙るとPC側はタイムアウトになる）
        cJSON *id=cJSON_GetObjectItem(j,"id");char line[96];
        int n=snprintf(line,sizeof(line),"\n{\"id\":%d,\"ok\":false,\"error\":\"%s\"}\n",cJSON_IsNumber(id)?id->valueint:0,esp_err_to_name(ESP_ERR_NO_MEM));
        board_usb_send(line,n,usb_write,NULL);
        DLOG_HEAP("usb: reply JSON could not be built (NO_MEM)");
    }
    for(int i=0;i<2;i++)cJSON_Delete(images[i]);
    cJSON_Delete(j);
    xSemaphoreGive(usb_mutex);
}
static esp_err_t apply_mode_settings(sensor_t *s,bool live){
    const camera_mode *m=&camera_modes[active_mode];
    if(live&&(s->set_framesize(s,(framesize_t)m->size)||s->set_quality(s,m->quality+quality_boost)))return ESP_FAIL;
    if(gain_limit>=0&&s->set_gainceiling(s,(gainceiling_t)gain_limit))return ESP_FAIL;
    if(s->set_saturation(s,m->saturation)||s->set_contrast(s,m->contrast)||s->set_aec2(s,m->night?1:0)||s->set_brightness(s,m->night?1:0)||s->set_special_effect(s,m->effect))return ESP_FAIL;
    return ESP_OK;
}
static esp_err_t camera_init(void) {
    if(camera_ready)return ESP_OK;
    camera_config_t c={
        .pin_pwdn=-1,.pin_reset=42,.pin_xclk=15,.pin_sccb_sda=4,.pin_sccb_scl=5,
        .pin_d0=40,.pin_d1=9,.pin_d2=8,.pin_d3=41,.pin_d4=3,.pin_d5=18,.pin_d6=17,.pin_d7=16,
        .pin_vsync=6,.pin_href=7,.pin_pclk=39,.xclk_freq_hz=camera_clock,
        .ledc_timer=LEDC_TIMER_0,.ledc_channel=LEDC_CHANNEL_0,
        .pixel_format=PIXFORMAT_JPEG,.frame_size=camera_modes[active_mode].size,.jpeg_quality=camera_modes[active_mode].quality+quality_boost,.fb_count=camera_modes[active_mode].video?2:1,
        .fb_location=CAMERA_FB_IN_PSRAM,.grab_mode=camera_modes[active_mode].video?CAMERA_GRAB_LATEST:CAMERA_GRAB_WHEN_EMPTY
    };
    int64_t started=esp_timer_get_time();
    esp_err_t e=esp_camera_init(&c); camera_ready=e==ESP_OK;
    dlog("camera_init size=%d quality=%d fb=%d clock=%u profile=%d -> %s (%u ms)",c.frame_size,c.jpeg_quality,c.fb_count,camera_clock,camera_profile,esp_err_to_name(e),ms_since(started));
    if(camera_ready){
        camera_started=esp_timer_get_time();
        sensor_t *s=esp_camera_sensor_get();
        if(camera_profile==1||camera_profile==3)s->set_reg(s,0x010c,0x06,0x04);
        if(camera_profile==2)s->set_reg(s,0x010c,0x06,0x02);
        if(camera_profile==3)s->set_aec2(s,1);
        if(rotate_sensor){s->set_vflip(s,1);s->set_hmirror(s,1);}
        if(apply_mode_settings(s,false)!=ESP_OK){esp_camera_deinit();camera_ready=false;return ESP_FAIL;}
        init_size=camera_modes[active_mode].size;init_video=camera_modes[active_mode].video;init_profile=camera_profile;init_clock=camera_clock;
    }
    return e;
}
// バッファ形式が同じで解像度が同じか小さいなら、再起動せず露出・WBを引き継ぐ
static bool can_switch_live(void){
    const camera_mode *m=&camera_modes[active_mode];
    return camera_ready&&image_settled&&init_video==m->video&&m->size<=init_size&&init_profile==0&&init_clock==20000000&&rotate_sensor&&camera_profile==0;
}
static esp_err_t sd_test_locked(void) {
    sd_stage="spi_init";sd_errno=0;
    spi_bus_config_t bus={.mosi_io_num=11,.miso_io_num=13,.sclk_io_num=12,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096};
    esp_err_t e=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO);
    if(e!=ESP_OK)return e;
    sdmmc_host_t host=SDSPI_HOST_DEFAULT(); host.slot=SPI2_HOST; host.max_freq_khz=sd_khz;
    sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT(); slot.gpio_cs=10; slot.host_id=SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount={.format_if_mount_failed=false,.max_files=2,.allocation_unit_size=16384};
    sdmmc_card_t *card=NULL;
    sd_stage="mount";e=esp_vfs_fat_sdspi_mount("/sdcard",&host,&slot,&mount,&card);
    if(e==ESP_OK){
        const char *path="/sdcard/pcbtest.tmp";
        // 既存ファイルは上書きしない
        sd_stage="exclusive_create";FILE *f=fopen(path,"wx");
        if(!f){sd_errno=errno;e=ESP_FAIL;}
        else {
            const char data[]="camera-board-test\n"; char buf[sizeof(data)]={0};
            sd_stage="write_read_verify";bool ok=fwrite(data,1,sizeof(data),f)==sizeof(data); if(fclose(f)!=0)ok=false;
            f=fopen(path,"rb");
            if(!f)ok=false;
            else {if(fread(buf,1,sizeof(buf),f)!=sizeof(buf)||memcmp(buf,data,sizeof(data)))ok=false; fclose(f);}
            if(unlink(path)!=0)ok=false;
            e=ok?ESP_OK:ESP_FAIL;if(!ok)sd_errno=errno;else sd_stage="complete";
        }
        if(e==ESP_OK)e=ensure_session_folder();
        esp_vfs_fat_sdcard_unmount("/sdcard",card);
    }
    spi_bus_free(SPI2_HOST); return e;
}
static void add_status(cJSON *r, esp_err_t e){
    int raw=0;esp_err_t ae=adc_oneshot_read(adc,ADC_CHANNEL_1,&raw);
    add_ap_status(r);
    cJSON_AddStringToObject(r,"device","camera-board-test-v1");
    cJSON_AddNumberToObject(r,"cpu_mhz",CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(r,"reset_reason",esp_reset_reason()); // 4 = panic, 6/7 = watchdog
    cJSON_AddNumberToObject(r,"psram_mhz",CONFIG_SPIRAM_SPEED);
    cJSON_AddBoolToObject(r,"ok",e==ESP_OK);cJSON_AddStringToObject(r,"error",esp_err_to_name(e));
    cJSON_AddNumberToObject(r,"adc",ae==ESP_OK?raw:-1);
    cJSON_AddNumberToObject(r,"psram",heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r,"jpeg_quality",camera_modes[active_mode].quality+quality_boost);cJSON_AddNumberToObject(r,"quality_boost",quality_boost);
    // 空き/最大ブロック。内部RAMが尽きると応答のJSONが作れずUSBが黙る
    cJSON_AddNumberToObject(r,"heap_internal",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r,"heap_internal_block",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r,"heap_internal_min",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r,"heap_psram",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r,"heap_psram_block",heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(r,"sd_stage",sd_stage);cJSON_AddNumberToObject(r,"sd_khz",sd_khz);cJSON_AddNumberToObject(r,"sd_errno",sd_errno);
    cJSON_AddStringToObject(r,"phase",phase);cJSON_AddBoolToObject(r,"ready",capture_ready);
    cJSON_AddBoolToObject(r,"busy",busy);cJSON_AddBoolToObject(r,"sd_ready",sd_ready);
    cJSON_AddStringToObject(r,"last_error",esp_err_to_name(last_error));
    cJSON_AddBoolToObject(r,"last_capture_failed",capture_failed);
    cJSON_AddStringToObject(r,"ready_led",ready_pattern());
    cJSON_AddBoolToObject(r,"countdown",countdown);{int64_t left=countdown?(countdown_end-esp_timer_get_time())/1000:0;cJSON_AddNumberToObject(r,"countdown_ms",left>0?left:0);}cJSON_AddStringToObject(r,"error_led",error_pattern());
    cJSON_AddNumberToObject(r,"warmup_frames",warmed_frames);cJSON_AddNumberToObject(r,"warmup_ms",warmup_ms);
    cJSON_AddNumberToObject(r,"mode",active_mode);cJSON_AddStringToObject(r,"mode_name",camera_modes[active_mode].name);
    cJSON_AddBoolToObject(r,"selecting",selecting);cJSON_AddNumberToObject(r,"selected_mode",selected_mode);
    cJSON_AddNumberToObject(r,"rv_filtered",rv_selector.filtered);
    cJSON_AddNumberToObject(r,"rv_max",rv_max);cJSON_AddBoolToObject(r,"recording",recording);
    cJSON_AddNumberToObject(r,"record_frames",record_frames);cJSON_AddNumberToObject(r,"record_ms",record_ms);
    cJSON_AddNumberToObject(r,"video_count",video_count);cJSON_AddStringToObject(r,"last_video",last_video);
    cJSON_AddNumberToObject(r,"last_video_bytes",last_video_bytes);cJSON_AddNumberToObject(r,"last_video_crc",last_video_crc);
    cJSON_AddStringToObject(r,"session_folder",session_folder[0]?session_folder+7:"");
    cJSON_AddNumberToObject(r,"photo_count",photos);cJSON_AddNumberToObject(r,"luma",latest_quality.luma);
    cJSON_AddNumberToObject(r,"gain_limit",gain_limit);cJSON_AddNumberToObject(r,"gain_reg",sensor_gain);
    cJSON_AddNumberToObject(r,"exposure",sensor_exposure);
    cJSON_AddNumberToObject(r,"cleanup_pending",cleanup_pending);cJSON_AddNumberToObject(r,"cleanup_done",cleanup_done);
    cJSON_AddNumberToObject(r,"cleanup_failed",cleanup_failed);cJSON_AddStringToObject(r,"cleanup_last",esp_err_to_name(cleanup_last));
    cJSON_AddNumberToObject(r,"cleanup_last_ms",cleanup_last_ms);
    {cJSON *steps=cJSON_AddArrayToObject(r,"cleanup_steps_ms");for(int i=0;i<5;i++)cJSON_AddItemToArray(steps,cJSON_CreateNumber(cleanup_steps_ms[i]));}
    cJSON_AddNumberToObject(r,"sensor_rotation",rotate_sensor?180:0);
    char path[sizeof(last_photo)];portENTER_CRITICAL(&state_lock);memcpy(path,last_photo,sizeof(path));portEXIT_CRITICAL(&state_lock);
    cJSON_AddStringToObject(r,"last_photo",path);
    cJSON *a=cJSON_AddArrayToObject(r,"leds");for(int i=0;i<5;i++)cJSON_AddItemToArray(a,cJSON_CreateBool(lights[i]));
    a=cJSON_AddArrayToObject(r,"buttons");for(int i=0;i<2;i++)cJSON_AddItemToArray(a,cJSON_CreateBool(stable[i]));
    a=cJSON_AddArrayToObject(r,"counts");for(int i=0;i<2;i++)cJSON_AddItemToArray(a,cJSON_CreateNumber(counts[i]));
}
static esp_err_t add_image(cJSON *r,const uint8_t *data,size_t len){
    size_t cap=4*((len+2)/3)+1,written=0;
    unsigned char *b=malloc(cap);if(!b)return ESP_ERR_NO_MEM;
    if(mbedtls_base64_encode(b,cap,&written,data,len)){free(b);return ESP_FAIL;}
    // コピーせずにbをそのまま持たせる（cJSONはvaluestringをfreeで解放する）
    cJSON *item=cJSON_CreateString("");
    if(!item){free(b);return ESP_ERR_NO_MEM;}
    cJSON_free(item->valuestring);item->valuestring=(char*)b;
    cJSON_DeleteItemFromObject(r,"jpeg");cJSON_AddItemToObject(r,"jpeg",item);
    return ESP_OK;
}
static esp_err_t sd_probe_locked(cJSON *r){
    spi_bus_config_t bus={.mosi_io_num=11,.miso_io_num=13,.sclk_io_num=12,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096};
    esp_err_t e=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO);if(e!=ESP_OK)return e;
    sdmmc_host_t host=SDSPI_HOST_DEFAULT();host.slot=SPI2_HOST;host.max_freq_khz=sd_khz;
    sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();slot.gpio_cs=10;slot.host_id=SPI2_HOST;
    sdspi_dev_handle_t handle=-1;
    e=sdspi_host_init_device(&slot,&handle);
    sdmmc_card_t card={0};uint8_t *sector=heap_caps_calloc(1,512,MALLOC_CAP_DMA);
    if(e==ESP_OK){host.slot=handle;e=sdmmc_card_init(&host,&card);}
    if(e==ESP_OK){
        cJSON_AddNumberToObject(r,"sd_capacity_mb",(double)card.csd.capacity*card.csd.sector_size/1048576);
        if(!sector)e=ESP_ERR_NO_MEM;
        else e=sdmmc_read_sectors(&card,sector,0,1);
        if(e==ESP_OK){
            uint32_t lba=(uint32_t)sector[454]|((uint32_t)sector[455]<<8)|((uint32_t)sector[456]<<16)|((uint32_t)sector[457]<<24);
            if(memcmp(sector+3,"EXFAT   ",8)&&memcmp(sector+82,"FAT32",5)&&memcmp(sector+54,"FAT",3)&&lba&&lba<card.csd.capacity){
                cJSON_AddNumberToObject(r,"partition_lba",lba);e=sdmmc_read_sectors(&card,sector,lba,1);
            }
            if(e==ESP_OK)cJSON_AddStringToObject(r,"sd_format",!memcmp(sector+3,"EXFAT   ",8)?"exFAT":!memcmp(sector+82,"FAT32",5)?"FAT32":!memcmp(sector+54,"FAT",3)?"FAT16":"unknown");
        }
    }
    free(sector);if(handle>=0)sdspi_host_remove_device(handle);spi_bus_free(SPI2_HOST);return e;
}
typedef struct{uint8_t *data;size_t len;double ms;} saved_frame;
static void capture_series(cJSON *req,cJSON *r){
    cJSON *n=cJSON_GetObjectItem(req,"count"),*clk=cJSON_GetObjectItem(req,"clock_mhz"),*profile=cJSON_GetObjectItem(req,"profile");
    int count=cJSON_IsNumber(n)?n->valueint:12;
    unsigned clock=cJSON_IsNumber(clk)?clk->valueint:20;
    int prof=cJSON_IsNumber(profile)?profile->valueint:0;
    esp_err_t e=ESP_OK;saved_frame frames[12]={0};
    if(count<1||count>12||prof<0||prof>3||(clock!=10&&clock!=16&&clock!=20))e=ESP_ERR_INVALID_ARG;
    if(e==ESP_OK){
        if(camera_ready){esp_camera_deinit();camera_ready=false;}
        rotate_sensor=false;camera_clock=clock*1000000;camera_profile=prof;e=camera_init();
    }
    for(int i=0;e==ESP_OK&&i<count;i++){
        camera_fb_t *fb=esp_camera_fb_get();
        if(!fb){e=ESP_ERR_TIMEOUT;break;}
        frames[i].ms=(esp_timer_get_time()-camera_started)/1000.0;frames[i].len=fb->len;
        frames[i].data=heap_caps_malloc(fb->len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(frames[i].data)memcpy(frames[i].data,fb->buf,fb->len);else e=ESP_ERR_NO_MEM;
        esp_camera_fb_return(fb);
    }
    if(e==ESP_OK){
        cJSON *id=cJSON_GetObjectItem(req,"id");
        for(int i=0;i<count;i++){
            cJSON *out=cJSON_CreateObject();if(cJSON_IsNumber(id))cJSON_AddNumberToObject(out,"id",id->valueint);
            cJSON_AddNumberToObject(out,"series_index",i+1);cJSON_AddNumberToObject(out,"series_count",count);
            cJSON_AddNumberToObject(out,"camera_ms",frames[i].ms);cJSON_AddNumberToObject(out,"clock_mhz",clock);
            cJSON_AddNumberToObject(out,"profile",prof);
            e=add_image(out,frames[i].data,frames[i].len);free(frames[i].data);frames[i].data=NULL;
            add_status(out,e);send_json(out);if(e!=ESP_OK)break;
        }
    }else{add_status(r,e);send_json(r);r=NULL;}
    for(int i=0;i<12;i++)free(frames[i].data);
    cJSON_Delete(r);
}
#include "session_storage.inc"
static esp_err_t sd_test(void){
    xSemaphoreTake(sd_lock,portMAX_DELAY);esp_err_t e=sd_test_locked();
    if(e!=ESP_OK&&sd_khz>SD_KHZ_SAFE){sd_khz=SD_KHZ_SAFE;e=sd_test_locked();}
    xSemaphoreGive(sd_lock);return e;
}
static esp_err_t sd_probe(cJSON *r){xSemaphoreTake(sd_lock,portMAX_DELAY);esp_err_t e=sd_probe_locked(r);xSemaphoreGive(sd_lock);return e;}
static esp_err_t sd_save_locked(camera_fb_t *frame,cJSON *reply){
    set_phase("saving",false,ESP_OK);sd_stage="mount_for_save";
    spi_bus_config_t bus={.mosi_io_num=11,.miso_io_num=13,.sclk_io_num=12,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096};
    esp_err_t e=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO);if(e!=ESP_OK)return e;
    sdmmc_host_t host=SDSPI_HOST_DEFAULT();host.slot=SPI2_HOST;host.max_freq_khz=sd_khz;
    sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();slot.gpio_cs=10;slot.host_id=SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount={.format_if_mount_failed=false,.max_files=3,.allocation_unit_size=16384};
    sdmmc_card_t *card=NULL;e=esp_vfs_fat_sdspi_mount("/sdcard",&host,&slot,&mount,&card);
    if(e==ESP_OK){
        e=ensure_session_folder();
        const char *folder=session_folder;
        if(e==ESP_OK&&next_photo==1){
            DIR *dir=opendir(folder);
            if(!dir)e=ESP_FAIL;
            else {struct dirent *entry;while((entry=readdir(dir))){unsigned n;char suffix[4];
                if(sscanf(entry->d_name,"IMG%5u.%3s",&n,suffix)==2&&n<99999&&n>=next_photo&&(!strcmp(suffix,"JPG")||!strcmp(suffix,"TMP")||!strcmp(suffix,"CLN")||!strcmp(suffix,"ORG")))next_photo=n+1;
            }closedir(dir);}
        }
        char temporary[96]={0},final[96]={0};FILE *file=NULL;
        while(e==ESP_OK&&next_photo<=99999){
            unsigned n=next_photo++;
            snprintf(temporary,sizeof(temporary),"%s/IMG%05u.TMP",folder,n);
            snprintf(final,sizeof(final),"%s/IMG%05u.JPG",folder,n);
            struct stat st;
            if(stat(final,&st)==0||stat(temporary,&st)==0)continue;
            char spare[96];snprintf(spare,sizeof(spare),"%s/IMG%05u.ORG",folder,n);if(stat(spare,&st)==0)continue; // 縞除去の途中で残ったもの
            snprintf(spare,sizeof(spare),"%s/IMG%05u.CLN",folder,n);if(stat(spare,&st)==0)continue;
            sd_stage="write_photo";file=fopen(temporary,"wx");
            if(file)break;
            if(errno!=EEXIST){e=ESP_FAIL;break;}
        }
        if(e==ESP_OK&&!file)e=ESP_ERR_NO_MEM;
        if(file){
            bool good=fwrite(frame->buf,1,frame->len,file)==frame->len;
            if(fflush(file)!=0)good=false;
            if(fsync(fileno(file))!=0)good=false;
            if(fclose(file)!=0)good=false;
            if(!good)e=ESP_FAIL;
            if(e==ESP_OK){
                sd_stage="verify_photo";file=fopen(temporary,"rb");
                uint8_t buffer[2048];size_t total=0;uint32_t crc=0;
                if(!file)e=ESP_FAIL;
                else{
                    size_t n;while((n=fread(buffer,1,sizeof(buffer),file))){crc=esp_rom_crc32_le(crc,buffer,n);total+=n;}
                    if(ferror(file)||total!=frame->len||crc!=esp_rom_crc32_le(0,frame->buf,frame->len))e=ESP_ERR_INVALID_CRC;
                    fclose(file);
                }
            }
            if(e==ESP_OK){
                sd_stage="commit_photo";
                if(rename(temporary,final)!=0)e=ESP_FAIL;
                else{
                    portENTER_CRITICAL(&state_lock);strlcpy(last_photo,final+7,sizeof(last_photo));photos++;portEXIT_CRITICAL(&state_lock);
                    cJSON_AddStringToObject(reply,"saved_path",final+7);cJSON_AddNumberToObject(reply,"saved_bytes",frame->len);
                    cJSON_AddNumberToObject(reply,"crc32",esp_rom_crc32_le(0,frame->buf,frame->len));
                    sd_stage="complete";
                }
            }
        }
        if(e!=ESP_OK)sd_errno=errno;
        esp_err_t unmount=esp_vfs_fat_sdcard_unmount("/sdcard",card);if(e==ESP_OK)e=unmount;
    }else dlog("sd_save: mount failed %s khz=%u",esp_err_to_name(e),sd_khz);
    if(e!=ESP_OK)dlog("sd_save: FAILED %s stage=%s errno=%d len=%u prev=%s",esp_err_to_name(e),sd_stage,sd_errno,(unsigned)frame->len,last_photo);
    sd_ready=e==ESP_OK;spi_bus_free(SPI2_HOST);return e;
}
static esp_err_t sd_save(camera_fb_t *frame,cJSON *reply){
    xSemaphoreTake(sd_lock,portMAX_DELAY);int64_t started=esp_timer_get_time();
    esp_err_t e=sd_save_locked(frame,reply);
    cJSON_AddNumberToObject(reply,"save_ms",(esp_timer_get_time()-started)/1000);
    if(e==ESP_OK)dlog("sd_save ok %s %u bytes %u ms",last_photo,(unsigned)frame->len,ms_since(started));
    xSemaphoreGive(sd_lock);return e;
}
static camera_fb_t *settled_frame(bool startup,esp_err_t *error){
    quality_gate gate={0};int64_t started=esp_timer_get_time();unsigned invalid=0;
    unsigned observed=0,boosts=0;
    if(!startup){gate.previous=latest_quality;gate.has_previous=true;}
    const unsigned minimum=startup?4000:0,limit=startup?25000:10000;
    while((esp_timer_get_time()-started)/1000<limit+boosts*6000){
        int64_t asked=esp_timer_get_time();
        camera_fb_t *frame=esp_camera_fb_get();frame_quality q={0};
        observed++;
        // 1枚ごと。lenがバッファ(768KB)に近いと次は入らずに捨てられる
        if(frame)dlog("frame #%u %s len=%u %ux%u wait=%u ms",observed,startup?"warm":"shot",(unsigned)frame->len,frame->width,frame->height,ms_since(asked));
        else dlog("frame #%u NULL after %u ms (camera_ready=%d quality=%d)",observed,ms_since(asked),camera_ready,camera_modes[active_mode].quality+quality_boost);
        if(!frame&&camera_ready&&quality_boost<16){ // フレームが来ない＝JPEGがバッファに入らない。圧縮を強めてやり直す
            sensor_t *s=esp_camera_sensor_get();quality_boost+=4;boosts++;
            if(s)s->set_quality(s,camera_modes[active_mode].quality+quality_boost);
            dlog("quality boost -> %d (jpeg quality %d)",quality_boost,camera_modes[active_mode].quality+quality_boost);
            continue;
        }
        unsigned before_decode=(esp_timer_get_time()-started)/1000;
        if(startup&&before_decode<2500&&frame&&frame->len>1024){
            warmed_frames=observed;warmup_ms=before_decode;
            esp_camera_fb_return(frame);vTaskDelay(pdMS_TO_TICKS(20));continue;
        }
        esp_err_t e=measure_frame(frame,&q);
        unsigned elapsed=(esp_timer_get_time()-started)/1000;
        if(e==ESP_OK){
            latest_quality=q;
            if(startup)gate.frames=observed-1;
            bool settled=quality_gate_check(&gate,&q,elapsed,minimum,startup?12:(observed==1?1:3),startup?3:(observed==1?1:2));
            if(startup){warmed_frames=gate.frames;warmup_ms=elapsed;}
            if(settled){
                sensor_t *s=esp_camera_sensor_get();
                sensor_gain=s->get_reg(s,0x0100,0xff);
                sensor_exposure=(s->get_reg(s,0x0145,0x3f)<<10)|(s->get_reg(s,0x0110,0xff)<<2)|s->get_reg(s,0x0104,3);
                dlog("settled %s after %u frames %u ms luma=%.1f gain=%d exposure=%d",startup?"warm":"shot",observed,elapsed,q.luma,sensor_gain,sensor_exposure);
                *error=ESP_OK;return frame;
            }
        }else{
            dlog("frame #%u unusable: %s (invalid %u/3)",observed,esp_err_to_name(e),invalid+1);
            gate.has_previous=false;gate.stable=0;invalid++;
            if(invalid>=3){if(frame)esp_camera_fb_return(frame);*error=e;return NULL;}
        }
        if(frame)esp_camera_fb_return(frame);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    dlog("settled %s TIMEOUT after %u frames %u ms (boosts %u)",startup?"warm":"shot",observed,ms_since(started),boosts);
    *error=ESP_ERR_TIMEOUT;return NULL;
}
#include "media.inc"
#include "ap_gallery.inc"
#include "cleanup.inc"
static bool ap_is_running(void){return ap_running;}
// ログをSDの/DEBUG.LOGに追記して電源を切っても残す。仕事が終わるたびに起こされ、優先度1でSDを使っていない時に書く
// 4MBを超えたらDEBUG.OLDに回す
static TaskHandle_t dlog_task;
static void dlog_kick(void){if(dlog_task)xTaskNotifyGive(dlog_task);}
static void dlog_sd_worker(void *arg){
    (void)arg;
    while(1){
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(60000));vTaskDelay(pdMS_TO_TICKS(1500)); // 続けて起きた分はまとめる
        if(ap_running||recording||dlog_sd_mark()>=dlog_head())continue;
        sdmmc_card_t *card=NULL;if(media_mount(&card)!=ESP_OK)continue;
        struct stat st;if(stat("/sdcard/DEBUG.LOG",&st)==0&&st.st_size>4*1024*1024){unlink("/sdcard/DEBUG.OLD");rename("/sdcard/DEBUG.LOG","/sdcard/DEBUG.OLD");}
        FILE *f=fopen("/sdcard/DEBUG.LOG","a");char *buffer=malloc(4096);
        if(f&&buffer){
            uint32_t position=dlog_sd_mark();bool good=true;
            while(good){
                uint32_t from=position;size_t n=dlog_read(&from,buffer,4096,NULL);
                if(from>position){char lost[48];int m=snprintf(lost,sizeof(lost),"[%u bytes of log lost]\n",(unsigned)(from-position));good=fwrite(lost,1,m,f)==(size_t)m;}
                if(!n)break;
                good=good&&fwrite(buffer,1,n,f)==n;position=from+n;
            }
            if(fclose(f)!=0)good=false;
            f=NULL;
            if(good)dlog_set_sd_mark(position);
        }
        if(f)fclose(f);
        free(buffer);media_unmount(card);
    }
}
static void add_ap_status(cJSON *r){
    cJSON_AddBoolToObject(r,"ap_active",ap_running);cJSON_AddStringToObject(r,"ap_ssid",ap_ssid);
    cJSON_AddStringToObject(r,"ap_password",ap_password);cJSON_AddStringToObject(r,"ap_url","http://192.168.4.1");
    cJSON_AddNumberToObject(r,"ap_clients",ap_clients());
}
static void prepare_camera(void){
    set_phase("switching",false,ESP_OK);quality_boost=0;
    esp_err_t stopped=ap_gallery_stop();
    if(stopped!=ESP_OK){sd_ready=false;finish_job(stopped);return;}
    if(active_mode==7){
        set_phase("cleaning",false,ESP_OK);cleanup_make_room(0,60000); // スマホには縞除去済みを渡す
        image_settled=false;if(camera_ready){esp_camera_deinit();camera_ready=false;}
        set_phase("starting_ap",false,ESP_OK);esp_err_t e=ap_gallery_start();sd_ready=e==ESP_OK;finish_job(e);return;
    }
    bool live=can_switch_live();
    dlog("prepare mode=%u (%s) live=%d camera_ready=%d",active_mode,camera_modes[active_mode].name,live,camera_ready);DLOG_HEAP("prepare");
    set_phase("checking_sd",false,ESP_OK);image_settled=false;warmed_frames=warmup_ms=0;
    esp_err_t e=sd_test();sd_ready=e==ESP_OK;
    dlog("sd_test %s stage=%s errno=%d khz=%u",esp_err_to_name(e),sd_stage,sd_errno,sd_khz);
    if(e==ESP_OK&&live){
        // 速い方: 動いているセンサーの設定だけ変えて短い安定確認
        set_phase("warming",false,ESP_OK);int64_t started=esp_timer_get_time();
        camera_fb_t *frame=apply_mode_settings(esp_camera_sensor_get(),true)==ESP_OK?settled_frame(false,&e):NULL;
        if(frame){esp_camera_fb_return(frame);warmup_ms=(esp_timer_get_time()-started)/1000;image_settled=true;finish_job(ESP_OK);return;}
        dlog("live switch failed (%s), restarting camera",esp_err_to_name(e));
        e=ESP_OK; // だめなら下で再起動
    }
    if(e==ESP_OK){
        set_phase("starting_camera",false,ESP_OK);
        if(camera_ready){esp_camera_deinit();camera_ready=false;}
        camera_clock=20000000;camera_profile=0;rotate_sensor=true;e=camera_init();
    }
    if(e==ESP_OK){
        set_phase("warming",false,ESP_OK);camera_fb_t *frame=settled_frame(true,&e);
        if(frame){esp_camera_fb_return(frame);image_settled=true;}
    }
    finish_job(e);
}
static esp_err_t take_photo(cJSON *reply){
    set_phase("metering",false,ESP_OK);esp_err_t e=ESP_OK;
    dlog("photo start mode=%u quality=%d cleanup_pending=%u",active_mode,camera_modes[active_mode].quality+quality_boost,cleanup_pending);DLOG_HEAP("photo");
    if(!camera_ready||!sd_ready||!image_settled){
        dlog("photo refused: camera_ready=%d sd_ready=%d image_settled=%d",camera_ready,sd_ready,image_settled);return ESP_ERR_INVALID_STATE;
    }
    int64_t t0=esp_timer_get_time();
    // 縞除去待ちは3枚まで（1枚6秒くらいかかるので、溜めるとPSRAMが尽きて返信が作れなくなる）
    // 加えてフレームのコピー分を先に空ける（フレームは長く持てない）
    cleanup_wait_backlog(3,30000);cleanup_make_room(1024*1024,15000);
    unsigned waited=ms_since(t0);if(waited>50){dlog("photo waited %u ms for cleanup (pending=%u)",waited,cleanup_pending);DLOG_HEAP("photo after wait");}
    // シャッターより前のフレームを捨てる
    int64_t t1=esp_timer_get_time();
    camera_fb_t *old=esp_camera_fb_get();if(old)esp_camera_fb_return(old);
    camera_fb_t *frame=settled_frame(false,&e);
    unsigned capture_ms=ms_since(t1);
    if(!frame)dlog("photo: no frame %s after %u ms",esp_err_to_name(e),capture_ms);
    if(frame){
        // すぐコピーする。数秒持っているとドライバに上書きされて下の方が壊れる
        camera_fb_t photo={.len=frame->len,.width=frame->width,.height=frame->height,.format=frame->format};
        photo.buf=photo_copy(frame->buf,frame->len);esp_camera_fb_return(frame);
        if(!photo.buf){DLOG_HEAP("photo: copy NO_MEM");return ESP_ERR_NO_MEM;}
        int64_t t2=esp_timer_get_time();
        e=sd_save(&photo,reply);
        unsigned save_ms=ms_since(t2),send_wait=0,encode_ms=0;
        // 撮ってSDに入れてから、USBの返信分（base64で約1.4倍）が空くのを待つ。シャッターは待たせない
        if(e==ESP_OK){set_phase("sending",false,ESP_OK);int64_t t3=esp_timer_get_time();cleanup_make_room(photo.len*3/2+256*1024,15000);send_wait=ms_since(t3);
            // SDには入っているので、PCに画像を送れなくても撮影は成功扱い（赤を点けない）
            int64_t t4=esp_timer_get_time();esp_err_t sent=add_image(reply,photo.buf,photo.len);encode_ms=ms_since(t4);
            if(sent!=ESP_OK)DLOG_HEAP("photo: base64 for PC failed");
            cJSON_AddBoolToObject(reply,"image_sent",sent==ESP_OK);}
        dlog("photo %s len=%u wait=%u capture=%u save=%u send_wait=%u base64=%u total=%u ms",esp_err_to_name(e),(unsigned)photo.len,waited,capture_ms,save_ms,send_wait,encode_ms,ms_since(t0));
        if(e==ESP_OK)cleanup_queue(photo.buf,photo.len,photo.width,photo.height); // jpegはここで渡す。last_photoはこのファイルのまま
        else free(photo.buf);
    }
    return e;
}
// セルフタイマー: 緑が速く点滅、SW6か停止コマンドで中止（失敗扱いにしない）
static bool countdown_wait(unsigned seconds){
    stop_recording=false;countdown_end=esp_timer_get_time()+seconds*1000000LL;countdown=true;set_phase("countdown",false,ESP_OK);
    while(esp_timer_get_time()<countdown_end&&!stop_recording)vTaskDelay(pdMS_TO_TICKS(20));
    bool cancelled=stop_recording;countdown=false;stop_recording=false;return !cancelled;
}
// 連写: 先に全部PSRAMにコピーしてから1枚ずつ保存・確認
static esp_err_t take_burst(cJSON *reply,unsigned count){
    set_phase("metering",false,ESP_OK);esp_err_t e=ESP_OK;
    if(!camera_ready||!sd_ready||!image_settled)return ESP_ERR_INVALID_STATE;
    uint8_t *copies[8]={0};size_t lengths[8]={0};unsigned taken=0,width=0,height=0;if(count>8)count=8;
    cleanup_make_room(0,60000); // 連写は縞除去が使っているPSRAMが要る
    camera_fb_t *old=esp_camera_fb_get();if(old)esp_camera_fb_return(old);
    camera_fb_t *frame=settled_frame(false,&e);
    while(frame){
        copies[taken]=heap_caps_malloc(frame->len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!copies[taken]){esp_camera_fb_return(frame);e=ESP_ERR_NO_MEM;break;}
        memcpy(copies[taken],frame->buf,frame->len);lengths[taken++]=frame->len;width=frame->width;height=frame->height;esp_camera_fb_return(frame);
        if(taken>=count)break;
        frame=esp_camera_fb_get();if(!frame)e=ESP_ERR_TIMEOUT;
    }
    unsigned saved=0;
    for(unsigned i=0;e==ESP_OK&&i<taken;i++){
        cJSON *target=i+1==taken?reply:cJSON_CreateObject();if(!target){e=ESP_ERR_NO_MEM;break;}
        camera_fb_t copy={.buf=copies[i],.len=lengths[i]};
        e=sd_save(&copy,target);if(target!=reply)cJSON_Delete(target);
        if(e==ESP_OK){saved++;cleanup_queue(i+1==taken?photo_copy(copies[i],lengths[i]):copies[i],lengths[i],width,height);if(i+1<taken)copies[i]=NULL;}
    }
    cJSON_AddNumberToObject(reply,"burst_saved",saved);
    dlog("burst %s taken=%u saved=%u",esp_err_to_name(e),taken,saved);if(e!=ESP_OK)DLOG_HEAP("burst failed");
    if(e==ESP_OK&&taken){set_phase("sending",false,ESP_OK);e=add_image(reply,copies[taken-1],lengths[taken-1]);}
    for(unsigned i=0;i<taken;i++)free(copies[i]);
    return e;
}
static void execute_request(cJSON *req) {
    cJSON *r=cJSON_CreateObject();
    cJSON *id=cJSON_GetObjectItem(req,"id"), *cmd=cJSON_GetObjectItem(req,"cmd");
    if(cJSON_IsNumber(id))cJSON_AddNumberToObject(r,"id",id->valueint);
    esp_err_t e=ESP_OK;
    bool reboot=false;
    // statusとlogはGUIが頻繁に呼ぶので残さない
    bool logged=cJSON_IsString(cmd)&&strcmp(cmd->valuestring,"status")&&strcmp(cmd->valuestring,"log");int64_t started=esp_timer_get_time();
    if(logged)dlog("cmd %s id=%d mode=%u",cmd->valuestring,cJSON_IsNumber(id)?id->valueint:-1,active_mode);
    if(!cJSON_IsString(cmd))e=ESP_ERR_INVALID_ARG;
    else if(!strcmp(cmd->valuestring,"log")){ // デバッグログを読む。fromから最大6000バイト（PCは pull_log.py）
        cJSON *from=cJSON_GetObjectItem(req,"from");uint32_t position=cJSON_IsNumber(from)&&from->valuedouble>0?(uint32_t)from->valuedouble:0,head=0;
        char *text=malloc(6001);
        if(!text)e=ESP_ERR_NO_MEM;
        else{size_t n=dlog_read(&position,text,6000,&head);text[n]=0;
            cJSON_AddNumberToObject(r,"from",position);cJSON_AddNumberToObject(r,"next",position+n);cJSON_AddNumberToObject(r,"head",head);
            cJSON_AddNumberToObject(r,"boot",dlog_boot());cJSON_AddStringToObject(r,"log",text);free(text);}
    }
    else if(!strcmp(cmd->valuestring,"log_clear"))dlog_clear();
    else if(!strcmp(cmd->valuestring,"led")){
        cJSON *pin=cJSON_GetObjectItem(req,"pin"), *on=cJSON_GetObjectItem(req,"on"); int index=-1;
        for(int i=0;i<5;i++)if(cJSON_IsNumber(pin)&&pin->valueint==pins[i])index=i;
        if(index<2||!cJSON_IsBool(on))e=ESP_ERR_INVALID_ARG;
        else if(true)e=ESP_ERR_NOT_SUPPORTED;
        else {lights[index]=cJSON_IsTrue(on);gpio_set_level(pins[index],!lights[index]);}
    } else if(!strcmp(cmd->valuestring,"off")){
        e=ESP_ERR_NOT_SUPPORTED;
    } else if(!strcmp(cmd->valuestring,"select")){
        if(busy||recording)e=ESP_ERR_INVALID_STATE;else select_begin();
    } else if(!strcmp(cmd->valuestring,"cancel")){select_cancel();
    } else if(!strcmp(cmd->valuestring,"stop")){
        if(recording||countdown)stop_recording=true;else e=ESP_ERR_INVALID_STATE;
    } else if(!strcmp(cmd->valuestring,"mode")){
        cJSON *value=cJSON_GetObjectItem(req,"value");
        if(!cJSON_IsNumber(value)||value->valueint<0||value->valueint>7){e=ESP_ERR_INVALID_ARG;finish_job(e);}
        else{active_mode=value->valueint;selected_mode=active_mode;selecting=false;gain_limit=camera_modes[active_mode].gain;mode_leds(active_mode,true);prepare_camera();e=last_error;if(e==ESP_OK){e=save_mode();if(e!=ESP_OK)finish_job(e);}}
    } else if(!strcmp(cmd->valuestring,"file_chunk")){e=read_video_chunk(req,r);finish_job(e==ESP_OK&&image_settled&&sd_ready?ESP_OK:e==ESP_OK?ESP_ERR_INVALID_STATE:e);
    } else if(!strcmp(cmd->valuestring,"restart")){
        portENTER_CRITICAL(&state_lock);
        if(busy)e=ESP_ERR_INVALID_STATE;
        else{busy=true;capture_ready=false;phase="restarting";reboot=true;}
        portEXIT_CRITICAL(&state_lock);
    } else if(!strcmp(cmd->valuestring,"sd")){e=sd_test();sd_ready=e==ESP_OK;finish_job(e==ESP_OK&&image_settled?ESP_OK:e==ESP_OK?ESP_ERR_INVALID_STATE:e);}
    else if(!strcmp(cmd->valuestring,"sd_info"))e=sd_probe(r);
    else if(!strcmp(cmd->valuestring,"expose")){ // 診断用: 露光・ゲイン固定+WB停止、引数なしで自動
        cJSON *value=cJSON_GetObjectItem(req,"value"),*gain=cJSON_GetObjectItem(req,"gain");
        sensor_t *s=camera_ready?esp_camera_sensor_get():NULL;bool manual=cJSON_IsNumber(value);
        if(!s)e=ESP_ERR_INVALID_STATE;
        else if(s->set_exposure_ctrl(s,!manual)||s->set_gain_ctrl(s,!manual)||s->set_whitebal(s,!manual)||
                (manual&&(s->set_aec_value(s,value->valueint)||s->set_agc_gain(s,cJSON_IsNumber(gain)?gain->valueint:0))))e=ESP_FAIL;
        finish_job(e==ESP_OK&&image_settled&&sd_ready?ESP_OK:e);
    }
    else if(!strcmp(cmd->valuestring,"sd_speed")){ // 診断用: 次のマウントからのSPIクロック
        cJSON *khz=cJSON_GetObjectItem(req,"khz");
        if(!cJSON_IsNumber(khz)||khz->valueint<400||khz->valueint>40000)e=ESP_ERR_INVALID_ARG;
        else{xSemaphoreTake(sd_lock,portMAX_DELAY);sd_khz=khz->valueint;xSemaphoreGive(sd_lock);}
        finish_job(e==ESP_OK&&image_settled&&sd_ready?ESP_OK:e);
    }
    else if(!strcmp(cmd->valuestring,"prepare")){prepare_camera();e=last_error;}
    else if(!strcmp(cmd->valuestring,"tune")){
        cJSON *limit=cJSON_GetObjectItem(req,"gain_limit");
        if(!cJSON_IsNumber(limit)||limit->valueint<0||limit->valueint>4){e=ESP_ERR_INVALID_ARG;finish_job(e);}
        else{gain_limit=limit->valueint;prepare_camera();e=last_error;}
    }
    else if(!strcmp(cmd->valuestring,"series")){capture_series(req,r);cJSON_Delete(req);prepare_camera();return;}
    else if(!strcmp(cmd->valuestring,"reg")){ // 診断用: センサーのレジスタ読み書き（addr bit8 = バンク）
        cJSON *addr=cJSON_GetObjectItem(req,"addr"),*mask=cJSON_GetObjectItem(req,"mask"),*value=cJSON_GetObjectItem(req,"value");
        sensor_t *s=camera_ready?esp_camera_sensor_get():NULL;
        if(!s||!cJSON_IsNumber(addr))e=ESP_ERR_INVALID_ARG;
        else{if(cJSON_IsNumber(mask)&&cJSON_IsNumber(value)&&s->set_reg(s,addr->valueint,mask->valueint,value->valueint))e=ESP_FAIL;
            cJSON_AddNumberToObject(r,"reg",s->get_reg(s,addr->valueint,0xff));}
        finish_job(e==ESP_OK&&image_settled&&sd_ready?ESP_OK:e==ESP_OK?ESP_ERR_INVALID_STATE:e);
    }
    else if(!strcmp(cmd->valuestring,"grab")){ // 診断用: SDに保存せず取得
        cJSON *n=cJSON_GetObjectItem(req,"skip");int skip=cJSON_IsNumber(n)?n->valueint:3;
        camera_fb_t *frame=NULL,copy={0},*fb=NULL;
        for(int i=0;camera_ready&&i<=skip;i++){if(frame)esp_camera_fb_return(frame);frame=esp_camera_fb_get();}
        if(frame){ // take_photoと同じくコピーで処理
            copy=(camera_fb_t){.len=frame->len,.width=frame->width,.height=frame->height,.format=frame->format};
            copy.buf=photo_copy(frame->buf,frame->len);esp_camera_fb_return(frame);if(copy.buf)fb=&copy;
        }
        cJSON *clean=cJSON_GetObjectItem(req,"clean");uint8_t *cleaned=NULL;size_t cleaned_len=0;float peak=0;unsigned ms[4]={0};
        if(fb&&cJSON_IsTrue(clean)){esp_err_t d=destripe_jpeg(fb->buf,fb->len,fb->width,fb->height,94,0,&cleaned,&cleaned_len,&peak,ms);
            cJSON_AddStringToObject(r,"destripe",esp_err_to_name(d));cJSON_AddNumberToObject(r,"destripe_peak",peak);
            cJSON *steps=cJSON_AddArrayToObject(r,"destripe_ms");for(int i=0;i<4;i++)cJSON_AddItemToArray(steps,cJSON_CreateNumber(ms[i]));
            if(d==ESP_OK&&cJSON_IsTrue(cJSON_GetObjectItem(req,"verify"))){ // 同じコアで処理した結果とバイト一致するか
                uint8_t *again=NULL;size_t again_len=0;float again_peak=0;unsigned again_ms[4]={0};
                esp_err_t v=destripe_jpeg(fb->buf,fb->len,fb->width,fb->height,94,xPortGetCoreID(),&again,&again_len,&again_peak,again_ms);
                cJSON_AddBoolToObject(r,"verify_identical",v==ESP_OK&&again_len==cleaned_len&&!memcmp(again,cleaned,cleaned_len));
                cJSON_AddNumberToObject(r,"verify_single_core_ms",again_ms[3]);free(again);
            }
            if(d==ESP_OK){e=add_image(r,cleaned,cleaned_len);cJSON_AddStringToObject(r,"jpeg_raw","");}}
        if(!fb)e=ESP_ERR_INVALID_STATE;
        else if(!cleaned)e=add_image(r,fb->buf,fb->len);
        else{ // 診断用: jpeg=縞除去後, jpeg_raw=センサーそのまま
            size_t cap=4*((fb->len+2)/3)+1,w=0;unsigned char *b=malloc(cap);
            if(b&&!mbedtls_base64_encode(b,cap,&w,fb->buf,fb->len))cJSON_ReplaceItemInObject(r,"jpeg_raw",cJSON_CreateString((char*)b));
            free(b);free(cleaned);
        }
        free(copy.buf);
        finish_job(e==ESP_OK&&image_settled&&sd_ready?ESP_OK:e);
    }
    else if(!strcmp(cmd->valuestring,"camera")){
        if(camera_modes[active_mode].video){
            recording=true;stop_recording=false;record_frames=record_ms=0;set_phase("recording",false,ESP_OK);
            add_status(r,ESP_OK);send_json(r);r=cJSON_CreateObject();cJSON_AddNumberToObject(r,"id",0);cJSON_AddStringToObject(r,"event","recording");
            portENTER_CRITICAL(&state_lock);recording=true;portEXIT_CRITICAL(&state_lock); // 縞除去タスク側のチェックと対
            cleanup_wait_until_paused(); // 処理中の1枚は終わらせる（最大6秒くらい）
            e=record_video(r);recording=false;capture_failed=e!=ESP_OK;finish_job(e);
        }else if(camera_modes[active_mode].timer_s&&!countdown_wait(camera_modes[active_mode].timer_s)){
            cJSON_AddBoolToObject(r,"cancelled",true);finish_job(ESP_OK);
        }else{e=camera_modes[active_mode].burst?take_burst(r,camera_modes[active_mode].burst):take_photo(r);capture_failed=e!=ESP_OK;finish_job(e);}
    } else if(strcmp(cmd->valuestring,"status")&&strcmp(cmd->valuestring,"log")&&strcmp(cmd->valuestring,"log_clear"))e=ESP_ERR_NOT_SUPPORTED;
    if(logged)dlog("cmd %s done %s (%u ms) phase=%s",cmd->valuestring,esp_err_to_name(e),ms_since(started),phase);
    if(cJSON_IsNumber(id)&&id->valueint==0&&!cJSON_HasObjectItem(r,"event"))cJSON_AddStringToObject(r,"event",!strcmp(cmd->valuestring,"camera")?"photo":"mode");
    add_status(r,e);
    send_json(r);cJSON_Delete(req);
    if(reboot){vTaskDelay(pdMS_TO_TICKS(150));esp_restart();}
}
static bool submit(cJSON *req){
    portENTER_CRITICAL(&state_lock);
    if(busy||(active_mode==7&&strcmp(cJSON_GetObjectItem(req,"cmd")->valuestring,"mode")&&strcmp(cJSON_GetObjectItem(req,"cmd")->valuestring,"prepare"))||(selecting&&strcmp(cJSON_GetObjectItem(req,"cmd")->valuestring,"mode"))){
        const char *now=phase;bool was_busy=busy;portEXIT_CRITICAL(&state_lock);
        dlog("refused %s: busy=%d phase=%s selecting=%d",cJSON_GetObjectItem(req,"cmd")->valuestring,was_busy,now,selecting);return false;
    }
    busy=true;capture_ready=false;phase="queued";
    portEXIT_CRITICAL(&state_lock);
    if(xQueueSend(jobs,&req,0)==pdTRUE)return true;
    finish_job(ESP_ERR_NO_MEM);return false;
}
static void command(char *line){
    cJSON *req=cJSON_Parse(line);if(!req)return;
    cJSON *cmd=cJSON_GetObjectItem(req,"cmd");
    bool job=cJSON_IsString(cmd)&&(!strcmp(cmd->valuestring,"mode")||!strcmp(cmd->valuestring,"file_chunk")||!strcmp(cmd->valuestring,"camera")||!strcmp(cmd->valuestring,"prepare")||!strcmp(cmd->valuestring,"tune")||!strcmp(cmd->valuestring,"sd")||!strcmp(cmd->valuestring,"sd_info")||!strcmp(cmd->valuestring,"sd_speed")||!strcmp(cmd->valuestring,"expose")||!strcmp(cmd->valuestring,"series")||!strcmp(cmd->valuestring,"reg")||!strcmp(cmd->valuestring,"grab"));
    if(!job){execute_request(req);return;}
    if(submit(req))return;
    cJSON *r=cJSON_CreateObject(),*id=cJSON_GetObjectItem(req,"id");if(cJSON_IsNumber(id))cJSON_AddNumberToObject(r,"id",id->valueint);
    add_status(r,ESP_ERR_INVALID_STATE);send_json(r);cJSON_Delete(req);
}
static void camera_worker(void *arg){
    (void)arg;prepare_camera();dlog_kick();cJSON *req;
    while(1){
        if(xQueueReceive(jobs,&req,portMAX_DELAY)==pdTRUE){
            cJSON *cmd=cJSON_GetObjectItem(req,"cmd");bool info=cJSON_IsString(cmd)&&!strcmp(cmd->valuestring,"sd_info");
            execute_request(req);
            if(info)finish_job((active_mode==7?ap_is_running():image_settled)&&sd_ready?ESP_OK:ESP_ERR_INVALID_STATE);
            dlog_kick();
        }
    }
}
static void input_worker(void *arg){
    (void)arg;
    while(1){
        tick++;
        for(int i=0;i<2;i++){
            bool pressed=!gpio_get_level(47+i);
            if(pressed!=candidate[i]){candidate[i]=pressed;changed[i]=tick;}
            if(tick-changed[i]>=(i==1?6:3)&&stable[i]!=pressed){
                stable[i]=pressed;
                if(pressed){counts[i]++;
                    dlog("%s pressed ready=%d busy=%d phase=%s recording=%d selecting=%d",i==0?"SW6":"SW7",capture_ready,busy,phase,recording,selecting);
                    if(i==0){
                        if(recording||countdown)stop_recording=true;
                        else if(selecting)select_cancel();
                        else if(capture_ready&&active_mode!=7){cJSON *req=cJSON_CreateObject();cJSON_AddNumberToObject(req,"id",0);cJSON_AddStringToObject(req,"cmd","camera");if(!submit(req))cJSON_Delete(req);}
                        else if(halted()){cJSON *req=cJSON_CreateObject();cJSON_AddNumberToObject(req,"id",0);cJSON_AddStringToObject(req,"cmd","prepare");if(!submit(req))cJSON_Delete(req);}
                    }else if(!busy){
                        if(!selecting)select_begin();
                        else{cJSON *req=cJSON_CreateObject();cJSON_AddNumberToObject(req,"id",0);cJSON_AddStringToObject(req,"cmd","mode");cJSON_AddNumberToObject(req,"value",selected_mode);if(!submit(req))cJSON_Delete(req);}
                    }
                }
            }
        }
        if(tick%2==0){
            unsigned samples[5];bool valid=true;
            for(int n=0;n<5;n++){int raw=0;if(adc_oneshot_read(adc,ADC_CHANNEL_1,&raw)!=ESP_OK){valid=false;break;}samples[n]=raw;}
            if(valid){unsigned value=median_five(samples);portENTER_CRITICAL(&state_lock);
                selector_sample(&rv_selector,value);
                if(selecting)selected_mode=selector_choose(&rv_selector,selected_mode,rv_max);
                portEXIT_CRITICAL(&state_lock);
            }
            // オレンジはモード番号（選択中は候補）、状態は緑で出す
            mode_leds(selecting?selected_mode:active_mode,true);
        }
        update_indicators();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void app_main(void){
    dlog_init(esp_reset_reason());
    esp_err_t storage=nvs_flash_init(); // NVSは自動で消さない
    if(storage==ESP_OK){nvs_handle_t h;if(nvs_open("camera",NVS_READONLY,&h)==ESP_OK){uint8_t mode=0;if(nvs_get_u8(h,"mode",&mode)==ESP_OK&&mode<8)active_mode=mode;nvs_close(h);}}
    dlog("start mode=%u (%s) nvs=%s",active_mode,camera_modes[active_mode].name,esp_err_to_name(storage));DLOG_HEAP("start");
    gain_limit=camera_modes[active_mode].gain;selected_mode=active_mode;
    for(int i=0;i<5;i++){gpio_set_level(pins[i],1);gpio_set_direction(pins[i],GPIO_MODE_OUTPUT);}
    for(int i=47;i<=48;i++){gpio_set_direction(i,GPIO_MODE_INPUT);gpio_set_pull_mode(i,GPIO_PULLUP_ONLY);}
    adc_oneshot_unit_init_cfg_t ac={.unit_id=ADC_UNIT_1};ESP_ERROR_CHECK(adc_oneshot_new_unit(&ac,&adc));
    adc_oneshot_chan_cfg_t cc={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_12};ESP_ERROR_CHECK(adc_oneshot_config_channel(adc,ADC_CHANNEL_1,&cc));
    usb_serial_jtag_driver_config_t uc={.tx_buffer_size=4096,.rx_buffer_size=1024};ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&uc));
    usb_mutex=xSemaphoreCreateMutex();jobs=xQueueCreate(1,sizeof(cJSON*));
    sd_lock=xSemaphoreCreateMutex();cleanup_jobs=xQueueCreate(16,sizeof(cleanup_job));
    configASSERT(usb_mutex&&jobs&&sd_lock&&cleanup_jobs);
    ESP_ERROR_CHECK(destripe_init());
    configASSERT(xTaskCreatePinnedToCore(cleanup_worker,"cleanup",16384,NULL,1,NULL,1)==pdPASS);
    configASSERT(xTaskCreate(camera_worker,"camera",20480,NULL,3,NULL)==pdPASS);
    configASSERT(xTaskCreate(input_worker,"buttons",4096,NULL,4,NULL)==pdPASS);
    configASSERT(xTaskCreatePinnedToCore(dlog_sd_worker,"dlog_sd",4096,NULL,1,&dlog_task,0)==pdPASS);
    char line[256];size_t used=0;bool overflow=false;
    while(1){
        char b[64];int n=usb_serial_jtag_read_bytes(b,sizeof(b),pdMS_TO_TICKS(10));
        for(int i=0;i<n;i++){
            if(b[i]=='\n'){if(!overflow){line[used]=0;command(line);}used=0;overflow=false;}
            else if(b[i]!='\r'){if(used<sizeof(line)-1)line[used++]=b[i];else overflow=true;}
        }
    }
}
