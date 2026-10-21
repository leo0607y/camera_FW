#include "sd_card.h"
#include <string.h>
#include "storage_lock.h"

static bool sd_card_mounted = false;
static uint32_t next_photo_number = 1;
static uint32_t generation = 0;
static uint32_t revision = 0;

bool initSDCard()
{
    StorageGuard guard;
    ++generation;
    // 抜き差し後に復旧できるように前のマウントを解除
    sd_card_mounted = false;
    SD.end();
    SPI.end();
    next_photo_number = 1;
    Serial.println("[SD] Initializing SD card...");

    // CS, MOSI, SCLK ピンをプルアップ
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    pinMode(SD_MOSI, OUTPUT);
    pinMode(SD_SCLK, OUTPUT);
    pinMode(SD_MISO, INPUT_PULLUP);

    delay(10);

    // SPI設定（ピン順: SCLK, MISO, MOSI, CS）
    // 周波数を低めに設定（4MHz）
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    SPI.setFrequency(4000000); // 4MHz
    delay(100);                // SPI安定化待機

    // SDカードマウント（SPIモード、SPIインスタンス、周波数指定）
    if (!SD.begin(SD_CS, SPI, 4000000))
    {
        Serial.println("[SD] Card Mount Failed");
        sd_card_mounted = false;
        return false;
    }

    uint8_t cardType = SD.cardType();

    if (cardType == CARD_NONE)
    {
        Serial.println("[SD] No SD card attached");
        sd_card_mounted = false;
        SD.end();
        return false;
    }

    Serial.print("[SD] SD Card Type: ");
    if (cardType == CARD_MMC)
    {
        Serial.println("MMC");
    }
    else if (cardType == CARD_SD)
    {
        Serial.println("SDSC");
    }
    else if (cardType == CARD_SDHC)
    {
        Serial.println("SDHC");
    }
    else
    {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("[SD] SD Card Size: %lluMB\n", cardSize);

    sd_card_mounted = true;
    Serial.println("[SD] SD card initialized successfully");
    return true;
}

int getFileCount()
{
    StorageGuard guard;
    if (!sd_card_mounted)
    {
        Serial.println("[SD] SD card not mounted");
        return 0;
    }

    File root = SD.open("/");
    if (!root)
    {
        Serial.println("[SD] Failed to open root directory");
        return 0;
    }

    int count = 0;
    File file = root.openNextFile();
    while (file)
    {
        if (!file.isDirectory())
        {
            // .jpgファイルのみカウント
            String fileName = String(file.name());
            if (fileName.endsWith(".jpg") || fileName.endsWith(".JPG"))
            {
                count++;
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    Serial.printf("[SD] Found %d image files\n", count);
    return count;
}

bool savePhotoToSD(uint8_t *data, size_t length)
{
    StorageGuard guard;
    if (!sd_card_mounted)
    {
        Serial.println("[SD] SD card not mounted");
        return false;
    }

    if (data == nullptr || length == 0)
    {
        Serial.println("[SD] Empty photo rejected");
        return false;
    }

    // 既存の写真と書き込み途中の.tmpの番号は使わない
    String fileName;
    String temporaryName;
    for (;;)
    {
        if (next_photo_number == 0)
        {
            Serial.println("[SD] Photo numbers exhausted");
            return false;
        }
        const String base = "/" + String(next_photo_number++);
        fileName = base + ".jpg";
        temporaryName = base + ".tmp";
        if (!SD.exists(fileName.c_str()) && !SD.exists((base + ".JPG").c_str()) &&
            !SD.exists(temporaryName.c_str()))
        {
            break;
        }
    }

    Serial.printf("[SD] Saving photo as: %s\n", fileName.c_str());

    // ファイルを書き込みモードで開く
    File file = SD.open(temporaryName.c_str(), FILE_WRITE);
    if (!file)
    {
        Serial.println("[SD] Failed to open file for writing");
        sd_card_mounted = false;
        return false;
    }

    // データを書き込み
    size_t written = file.write(data, length);
    file.flush();
    file.close();

    // 開き直して少しずつ比較してから.jpgにする
    File saved = SD.open(temporaryName.c_str(), FILE_READ);
    bool complete = saved && saved.size() == length && written == length;
    uint8_t verification_buffer[512];
    size_t verified = 0;
    while (complete && verified < length)
    {
        const size_t remaining = length - verified;
        const size_t chunk = remaining < sizeof(verification_buffer) ? remaining : sizeof(verification_buffer);
        const size_t read = saved.read(verification_buffer, chunk);
        complete = read == chunk && memcmp(verification_buffer, data + verified, chunk) == 0;
        verified += read;
    }
    saved.close();
    if (!complete)
    {
        SD.remove(temporaryName.c_str());
        sd_card_mounted = false;
        Serial.printf("[SD] Incomplete write: expected %u bytes, wrote %u\n", length, written);
        return false;
    }
    if (SD.exists(fileName.c_str()) || !SD.rename(temporaryName.c_str(), fileName.c_str()))
    {
        // 復旧用に.tmpを残す
        sd_card_mounted = false;
        Serial.println("[SD] Could not finalize photo; temporary file retained");
        return false;
    }
    Serial.printf("[SD] Photo saved successfully (%u bytes)\n", written);
    ++revision;
    return true;
}

bool isSDCardMounted()
{
    StorageGuard guard;
    return sd_card_mounted;
}

uint32_t sdGeneration() { StorageGuard guard; return generation; }
uint32_t sdRevision() { StorageGuard guard; return revision; }
