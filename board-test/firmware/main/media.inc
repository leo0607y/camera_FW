// main.cからinclude。media_mountからmedia_unmountまでsd_lockを持つ
static esp_err_t media_mount(sdmmc_card_t **card){
    xSemaphoreTake(sd_lock,portMAX_DELAY);
    spi_bus_config_t bus={.mosi_io_num=11,.miso_io_num=13,.sclk_io_num=12,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096};
    esp_err_t e=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO);if(e!=ESP_OK){xSemaphoreGive(sd_lock);return e;}
    sdmmc_host_t host=SDSPI_HOST_DEFAULT();host.slot=SPI2_HOST;host.max_freq_khz=sd_khz;
    sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();slot.gpio_cs=10;slot.host_id=SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mount={.format_if_mount_failed=false,.max_files=3,.allocation_unit_size=16384};
    e=esp_vfs_fat_sdspi_mount("/sdcard",&host,&slot,&mount,card);
    if(e!=ESP_OK){spi_bus_free(SPI2_HOST);xSemaphoreGive(sd_lock);}
    return e;
}
static esp_err_t media_unmount(sdmmc_card_t *card){esp_err_t e=esp_vfs_fat_sdcard_unmount("/sdcard",card);spi_bus_free(SPI2_HOST);xSemaphoreGive(sd_lock);return e;}
static esp_err_t record_video(cJSON *reply){
    if(!camera_ready||!sd_ready||!image_settled)return ESP_ERR_INVALID_STATE;
    sdmmc_card_t *card=NULL;esp_err_t e=media_mount(&card);if(e!=ESP_OK){sd_ready=false;return e;}
    char temporary[96]={0},final[96]={0};FILE *file=NULL;
    avi_index *index=heap_caps_malloc(12000*sizeof(avi_index),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    avi_writer avi={0};uint8_t *preview=NULL;size_t preview_len=0;
    if(!index)e=ESP_ERR_NO_MEM;
    if(e==ESP_OK)e=ensure_session_folder();
    for(unsigned n=1;e==ESP_OK&&n<=99999;n++){
        snprintf(temporary,sizeof(temporary),"%s/VID%05u.TMP",session_folder,n);
        snprintf(final,sizeof(final),"%s/VID%05u.AVI",session_folder,n);struct stat st;
        if(stat(final,&st)==0||stat(temporary,&st)==0)continue;
        file=fopen(temporary,"wx");if(!file)e=ESP_FAIL;break;
    }
    if(e==ESP_OK&&!file)e=ESP_ERR_NO_MEM;
    if(file){setvbuf(file,NULL,_IOFBF,16384);}
    bool lapse=camera_modes[active_mode].timelapse;
    int64_t start=esp_timer_get_time(),first=0,last=0,next=start,last_sync=start;
    while(e==ESP_OK){
        if(avi.frames&&(stop_recording||esp_timer_get_time()-start>=(lapse?3600000000LL:300000000LL)||ftell(file)>240*1024*1024||avi.frames>=12000))break;
        while(esp_timer_get_time()<next&&!stop_recording){record_ms=(esp_timer_get_time()-start)/1000;vTaskDelay(pdMS_TO_TICKS(10));}
        if(stop_recording&&avi.frames)break;
        int64_t frame_started=esp_timer_get_time();
        camera_fb_t *frame=esp_camera_fb_get();
        if(!frame){e=ESP_ERR_TIMEOUT;break;}
        if(!avi.frames&&!avi_begin(&avi,file,frame->width,frame->height,index,12000))e=ESP_FAIL;
        if(e==ESP_OK&&!avi_append(&avi,frame->buf,frame->len))e=ESP_FAIL;
        int64_t now=esp_timer_get_time();if(!first)first=now;last=now;
        if(e==ESP_OK){
            record_frames=avi.frames;record_ms=(now-start)/1000;
            if(!preview){preview=heap_caps_malloc(frame->len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(preview){memcpy(preview,frame->buf,frame->len);preview_len=frame->len;}}
            if(now-last_sync>1000000){if(fflush(file)||fsync(fileno(file)))e=ESP_FAIL;last_sync=now;}
        }
        esp_camera_fb_return(frame);
        next=frame_started+(lapse?2000000:100000); // 取得とSD書き込みも含めて間隔を取る。遅れても詰めて撮らない
    }
    set_phase("finalizing",false,e);uint32_t crc=0,bytes=0;
    uint32_t period=lapse?100000:(avi.frames>1?(last-first)/(avi.frames-1):100000);
    if(e==ESP_OK&&(!avi_finish(&avi,period)||fsync(fileno(file))))e=ESP_FAIL;
    if(file&&fclose(file))e=ESP_FAIL;
    if(e==ESP_OK){file=fopen(temporary,"rb");if(!file)e=ESP_FAIL;else{if(!avi_verify(file,&avi,&crc,&bytes))e=ESP_ERR_INVALID_CRC;fclose(file);}}
    if(e==ESP_OK){
        if(rename(temporary,final))e=ESP_FAIL;
        else{
            strlcpy(last_video,final+7,sizeof(last_video));last_video_crc=crc;last_video_bytes=bytes;video_count++;
            cJSON_AddStringToObject(reply,"saved_path",last_video);cJSON_AddNumberToObject(reply,"saved_bytes",bytes);cJSON_AddNumberToObject(reply,"file_crc32",crc);
            cJSON_AddNumberToObject(reply,"fps",1000000.0/period);cJSON_AddNumberToObject(reply,"recorded_frames",avi.frames);
            if(preview)add_image(reply,preview,preview_len);
        }
    }
    free(preview);free(index);esp_err_t unmount=media_unmount(card);if(e==ESP_OK)e=unmount;
    sd_ready=e==ESP_OK;mode_leds(active_mode,true);return e;
}
static esp_err_t read_video_chunk(cJSON *req,cJSON *reply){
    cJSON *off=cJSON_GetObjectItem(req,"offset");
    if(!last_video[0]||!cJSON_IsNumber(off)||off->valuedouble<0||off->valuedouble>=last_video_bytes||off->valuedouble!=off->valueint)return ESP_ERR_INVALID_ARG;
    unsigned offset=off->valueint;sdmmc_card_t *card=NULL;esp_err_t e=media_mount(&card);if(e!=ESP_OK)return e;
    char path[128];snprintf(path,sizeof(path),"/sdcard%s",last_video);FILE *f=fopen(path,"rb");
    uint8_t *data=heap_caps_malloc(32768,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!f)e=ESP_ERR_NOT_FOUND;else if(!data)e=ESP_ERR_NO_MEM;
    else if(fseek(f,offset,SEEK_SET))e=ESP_FAIL;
    else{
        unsigned expected=last_video_bytes-offset;if(expected>32768)expected=32768;
        size_t n=fread(data,1,expected,f);if(n!=expected||ferror(f))e=ESP_FAIL;
        else{e=add_image(reply,data,n); // 送り方は同じ、プレビューとは別のフィールド
            cJSON *chunk=cJSON_DetachItemFromObject(reply,"jpeg");if(chunk)cJSON_AddItemToObject(reply,"file_data",chunk);
            cJSON_AddNumberToObject(reply,"offset",offset);cJSON_AddNumberToObject(reply,"total",last_video_bytes);cJSON_AddNumberToObject(reply,"file_crc32",last_video_crc);
        }
    }
    if(f){fclose(f);}
    free(data);esp_err_t u=media_unmount(card);if(e==ESP_OK)e=u;return e;
}
