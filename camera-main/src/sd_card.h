#ifndef SD_CARD_H
#define SD_CARD_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "config.h"

// SDカード初期化
bool initSDCard();

// SDカードにファイル数を取得
int getFileCount();

// 写真をSDカードに保存
bool savePhotoToSD(uint8_t *data, size_t length);

// SDカードの状態確認
bool isSDCardMounted();

#endif
