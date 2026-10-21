#include "debug_log.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define DLOG_SIZE (24*1024)
#define DLOG_MAGIC 0x444c4f47u
typedef struct {uint32_t magic,head,start,sd_mark,boots;char data[DLOG_SIZE];} dlog_store;
static __NOINIT_ATTR dlog_store store;
static DRAM_ATTR portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR bool rom_line_start=true;

// ROMのprintfは割り込み内（キャッシュ無効中も）から来るのでIRAMに置き、ロックの中では関数を呼ばない
static inline IRAM_ATTR void put_locked(const char *s,size_t n){
    for(size_t i=0;i<n;i++)store.data[(store.head+i)%DLOG_SIZE]=s[i];
    store.head+=n;
    if(store.head-store.start>DLOG_SIZE)store.start=store.head-DLOG_SIZE;
}
static void IRAM_ATTR rom_putc(char c){
    portENTER_CRITICAL_SAFE(&lock);
    if(rom_line_start&&c!='\n'){char mark[2]={'~',' '};put_locked(mark,2);} // ROM経由の行の印
    put_locked(&c,1);rom_line_start=c=='\n';
    portEXIT_CRITICAL_SAFE(&lock);
}
static int log_vprintf(const char *fmt,va_list ap){ // ESP_LOGの1行（"W (1234) tag: ...\n"）
    char line[192];int n=vsnprintf(line,sizeof(line),fmt,ap);
    if(n<0)return n;
    if(n>=(int)sizeof(line)){n=sizeof(line)-1;line[n-1]='\n';}
    portENTER_CRITICAL_SAFE(&lock);put_locked(line,n);portEXIT_CRITICAL_SAFE(&lock);
    return n;
}
void dlog_init(int reset_reason){
    // 電源投入時は中身が不定。それ以外の再起動では前の起動のログを残す
    bool keep=reset_reason!=ESP_RST_POWERON&&reset_reason!=ESP_RST_BROWNOUT&&store.magic==DLOG_MAGIC&&
              store.head-store.start<=DLOG_SIZE&&store.sd_mark<=store.head;
    if(!keep){store.magic=DLOG_MAGIC;store.head=store.start=store.sd_mark=store.boots=0;}
    store.boots++;
    esp_log_set_vprintf(log_vprintf);
    esp_rom_install_channel_putc(1,rom_putc);
    dlog("=== boot %u reset_reason=%d kept=%d", (unsigned)store.boots,reset_reason,keep);
}
void dlog(const char *fmt,...){
    char line[192];int64_t ms=esp_timer_get_time()/1000;
    int n=snprintf(line,sizeof(line),"b%u %lld.%03lld ",(unsigned)store.boots,ms/1000,ms%1000);
    va_list ap;va_start(ap,fmt);int m=vsnprintf(line+n,sizeof(line)-n,fmt,ap);va_end(ap);
    if(m>0)n+=m;
    if(n>(int)sizeof(line)-1)n=sizeof(line)-1;
    line[n++]='\n';
    portENTER_CRITICAL_SAFE(&lock);put_locked(line,n);portEXIT_CRITICAL_SAFE(&lock);
}
size_t dlog_read(uint32_t *from,char *out,size_t cap,uint32_t *head){
    portENTER_CRITICAL(&lock);
    uint32_t h=store.head;
    if(*from<store.start)*from=store.start;
    if(*from>h)*from=h;
    size_t n=h-*from;if(n>cap)n=cap;
    for(size_t i=0;i<n;i++)out[i]=store.data[(*from+i)%DLOG_SIZE];
    portEXIT_CRITICAL(&lock);
    if(head)*head=h;
    return n;
}
uint32_t dlog_head(void){return store.head;}
unsigned dlog_boot(void){return store.boots;}
void dlog_clear(void){portENTER_CRITICAL(&lock);store.start=store.head;portEXIT_CRITICAL(&lock);}
uint32_t dlog_sd_mark(void){return store.sd_mark;}
void dlog_set_sd_mark(uint32_t position){store.sd_mark=position;}
