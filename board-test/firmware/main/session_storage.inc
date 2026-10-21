// 起動後最初のSDマウントでフォルダを割り当てる。モード切替では変えない
static bool session_name(const char *name,unsigned *number){
    if(strlen(name)!=8||name[0]!='S')return false;
    unsigned n=0;for(int i=1;i<8;i++){if(name[i]<'0'||name[i]>'9')return false;n=n*10+name[i]-'0';}
    if(!n)return false;
    if(number)*number=n;
    return true;
}
static esp_err_t ensure_session_folder(void){
    if(session_folder[0])return ESP_OK;
    if(mkdir("/sdcard/DCIM",0777)&&errno!=EEXIST)return ESP_FAIL;
    if(mkdir("/sdcard/DCIM/CAMERA",0777)&&errno!=EEXIST)return ESP_FAIL;
    nvs_handle_t handle;esp_err_t e=nvs_open("camera",NVS_READWRITE,&handle);if(e!=ESP_OK)return e;
    uint32_t highest=0;e=nvs_get_u32(handle,"session",&highest);if(e==ESP_ERR_NVS_NOT_FOUND)e=ESP_OK;
    DIR *dir=e==ESP_OK?opendir("/sdcard/DCIM/CAMERA"):NULL;
    if(e==ESP_OK&&!dir)e=ESP_FAIL;
    if(dir){struct dirent *entry;while((entry=readdir(dir))){unsigned n;if(session_name(entry->d_name,&n)&&n>highest)highest=n;}closedir(dir);}
    char path[64]={0};
    while(e==ESP_OK&&highest<9999999){
        highest++;snprintf(path,sizeof(path),"/sdcard/DCIM/CAMERA/S%07lu",(unsigned long)highest);
        if(mkdir(path,0777)==0)break;
        if(errno!=EEXIST){e=ESP_FAIL;break;}
        path[0]=0;
    }
    if(e==ESP_OK&&!path[0])e=ESP_ERR_NO_MEM;
    if(e==ESP_OK){e=nvs_set_u32(handle,"session",highest);if(e==ESP_OK)e=nvs_commit(handle);}
    if(e==ESP_OK)strlcpy(session_folder,path,sizeof(session_folder));
    nvs_close(handle);return e;
}
