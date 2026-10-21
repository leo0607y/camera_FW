#include "camera_control.h"
#include "sd_card.h"
#include "led_control.h"
#include "esp_camera.h"
#include "debounced_button.h"
#include <atomic>

namespace
{
bool camera_initialized = false;
std::atomic<CaptureState> state{CaptureState::Starting};
std::atomic<CaptureError> error{CaptureError::None};
DebouncedButton shutter(BUTTON_DEBOUNCE_MS);
uint32_t state_started = 0;
uint32_t last_frame_at = 0;
uint8_t discarded_frames = 0;
constexpr uint8_t WARMUP_FRAMES = 5;
constexpr uint32_t STARTUP_FRAME_INTERVAL_MS = 300;
constexpr uint32_t PHOTO_FRAME_INTERVAL_MS = 50;

// スコープを抜けたらフレームを返す
class ScopedFrame
{
public:
    ScopedFrame() : frame(esp_camera_fb_get()) {}
    ~ScopedFrame() { if (frame) esp_camera_fb_return(frame); }
    ScopedFrame(const ScopedFrame &) = delete;
    ScopedFrame &operator=(const ScopedFrame &) = delete;
    bool valid() const { return frame && frame->buf && frame->len > 0; }
    camera_fb_t *get() const { return frame; }
private:
    camera_fb_t *frame;
};

void transition(CaptureState next)
{
    state = next;
    state_started = uint32_t(millis());
    updateCameraStatusLED(state == CaptureState::Ready, state == CaptureState::Error);
}

void fail(CaptureError reason)
{
    error = reason;
    transition(CaptureState::Error);
    Serial.printf("[Capture] Error: %s\n", captureErrorName(reason));
}

bool initCamera()
{
    if (camera_initialized)
    {
        return true;
    }
    Serial.println("[Camera] Starting initialization...");

    camera_config_t config = {};
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
        return false;
    }

    camera_initialized = true;
    Serial.println("[Camera] Initialized successfully");
    return true;
}


void startWarmup(CaptureState next)
{
    discarded_frames = 0;
    last_frame_at = uint32_t(millis());
    transition(next);
}

void initializeHardware()
{
    error = CaptureError::None;
    transition(CaptureState::Starting);
    if (camera_initialized)
    {
        if (esp_camera_deinit() != ESP_OK)
        {
            fail(CaptureError::CameraReset);
            return;
        }
        camera_initialized = false;
    }
    if (!initSDCard())
    {
        fail(CaptureError::StorageInit);
        return;
    }
    if (!initCamera())
    {
        fail(CaptureError::CameraInit);
        return;
    }
    startWarmup(CaptureState::WarmingUp);
}

void advanceWarmup(uint32_t now)
{
    const bool startup = state == CaptureState::WarmingUp;
    const uint32_t interval = startup ? STARTUP_FRAME_INTERVAL_MS : PHOTO_FRAME_INTERVAL_MS;
    if (discarded_frames != 0 && uint32_t(now - last_frame_at) < interval)
    {
        return;
    }
    if (discarded_frames == WARMUP_FRAMES)
    {
        transition(startup ? CaptureState::Ready : CaptureState::Capturing);
        return;
    }
    ScopedFrame frame;
    if (!frame.valid())
    {
        fail(startup ? CaptureError::CameraWarmup : CaptureError::Capture);
        return;
    }
    ++discarded_frames;
    last_frame_at = uint32_t(millis());
}

void captureAndSave()
{
    ScopedFrame frame;
    if (!frame.valid())
    {
        fail(CaptureError::Capture);
        return;
    }
    transition(CaptureState::Saving);
    if (!savePhotoToSD(frame.get()->buf, frame.get()->len))
    {
        fail(CaptureError::StorageSave);
        return;
    }
    transition(CaptureState::Cooldown);
    Serial.println("[Capture] Photo saved");
}
} // namespace

void beginCapture()
{
    initializeHardware();
    shutter.reset(digitalRead(BUTTON_47) == LOW, uint32_t(millis()));
}

void updateCapture()
{
    const uint32_t now = uint32_t(millis());
    if (state == CaptureState::Cooldown && uint32_t(now - state_started) >= PHOTO_COOLDOWN_MS)
    {
        transition(CaptureState::Ready);
    }

    // 押し始めたときに受付中だったかを覚えておく（準備中の押下を後で撮影にしない）
    const bool accepts_press = state == CaptureState::Ready || state == CaptureState::Error;
    if (shutter.update(digitalRead(BUTTON_47) == LOW, now, accepts_press))
    {
        if (state == CaptureState::Error)
        {
            beginCapture();
            return;
        }
        if (state == CaptureState::Ready)
        {
            startWarmup(CaptureState::Preparing);
        }
    }

    switch (state)
    {
    case CaptureState::WarmingUp:
    case CaptureState::Preparing:
        advanceWarmup(now);
        break;
    case CaptureState::Capturing:
        captureAndSave();
        // ドライバ呼び出し中に押されたものは無視。離して押し直すまで待つ
        shutter.reset(digitalRead(BUTTON_47) == LOW, uint32_t(millis()));
        break;
    default:
        break;
    }
}

CaptureState captureState() { return state; }
CaptureError captureError() { return error; }

const char *captureErrorName(CaptureError reason)
{
    switch (reason)
    {
    case CaptureError::None: return "none";
    case CaptureError::StorageInit: return "SD initialization failed";
    case CaptureError::CameraInit: return "camera initialization failed";
    case CaptureError::CameraReset: return "camera reset failed";
    case CaptureError::CameraWarmup: return "camera warmup failed";
    case CaptureError::Capture: return "image acquisition failed";
    case CaptureError::StorageSave: return "photo save failed";
    }
    return "unknown";
}

void handlePhoto(WebServer &server)
{
    // standaloneではWebのルートなし。撮影バッファは貸さない
    server.send(503, "text/plain", "Photo download is not enabled in standalone mode");
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
