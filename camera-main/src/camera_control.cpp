#include "camera_control.h"
#include "sd_card.h"
#include "led_control.h"

// カメラバッファ
camera_fb_t *fb = NULL;
bool camera_initialized = false;

// 撮影状態管理
bool camera_error_state = false;
unsigned long last_photo_time = 0;

void initCamera()
{
    Serial.println("[Camera] Starting initialization...");

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = CAMERA_XCLK_FREQ;
    config.pixel_format = CAMERA_PIXEL_FORMAT;
    config.frame_size = CAMERA_FRAME_SIZE;
    config.jpeg_quality = CAMERA_JPEG_QUALITY;
    config.fb_count = CAMERA_FB_COUNT;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("[Camera] Initialization failed! Error code: 0x%x\n", err);
        camera_initialized = false;
        return;
    }

    camera_initialized = true;
    Serial.println("[Camera] Initialized successfully");
}

void warmupCamera()
{
    if (!camera_initialized)
    {
        Serial.println("[Camera] Cannot warmup - not initialized");
        return;
    }

    Serial.println("[Camera] Warming up - discarding 5 frames for AE/AWB stabilization...");
    for (int i = 0; i < 5; i++)
    {
        camera_fb_t *temp_fb = esp_camera_fb_get();
        if (temp_fb)
        {
            Serial.printf("[Camera] Warmup frame %d/%d discarded (%u bytes)\n", i + 1, 5, temp_fb->len);
            esp_camera_fb_return(temp_fb);
        }
        else
        {
            Serial.printf("[Camera] Warmup frame %d/%d failed to capture\n", i + 1, 5);
        }
        delay(300);
    }
    Serial.println("[Camera] Warmup complete - sensor ready");
}

void handleCameraButton()
{
    static bool button47_prev = false;
    bool button47_pressed = (digitalRead(BUTTON_47) == LOW);

    if (button47_pressed && !button47_prev)
    {
        Serial.println("[Button 47 PRESSED]");

        // エラー状態の場合、エラーをリセット
        if (camera_error_state)
        {
            camera_error_state = false;
            Serial.println("[Button 47] Error state cleared");
        }
        else
        {
            // 撮影間隔チェック
            unsigned long current_time = millis();
            if (current_time - last_photo_time < PHOTO_COOLDOWN_MS)
            {
                Serial.printf("[Button 47] Cooldown active, wait %lu ms\n", PHOTO_COOLDOWN_MS - (current_time - last_photo_time));
                return;
            }

            Serial.println("[Button 47] Taking photo...");

            // 撮影開始: IO46を即座に消灯
            updateCameraStatusLED(false, false);

            if (!camera_initialized)
            {
                Serial.println("[Button 47] Camera not initialized, attempting to initialize...");
                initCamera();
            }

            if (camera_initialized)
            {
                // 古いフレームバッファをクリア + AE/AWB安定化のため複数フレーム破棄
                Serial.println("[Camera] Warming up sensor (AE/AWB stabilization)...");
                for (int i = 0; i < 5; i++)
                {
                    camera_fb_t *temp_fb = esp_camera_fb_get();
                    if (temp_fb)
                    {
                        esp_camera_fb_return(temp_fb);
                    }
                    delay(50); // センサーの露出/ホワイトバランス調整時間
                }

                // 新しいフレームを取得
                fb = esp_camera_fb_get();
                if (fb)
                {
                    Serial.printf("[Button 47] Photo captured: %u bytes\n", fb->len);

                    // SDカードに保存
                    if (savePhotoToSD(fb->buf, fb->len))
                    {
                        Serial.println("[Button 47] Photo saved to SD card");
                        last_photo_time = millis(); // 撮影時間を記録
                    }
                    else
                    {
                        Serial.println("[Button 47] Failed to save photo to SD card");
                        camera_error_state = true;          // エラー状態に設定
                        updateCameraStatusLED(false, true); // 即座にエラーLED点灯
                    }

                    // フレームバッファを解放
                    esp_camera_fb_return(fb);
                    fb = NULL;
                }
                else
                {
                    Serial.println("[Button 47] Photo capture failed");
                    camera_error_state = true;          // エラー状態に設定
                    updateCameraStatusLED(false, true); // 即座にエラーLED点灯
                }
            }
            else
            {
                Serial.println("[Button 47] Camera initialization failed");
                camera_error_state = true;          // エラー状態に設定
                updateCameraStatusLED(false, true); // 即座にエラーLED点灯
            }
        }

        button47_prev = true;
    }
    else if (!button47_pressed)
    {
        button47_prev = false;
    }
}

void handlePhoto(WebServer &server)
{
    if (!camera_initialized)
    {
        server.send(503, "text/plain", "Camera not initialized");
        return;
    }

    if (!fb || fb->len == 0)
    {
        server.send(404, "text/plain", "No photo captured");
        return;
    }

    server.sendHeader("Content-Type", "image/jpeg");
    server.sendHeader("Content-Length", String(fb->len));
    server.sendHeader("Content-Disposition", "inline; filename=photo.jpg");

    server.client().write(fb->buf, fb->len);

    esp_camera_fb_return(fb);
    fb = NULL;

    Serial.println("[Photo] Sent to client");
}

void handleCameraStream(WebServer &server)
{
    String html = R"DELIMITER(
<!DOCTYPE html>
<html>
<head>
    <title>Camera Stream</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; text-align: center; margin: 20px; background-color: #f0f0f0; }
        .container { max-width: 800px; margin: 0 auto; background-color: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        h1 { color: #333; }
        img { max-width: 100%; height: auto; border: 2px solid #ddd; border-radius: 4px; margin-top: 20px; }
        p { color: #666; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Camera Stream</h1>
        <p>Press Button 47 on the device to capture a photo</p>
        <img id="photo" src="/photo" alt="Camera photo" style="display: none;">
        <p id="status">Waiting for photo...</p>
    </div>
    <script>
        function refreshPhoto() {
            var img = document.getElementById('photo');
            img.src = '/photo?' + new Date().getTime();
            img.style.display = 'block';
            document.getElementById('status').innerText = 'Photo captured at ' + new Date().toLocaleTimeString();
        }
        setInterval(refreshPhoto, 1000);
    </script>
</body>
</html>
)DELIMITER";
    server.send(200, "text/html", html);
}
