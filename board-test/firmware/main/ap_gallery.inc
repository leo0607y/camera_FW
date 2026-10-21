#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "gallery_files.h"

extern const unsigned char gallery_html_start[] asm("_binary_gallery_html_start");
static httpd_handle_t gallery_server;
static esp_netif_t *gallery_netif;
static bool gallery_wifi_initialized;
static volatile bool ap_running;
static sdmmc_card_t *gallery_card;
static char ap_ssid[32],ap_password[32];
static esp_err_t gallery_password(void){
    // パスワードは固定値。前の自動生成の値が残っていたら1回だけ移行
    const char *chosen="12345678";
    nvs_handle_t handle;esp_err_t e=nvs_open("camera",NVS_READWRITE,&handle);if(e!=ESP_OK)return e;
    size_t size=sizeof(ap_password);e=nvs_get_str(handle,"ap_password",ap_password,&size);
    if(e==ESP_ERR_NVS_NOT_FOUND||(e==ESP_OK&&strcmp(ap_password,chosen))){
        strlcpy(ap_password,chosen,sizeof(ap_password));
        e=nvs_set_str(handle,"ap_password",ap_password);if(e==ESP_OK)e=nvs_commit(handle);
    }
    nvs_close(handle);return e;
}
static unsigned ap_clients(void){wifi_sta_list_t stations={0};return ap_running&&esp_wifi_ap_get_sta_list(&stations)==ESP_OK?stations.num:0;}
static void gallery_headers(httpd_req_t *req){
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Referrer-Policy","no-referrer");
}
static esp_err_t gallery_unavailable(httpd_req_t *req){httpd_resp_set_status(req,"503 Service Unavailable");return httpd_resp_send(req,"Transfer mode is stopping",HTTPD_RESP_USE_STRLEN);}
static esp_err_t gallery_home(httpd_req_t *req){
    gallery_headers(req);httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'self'; img-src 'self' blob:; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
    return httpd_resp_send(req,(const char*)gallery_html_start,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t gallery_albums(httpd_req_t *req){
    if(!ap_running)return gallery_unavailable(req);
    unsigned ids[64]={0},count=0,total=0,before=10000000;bool legacy=false;
    char query[32]={0},value[16]={0};
    if(httpd_req_get_url_query_str(req,query,sizeof(query))==ESP_OK&&httpd_query_key_value(query,"before",value,sizeof(value))==ESP_OK){
        char *end;unsigned long n=strtoul(value,&end,10);if(!value[0]||*end||n>10000000||n<1)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid cursor");before=n;
    }
    DIR *dir=opendir("/sdcard/DCIM/CAMERA");
    if(!dir&&errno!=ENOENT)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot read SD card");
    if(dir){struct dirent *entry;while((entry=readdir(dir))){
        unsigned n;if(gallery_filename(entry->d_name,NULL,NULL)){legacy=true;continue;}
        if(!session_name(entry->d_name,&n)||n>=before)continue;
        char path[64];snprintf(path,sizeof(path),"/sdcard/DCIM/CAMERA/%.8s",entry->d_name);struct stat st;if(stat(path,&st)||!S_ISDIR(st.st_mode))continue;
        DIR *album=opendir(path);bool has_media=false;if(album){struct dirent *media;while((media=readdir(album))){if(gallery_filename(media->d_name,NULL,NULL)){has_media=true;break;}}closedir(album);}if(!has_media)continue;
        total++;unsigned at=0;while(at<count&&ids[at]>n)at++;if(at>=64)continue;
        if(count<64)count++;
        for(unsigned i=count-1;i>at;i--){ids[i]=ids[i-1];}
        ids[at]=n;
    }closedir(dir);}
    cJSON *json=cJSON_CreateObject();if(!json)return ESP_ERR_NO_MEM;cJSON *array=cJSON_AddArrayToObject(json,"albums");
    for(unsigned i=0;i<count;i++){char id[16];snprintf(id,sizeof(id),"S%07u",ids[i]);cJSON_AddItemToArray(array,cJSON_CreateString(id));}
    cJSON_AddBoolToObject(json,"legacy",legacy);cJSON_AddBoolToObject(json,"more",total>64);cJSON_AddNumberToObject(json,"before",count?ids[count-1]:0);
    char *body=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(!body)return ESP_ERR_NO_MEM;gallery_headers(req);httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");esp_err_t e=httpd_resp_send(req,body,HTTPD_RESP_USE_STRLEN);free(body);return e;
}
static esp_err_t gallery_list(httpd_req_t *req){
    if(!ap_running)return gallery_unavailable(req);
    char query[112]={0},kind[12]={0},cursor[12]={0},album[16]="legacy";bool video=false;unsigned before=100000;
    if(httpd_req_get_url_query_len(req)>=sizeof(query))return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid query");
    if(httpd_req_get_url_query_str(req,query,sizeof(query))==ESP_OK){
        if(httpd_query_key_value(query,"album",album,sizeof(album))==ESP_ERR_HTTPD_RESULT_TRUNC)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid album");
        if(httpd_query_key_value(query,"kind",kind,sizeof(kind))==ESP_OK){if(!strcmp(kind,"video"))video=true;else if(strcmp(kind,"photo"))return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid media type");}
        if(httpd_query_key_value(query,"before",cursor,sizeof(cursor))==ESP_OK){
            char *end=NULL;unsigned long parsed=strtoul(cursor,&end,10);
            if(!cursor[0]||!end||*end||parsed<1||parsed>100000)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid cursor");
            before=parsed;
        }
    }
    if(strcmp(album,"legacy")&&!session_name(album,NULL))return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid album");
    char folder[64];snprintf(folder,sizeof(folder),"/sdcard/DCIM/CAMERA%s%s",strcmp(album,"legacy")?"/":"",strcmp(album,"legacy")?album:"");
    typedef struct{unsigned n;char name[13];} entry_t;
    entry_t entries[24]={0};unsigned count=0,total=0;
    DIR *dir=opendir(folder);
    if(!dir&&errno!=ENOENT)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot read SD card");
    if(dir){struct dirent *entry;
        while((entry=readdir(dir))){bool v;unsigned n;if(!gallery_filename(entry->d_name,&v,&n)||v!=video||n>=before)continue;
            total++;unsigned at=0;while(at<count&&entries[at].n>n)at++;
            if(at>=24)continue;
            if(count<24)count++;
            for(unsigned i=count-1;i>at;i--)entries[i]=entries[i-1];
            entries[at].n=n;strlcpy(entries[at].name,entry->d_name,sizeof(entries[at].name));
        }closedir(dir);
    }
    cJSON *json=cJSON_CreateObject();if(!json)return ESP_ERR_NO_MEM;
    cJSON *files=cJSON_AddArrayToObject(json,"files");
    for(unsigned i=0;i<count;i++){
        char path[64];snprintf(path,sizeof(path),"%.40s/%s",folder,entries[i].name);struct stat st;
        if(stat(path,&st)||!S_ISREG(st.st_mode))continue;
        cJSON *file=cJSON_CreateObject();cJSON_AddStringToObject(file,"name",entries[i].name);
        char relative[32];snprintf(relative,sizeof(relative),"%s%s%s",strcmp(album,"legacy")?album:"",strcmp(album,"legacy")?"/":"",entries[i].name);cJSON_AddStringToObject(file,"path",relative);cJSON_AddNumberToObject(file,"bytes",(double)st.st_size);cJSON_AddItemToArray(files,file);
    }
    cJSON_AddBoolToObject(json,"more",total>24);cJSON_AddNumberToObject(json,"before",count?entries[count-1].n:0);
    char *body=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(!body)return ESP_ERR_NO_MEM;
    gallery_headers(req);httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t e=httpd_resp_send(req,body,HTTPD_RESP_USE_STRLEN);free(body);return e;
}
// "NAME" か "S0000001/NAME"（ファームが作った名前）だけ。SDのパスを返す
static bool gallery_resolve(const char *start,size_t length,char path[64],const char **name,bool *video){
    char relative[32];if(length!=12&&length!=21)return false;
    memcpy(relative,start,length);relative[length]=0;size_t offset=0;
    if(length==21){char album[9];memcpy(album,relative,8);album[8]=0;if(relative[8]!='/'||!session_name(album,NULL))return false;offset=9;}
    if(!gallery_filename(relative+offset,video,NULL))return false;
    snprintf(path,64,"/sdcard/DCIM/CAMERA/%s",relative);if(name)*name=start+offset;return true;
}
static esp_err_t gallery_file(httpd_req_t *req){
    if(!ap_running)return gallery_unavailable(req);
    const char *start=req->uri+6;char path[64],name[13];const char *found;bool video;
    if(!gallery_resolve(start,strcspn(start,"?"),path,&found,&video))return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Media not found");
    memcpy(name,found,12);name[12]=0;FILE *file=fopen(path,"rb");
    if(!file)return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Media not found");
    char query[32]={0},value[8]={0};bool inline_image=!video&&httpd_req_get_url_query_str(req,query,sizeof(query))==ESP_OK&&httpd_query_key_value(query,"inline",value,sizeof(value))==ESP_OK&&!strcmp(value,"1");
    gallery_headers(req);httpd_resp_set_type(req,video?"video/x-msvideo":"image/jpeg");
    char disposition[64];snprintf(disposition,sizeof(disposition),"%s; filename=\"%s\"",inline_image?"inline":"attachment",name);
    httpd_resp_set_hdr(req,"Content-Disposition",disposition);httpd_resp_set_hdr(req,"Cache-Control",inline_image?"private, max-age=3600":"no-store");
    char *buffer=malloc(8192);esp_err_t e=buffer?ESP_OK:ESP_ERR_NO_MEM;
    if(!buffer){fclose(file);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");}
    size_t n;while(ap_running&&(n=fread(buffer,1,8192,file))){e=httpd_resp_send_chunk(req,buffer,n);if(e!=ESP_OK)break;}
    if(ferror(file)||!ap_running)e=ESP_FAIL;
    if(e==ESP_OK)e=httpd_resp_send_chunk(req,NULL,0);
    fclose(file);free(buffer);return e;
}
static esp_err_t gallery_delete(httpd_req_t *req){
    if(!ap_running)return gallery_unavailable(req);
    // 独自ヘッダーでCORSのプリフライトが必要になる（応答しない）ので、他のサイトからは消せない
    char marker[8]={0};
    if(httpd_req_get_hdr_value_str(req,"X-Camera-Action",marker,sizeof(marker))!=ESP_OK||strcmp(marker,"delete"))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Missing confirmation header");
    char query[48]={0},value[32]={0},path[64];bool video;
    if(httpd_req_get_url_query_str(req,query,sizeof(query))!=ESP_OK||httpd_query_key_value(query,"path",value,sizeof(value))!=ESP_OK)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Missing path");
    for(char *c=value;*c;c++)if(c[0]=='%'&&c[1]=='2'&&(c[2]=='F'||c[2]=='f')){c[0]='/';memmove(c+1,c+3,strlen(c+3)+1);}
    if(!gallery_resolve(value,strlen(value),path,NULL,&video))return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Media not found");
    struct stat st;if(stat(path,&st)||!S_ISREG(st.st_mode))return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"Media not found");
    if(unlink(path))return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot delete");
    // USB取得用の最後の動画が消したファイルを指さないように
    if(video&&last_video[0]&&!strcmp(last_video,path+7)){last_video[0]=0;last_video_bytes=0;last_video_crc=0;}
    gallery_headers(req);httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_send(req,"{\"deleted\":true}",HTTPD_RESP_USE_STRLEN);
}
static esp_err_t ap_gallery_stop(void){
    ap_running=false;esp_err_t e=ESP_OK;
    if(gallery_server){e=httpd_stop(gallery_server);gallery_server=NULL;}
    if(gallery_wifi_initialized){esp_err_t stop=esp_wifi_stop();if(e==ESP_OK&&stop!=ESP_ERR_WIFI_NOT_STARTED)e=stop;}
    if(gallery_card){esp_err_t unmount=media_unmount(gallery_card);gallery_card=NULL;if(e==ESP_OK)e=unmount;}
    return e;
}
static esp_err_t ap_gallery_start(void){
    esp_err_t e=media_mount(&gallery_card);if(e!=ESP_OK)return e;
    e=ensure_session_folder();if(e!=ESP_OK){ap_gallery_stop();return e;}
    if(!gallery_wifi_initialized){
        e=esp_netif_init();
        if(e==ESP_OK){e=esp_event_loop_create_default();if(e==ESP_ERR_INVALID_STATE)e=ESP_OK;}
        if(e==ESP_OK){gallery_netif=esp_netif_create_default_wifi_ap();if(!gallery_netif)e=ESP_ERR_NO_MEM;}
        if(e==ESP_OK){wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();config.nvs_enable=false;e=esp_wifi_init(&config);if(e==ESP_OK)gallery_wifi_initialized=true;}
    }
    if(e==ESP_OK){
        // IPは明示的に設定（SDKのデフォルトに頼らない）
        e=esp_netif_dhcps_stop(gallery_netif);
        if(e==ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)e=ESP_OK;
        esp_netif_ip_info_t address={.ip={.addr=ESP_IP4TOADDR(192,168,4,1)},.gw={.addr=ESP_IP4TOADDR(192,168,4,1)},.netmask={.addr=ESP_IP4TOADDR(255,255,255,0)}};
        if(e==ESP_OK)e=esp_netif_set_ip_info(gallery_netif,&address);
        if(e==ESP_OK)e=esp_netif_dhcps_start(gallery_netif);
    }
    if(e==ESP_OK){
        uint8_t mac[6];e=esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP);
        if(e==ESP_OK){
            snprintf(ap_ssid,sizeof(ap_ssid),"Camera-%02X%02X%02X",mac[3],mac[4],mac[5]);
            e=gallery_password();
            wifi_config_t config={.ap={.channel=6,.max_connection=3,.authmode=WIFI_AUTH_WPA2_PSK,.pairwise_cipher=WIFI_CIPHER_TYPE_CCMP,.pmf_cfg={.required=false}}};
            strlcpy((char*)config.ap.ssid,ap_ssid,sizeof(config.ap.ssid));config.ap.ssid_len=strlen(ap_ssid);strlcpy((char*)config.ap.password,ap_password,sizeof(config.ap.password));
            if(e==ESP_OK)e=esp_wifi_set_storage(WIFI_STORAGE_RAM);
            if(e==ESP_OK)e=esp_wifi_set_mode(WIFI_MODE_AP);
            if(e==ESP_OK)e=esp_wifi_set_config(WIFI_IF_AP,&config);
            if(e==ESP_OK)e=esp_wifi_start();
        }
    }
    if(e==ESP_OK){
        httpd_config_t config=HTTPD_DEFAULT_CONFIG();config.uri_match_fn=httpd_uri_match_wildcard;config.max_open_sockets=4;config.lru_purge_enable=true;config.stack_size=8192;config.send_wait_timeout=3;config.recv_wait_timeout=3;
        e=httpd_start(&gallery_server,&config);
        if(e==ESP_OK){
            httpd_uri_t albums={.uri="/api/albums",.method=HTTP_GET,.handler=gallery_albums};
            httpd_uri_t home={.uri="/",.method=HTTP_GET,.handler=gallery_home},list={.uri="/api/files",.method=HTTP_GET,.handler=gallery_list},file={.uri="/file/*",.method=HTTP_GET,.handler=gallery_file};
            e=httpd_register_uri_handler(gallery_server,&albums);if(e==ESP_OK)e=httpd_register_uri_handler(gallery_server,&home);if(e==ESP_OK)e=httpd_register_uri_handler(gallery_server,&list);if(e==ESP_OK)e=httpd_register_uri_handler(gallery_server,&file);
            httpd_uri_t remove={.uri="/api/delete",.method=HTTP_POST,.handler=gallery_delete};if(e==ESP_OK)e=httpd_register_uri_handler(gallery_server,&remove);
        }
    }
    if(e==ESP_OK){ap_running=true;}else{ap_gallery_stop();}
    return e;
}
