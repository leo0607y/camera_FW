#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
// OV2640の横縞を消してJPEGを作り直す
// 成功時の*outは呼び出し側が持つPSRAMバッファ。失敗したら元のを使う
// 起動時に1回呼ぶ（コアごとの補助タスクを作る）
esp_err_t destripe_init(void);
// helper_coreで推定と補正、呼び出し側でデコードと圧縮
// ms: デコード, 推定, 補正+圧縮, 合計
esp_err_t destripe_jpeg(const uint8_t *jpeg,size_t len,unsigned width,unsigned height,int quality,int helper_core,uint8_t **out,size_t *out_len,float *peak,unsigned ms[4]);
