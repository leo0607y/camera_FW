// ホストのファイルシステムでテスト: 再マウント、再起動、NVSカウンタ消失、書き込み失敗
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>
#include <direct.h>
typedef int esp_err_t;typedef int nvs_handle_t;
enum{ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=1,ESP_ERR_NO_MEM=2,NVS_READWRITE=0};
static char session_folder[64];static const char *root;static uint32_t stored;static bool fail_commit;
static int nvs_open(const char *name,int mode,int *h){*h=1;return ESP_OK;}
static int nvs_get_u32(int h,const char *key,uint32_t *value){*value=stored;return stored?ESP_OK:ESP_ERR_NVS_NOT_FOUND;}
static int nvs_set_u32(int h,const char *key,uint32_t value){if(!fail_commit)stored=value;return ESP_OK;}
static int nvs_commit(int h){return fail_commit?ESP_FAIL:ESP_OK;}
static void nvs_close(int h){}
static size_t copy_string(char *out,const char *in,size_t size){snprintf(out,size,"%s",in);return strlen(in);}
static const char *mapped(const char *path,char *out){assert(!strncmp(path,"/sdcard",7));snprintf(out,1024,"%s%s",root,path+7);return out;}
static int mapped_mkdir(const char *path,int mode){char buf[1024];return _mkdir(mapped(path,buf));}
static DIR *mapped_opendir(const char *path){char buf[1024];return opendir(mapped(path,buf));}
#define strlcpy copy_string
#define mkdir mapped_mkdir
#define opendir mapped_opendir
#include "main/session_storage.inc"
#undef opendir
int main(int argc,char **argv){
    assert(argc==2);root=argv[1];
    assert(ensure_session_folder()==ESP_OK&&!strcmp(session_folder,"/sdcard/DCIM/CAMERA/S0000001"));
    assert(ensure_session_folder()==ESP_OK&&stored==1); // モード切替・再マウントでは新しく割り当てない
    session_folder[0]=0;
    assert(ensure_session_folder()==ESP_OK&&!strcmp(session_folder,"/sdcard/DCIM/CAMERA/S0000002"));
    stored=0;session_folder[0]=0;
    assert(ensure_session_folder()==ESP_OK&&!strcmp(session_folder,"/sdcard/DCIM/CAMERA/S0000003"));
    session_folder[0]=0;fail_commit=true;
    assert(ensure_session_folder()!=ESP_OK&&!session_folder[0]);
    fail_commit=false;assert(ensure_session_folder()==ESP_OK&&!strcmp(session_folder,"/sdcard/DCIM/CAMERA/S0000005"));
    puts("PASS: one folder per boot, same-boot reuse, SD collision protection, NVS failure");
}
