#pragma once
#include <stddef.h>
#include <stdint.h>
// 撮影の失敗を後から追うためのログ。内部RAMのnoinit領域に貯めるので、パニックやWDTの再起動でも前の分が残る（電源を切ると消える）
// ESP_LOGとROMのprintf（カメラドライバの割り込み内の警告など）もここに入る。SDの/DEBUG.LOGへの追記はmain.c
// 位置は起動をまたいで通しのバイト数。古い分は上書きされる
void dlog_init(int reset_reason);
void dlog(const char *fmt,...) __attribute__((format(printf,1,2)));
// *from（残っている一番古い位置より前なら繰り上げる）から最大capバイトをoutにコピーして、バイト数を返す
size_t dlog_read(uint32_t *from,char *out,size_t cap,uint32_t *head);
uint32_t dlog_head(void);
unsigned dlog_boot(void);
void dlog_clear(void);
// SDに書き終えた位置（これもnoinit）
uint32_t dlog_sd_mark(void);
void dlog_set_sd_mark(uint32_t position);
