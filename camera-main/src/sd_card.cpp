#include "sd_card.h"

static bool sd_card_mounted = false;

bool initSDCard()
{
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
        file = root.openNextFile();
    }
    root.close();

    Serial.printf("[SD] Found %d image files\n", count);
    return count;
}

bool savePhotoToSD(uint8_t *data, size_t length)
{
    if (!sd_card_mounted)
    {
        Serial.println("[SD] SD card not mounted");
        return false;
    }

    // ファイル数を取得してファイル名を生成
    int fileCount = getFileCount();
    String fileName = "/" + String(fileCount + 1) + ".jpg";

    Serial.printf("[SD] Saving photo as: %s\n", fileName.c_str());

    // ファイルを書き込みモードで開く
    File file = SD.open(fileName.c_str(), FILE_WRITE);
    if (!file)
    {
        Serial.println("[SD] Failed to open file for writing");
        return false;
    }

    // データを書き込み
    size_t written = file.write(data, length);
    file.close();

    if (written == length)
    {
        Serial.printf("[SD] Photo saved successfully (%u bytes)\n", written);
        return true;
    }
    else
    {
        Serial.printf("[SD] Write failed! Expected %u bytes, wrote %u bytes\n", length, written);
        return false;
    }
}

bool isSDCardMounted()
{
    return sd_card_mounted;
}
