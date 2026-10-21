// main.cでmedia.incの後にinclude。撮影の合間にコア1で縞除去する
// 先にセンサーのJPEGを保存・表示してすぐReadyに戻り、あとで縞除去版に差し替える
// 縞除去版を書いて確認するまで元は残す
typedef struct {uint8_t *jpeg;size_t len;unsigned width,height;int quality;char path[112];} cleanup_job;
// jpeg（PSRAMのコピー）はキューに入らなくてもこっちで解放する。sd_saveの直後に呼ぶ
static void cleanup_queue(uint8_t *jpeg,size_t len,unsigned width,unsigned height){
    cleanup_job job={.jpeg=jpeg,.len=len,.width=width,.height=height,.quality=camera_modes[active_mode].quality<=8?94:90};
    portENTER_CRITICAL(&state_lock);snprintf(job.path,sizeof(job.path),"/sdcard%s",last_photo);cleanup_pending++;portEXIT_CRITICAL(&state_lock);
    if(!jpeg||xQueueSend(cleanup_jobs,&job,0)!=pdTRUE){free(jpeg);portENTER_CRITICAL(&state_lock);cleanup_pending--;cleanup_failed++;portEXIT_CRITICAL(&state_lock);}
}
static uint8_t *photo_copy(const uint8_t *buf,size_t len){
    uint8_t *copy=heap_caps_malloc(len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(copy)memcpy(copy,buf,len);return copy;
}
// 縞除去がPSRAMをneedバイト空けるまで待つ（need=0なら終わるまで）。タイムアウトあり
static void cleanup_make_room(size_t need,unsigned timeout_ms){
    int64_t end=esp_timer_get_time()+timeout_ms*1000LL;
    while(cleanup_pending&&esp_timer_get_time()<end&&(!need||heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<need))vTaskDelay(pdMS_TO_TICKS(50));
}
// 縞除去待ちがlimit枚未満になるまで待つ。タイムアウトあり
static void cleanup_wait_backlog(unsigned limit,unsigned timeout_ms){
    int64_t end=esp_timer_get_time()+timeout_ms*1000LL;
    while(cleanup_pending>=limit&&esp_timer_get_time()<end)vTaskDelay(pdMS_TO_TICKS(50));
}
static esp_err_t cleanup_replace(const cleanup_job *job,const uint8_t *data,size_t len){
    char cleaned[112],original[112];size_t n=strlen(job->path);
    if(n<4||strcmp(job->path+n-4,".JPG"))return ESP_ERR_INVALID_ARG;
    snprintf(cleaned,sizeof(cleaned),"%.*s.CLN",(int)n-4,job->path);snprintf(original,sizeof(original),"%.*s.ORG",(int)n-4,job->path);
    sdmmc_card_t *card=NULL;esp_err_t e=media_mount(&card);if(e!=ESP_OK)return e;
    struct stat st;
    if(stat(job->path,&st)!=0||(size_t)st.st_size!=job->len)e=ESP_ERR_NOT_FOUND; // その間に消されたか差し替えられた
    FILE *f=e==ESP_OK?fopen(cleaned,"wx"):NULL;
    if(e==ESP_OK&&!f)e=ESP_FAIL;
    if(f){
        bool good=fwrite(data,1,len,f)==len;if(fflush(f)!=0||fsync(fileno(f))!=0)good=false;if(fclose(f)!=0)good=false;
        if(good){
            f=fopen(cleaned,"rb");uint8_t buffer[2048];size_t total=0,r;uint32_t crc=0;
            if(!f)good=false;else{while((r=fread(buffer,1,sizeof(buffer),f))){crc=esp_rom_crc32_le(crc,buffer,r);total+=r;}if(ferror(f))good=false;fclose(f);}
            if(total!=len||crc!=esp_rom_crc32_le(0,data,len))good=false;
        }
        if(!good){unlink(cleaned);e=ESP_ERR_INVALID_CRC;}
        else if(rename(job->path,original)!=0){unlink(cleaned);e=ESP_FAIL;}
        else if(rename(cleaned,job->path)!=0){rename(original,job->path);unlink(cleaned);e=ESP_FAIL;}
        else unlink(original);
    }
    esp_err_t u=media_unmount(card);return e==ESP_OK?u:e;
}
// 録画中は両コアとSDを使うので新しく始めない
static volatile bool cleanup_working;
static void cleanup_wait_until_paused(void){ // recordingをセットしてから呼ぶ
    int64_t end=esp_timer_get_time()+15000000;
    while(cleanup_working&&esp_timer_get_time()<end)vTaskDelay(pdMS_TO_TICKS(20));
}
static void cleanup_worker(void *arg){
    (void)arg;cleanup_job job;
    while(1){
        if(xQueueReceive(cleanup_jobs,&job,portMAX_DELAY)!=pdTRUE)continue;
        for(bool go=false;!go;){
            portENTER_CRITICAL(&state_lock);go=!recording;if(go)cleanup_working=true;portEXIT_CRITICAL(&state_lock);
            if(!go)vTaskDelay(pdMS_TO_TICKS(200));
        }
        int64_t started=esp_timer_get_time();uint8_t *cleaned=NULL;size_t len=0;float peak;unsigned ms[4];esp_err_t e;
        for(int attempt=0;;attempt++){ // 連写やUSB転送でPSRAMが埋まっていることがあるのでしばらくリトライ
            e=destripe_jpeg(job.jpeg,job.len,job.width,job.height,job.quality,0,&cleaned,&len,&peak,ms);
            if(e!=ESP_ERR_NO_MEM||attempt>=100)break;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        int64_t encoded=esp_timer_get_time();
        if(e==ESP_OK)e=cleanup_replace(&job,cleaned,len);
        free(cleaned);free(job.jpeg);
        portENTER_CRITICAL(&state_lock);
        cleanup_pending--;if(e==ESP_OK)cleanup_done++;else cleanup_failed++;cleanup_last=e;cleanup_last_ms=(esp_timer_get_time()-started)/1000;
        if(e==ESP_OK){for(int i=0;i<4;i++)cleanup_steps_ms[i]=ms[i];cleanup_steps_ms[4]=(esp_timer_get_time()-encoded)/1000;}
        cleanup_working=false;
        portEXIT_CRITICAL(&state_lock);
    }
}
