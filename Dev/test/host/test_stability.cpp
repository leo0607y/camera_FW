// 本番の撮影・SD・LED・起動処理を偽ハードで動かす
#include "camera_control.h"
#include "sd_card.h"
#include "led_control.h"
#include "esp_camera.h"
#include <cassert>
#include <iostream>
#include <string>

const char *DEVICE_NAME="test";
const char *DEVICE_VERSION="test";
void setup();
void loop();
// ネットワークは実機では別タスク。撮影のテストはオフライン
void beginConnectivity(bool) {}

static void setButton(bool pressed, uint32_t elapsed=0) {
    mock_time += elapsed;
    mock_pins[BUTTON_47] = pressed ? LOW : HIGH;
    updateCapture();
}
static void press() { setButton(true); setButton(true, BUTTON_DEBOUNCE_MS); }
static void release() { setButton(false); setButton(false, BUTTON_DEBOUNCE_MS); }
static void tick(uint32_t elapsed=5) { mock_time += elapsed; updateCapture(); }
static bool isError() { return captureState() == CaptureState::Error; }
static void settle() {
    for (int i=0; i<1000 && captureState()!=CaptureState::Ready && !isError(); ++i) tick();
    assert(captureState()==CaptureState::Ready || isError());
}
static void ready() { setup(); settle(); assert(captureState()==CaptureState::Ready); }
static void shoot() { press(); settle(); assert(captureState()==CaptureState::Ready); }
static void awaitCapture() {
    for (int i=0; i<100 && captureState()!=CaptureState::Capturing; ++i) tick();
    assert(captureState()==CaptureState::Capturing);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario=argv[1];
    const std::vector<uint8_t> old_photo={1,2,3};
    if (scenario == "existing_photos") {
        mock_files["/1.jpg"]=old_photo;
        mock_files["/3.jpg"]=old_photo;
        assert(initSDCard());
        assert(savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(mock_files.at("/1.jpg") == old_photo);
        assert(mock_files.at("/3.jpg") == old_photo);
        assert(mock_files.at("/2.jpg").size() == sizeof(mock_image));
        assert(mock_files.at("/4.jpg").size() == sizeof(mock_image));
    } else if (scenario == "restart_and_temporary") {
        mock_files["/1.tmp"]=old_photo;
        mock_files["/2.jpg"]=old_photo;
        assert(initSDCard());
        assert(savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(initSDCard());
        assert(savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(mock_files.at("/1.tmp") == old_photo);
        assert(mock_files.at("/2.jpg") == old_photo);
        assert(mock_files.count("/3.jpg") && mock_files.count("/4.jpg"));
    } else if (scenario == "short_write" || scenario == "flush_truncation") {
        assert(initSDCard());
        if (scenario == "short_write") mock_write_limit=2;
        else mock_truncate_on_flush=true;
        assert(!savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(mock_files.empty());
        assert(!isSDCardMounted());
    } else if (scenario == "rename_failure") {
        assert(initSDCard());
        mock_rename_ok=false;
        assert(!savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(!mock_files.count("/1.jpg"));
        assert(mock_files.at("/1.tmp").size() == sizeof(mock_image));
        mock_rename_ok=true;
        assert(initSDCard());
        assert(savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(mock_files.count("/2.jpg"));
    } else if (scenario == "invalid_input") {
        assert(initSDCard());
        assert(!savePhotoToSD(nullptr, 4));
        assert(!savePhotoToSD(mock_image, 0));
        assert(mock_files.empty());
    } else if (scenario == "open_failure") {
        assert(initSDCard());
        mock_open_ok=false;
        assert(!savePhotoToSD(mock_image, sizeof(mock_image)));
        assert(!isSDCardMounted());
    } else if (scenario == "readback_corruption" || scenario == "readback_failure" || scenario == "large_photo") {
        assert(initSDCard());
        std::vector<uint8_t> data(1537);
        for (size_t i=0; i<data.size(); ++i) data[i]=uint8_t(i);
        if (scenario == "readback_corruption") mock_corrupt_at=1024;
        if (scenario == "readback_failure") mock_read_limit=0;
        const bool saved=savePhotoToSD(data.data(), data.size());
        if (scenario == "large_photo") {
            assert(saved && mock_files.at("/1.jpg")==data);
        } else {
            assert(!saved && mock_files.empty() && !isSDCardMounted());
        }
    } else if (scenario == "cooldown_hold" || scenario == "cooldown_boundary") {
        ready(); press(); awaitCapture(); tick();
        assert(captureState()==CaptureState::Cooldown && mock_files.size()==1);
        release();
        if (scenario == "cooldown_boundary") tick(PHOTO_COOLDOWN_MS-BUTTON_DEBOUNCE_MS-10);
        press();
        setButton(true, 1000);
        assert(mock_files.size()==1 && captureState()==CaptureState::Ready);
        release(); shoot();
        assert(mock_files.size()==2);
    } else if (scenario == "bounce") {
        ready();
        const int captures=mock_captures;
        setButton(true); setButton(false, 10); setButton(true, 10);
        setButton(true, 20);
        assert(mock_files.empty() && mock_captures==captures);
        setButton(true, 10);
        assert(captureState()==CaptureState::Preparing);
        setButton(false, 5); setButton(true, 5);
        settle(); tick(1000);
        assert(mock_files.size()==1);
    } else if (scenario == "debounce_clock_wrap") {
        ready();
        mock_time=UINT32_MAX-10;
        setButton(true); setButton(true, 20);
        assert(captureState()==CaptureState::Ready);
        setButton(true, 10); settle();
        assert(mock_files.size()==1);
    } else if (scenario == "cooldown_clock_wrap") {
        ready(); press(); awaitCapture();
        mock_time=UINT32_MAX-200;
        tick(0);
        assert(captureState()==CaptureState::Cooldown);
        tick(499);
        assert(captureState()==CaptureState::Cooldown);
        tick(1);
        assert(captureState()==CaptureState::Ready);
    } else if (scenario == "sd_recovery") {
        mock_mount_ok=false;
        setup();
        assert(isError() && captureError()==CaptureError::StorageInit);
        assert(mock_pins[LED_PINS[LED_READY_INDEX]]==HIGH);
        press();
        assert(isError() && mock_files.empty());
        release(); mock_mount_ok=true; press(); settle();
        assert(!isError() && mock_files.empty());
        release(); shoot();
        assert(mock_files.size()==1 && mock_mounts>=3);
    } else if (scenario == "camera_startup_failure") {
        mock_camera_ok=false;
        setup(); loop();
        assert(isError() && captureError()==CaptureError::CameraInit);
        assert(mock_pins[LED_PINS[LED_READY_INDEX]]==HIGH);
        assert(mock_pins[LED_PINS[LED_ERROR_INDEX]]==LOW);
        mock_camera_ok=true; press(); settle();
        assert(!isError() && mock_files.empty());
        release(); shoot();
        assert(mock_files.size()==1);
    } else if (scenario == "warmup_failure") {
        mock_capture_ok=false;
        setup(); loop();
        assert(isError() && captureError()==CaptureError::CameraWarmup);
        assert(mock_pins[LED_PINS[LED_READY_INDEX]]==HIGH);
        press(); settle();
        assert(isError());
        release(); mock_capture_ok=true; press(); settle();
        assert(!isError() && mock_files.empty());
        release(); shoot();
        assert(mock_files.size()==1);
    } else if (scenario == "capture_failure" || scenario == "empty_frame" || scenario == "final_capture_failure") {
        ready();
        if (scenario=="final_capture_failure") { press(); awaitCapture(); mock_capture_ok=false; tick(); }
        else {
            if (scenario=="capture_failure") mock_capture_ok=false;
            else mock_frame.len=0;
            press();
        }
        assert(isError() && captureError()==CaptureError::Capture && mock_files.empty());
        assert(mock_frames_out==0);
    } else if (scenario == "startup_held") {
        initLED(); mock_pins[BUTTON_47]=LOW;
        beginCapture(); settle(); tick(1000);
        assert(mock_files.empty());
        release(); shoot();
        assert(mock_files.size()==1);
    } else if (scenario == "busy_press") {
        ready(); press(); release(); press();
        settle(); tick(1000);
        assert(mock_files.size()==1);
        release(); shoot();
        assert(mock_files.size()==2);
    } else if (scenario == "press_during_write") {
        ready(); press(); release(); awaitCapture();
        mock_on_write=[] {
            assert(captureState()==CaptureState::Saving);
            assert(mock_pins[LED_PINS[LED_READY_INDEX]]==HIGH);
            mock_pins[BUTTON_47]=LOW;
            mock_time+=2000;
        };
        tick(); settle(); tick(1000);
        assert(mock_files.size()==1);
        mock_on_write=nullptr;
        release(); shoot();
        assert(mock_files.size()==2);
    } else if (scenario == "save_failure_recovery") {
        ready(); mock_write_limit=2; press(); settle();
        assert(isError() && captureError()==CaptureError::StorageSave && mock_files.empty());
        assert(mock_frames_out==0);
        mock_write_limit=100;
        release(); press(); settle();
        assert(captureError()==CaptureError::None && mock_files.empty());
        release(); shoot();
        assert(mock_files.size()==1);
    } else if (scenario == "reset_failure") {
        ready(); mock_capture_ok=false; press();
        assert(isError());
        mock_reset_ok=false; release(); press();
        assert(isError() && captureError()==CaptureError::CameraReset);
        mock_reset_ok=true; mock_capture_ok=true;
        release(); press(); settle();
        assert(captureState()==CaptureState::Ready);
    } else if (scenario == "cooperative_warmup") {
        setup();
        assert(captureState()==CaptureState::WarmingUp);
        const uint32_t start=mock_time;
        for (int i=0; i<100; ++i) updateCapture();
        assert(mock_time==start && mock_captures==1);
        settle();
        assert(mock_captures==5);
        press();
        const uint32_t preparation=mock_time;
        const int captures=mock_captures;
        for (int i=0; i<100; ++i) updateCapture();
        assert(mock_time==preparation && mock_captures==captures);
        settle();
        assert(mock_captures==11 && mock_files.size()==1);
    } else if (scenario == "led_state_and_logging") {
        initLED();
        updateCameraStatusLED(true, false);
        assert(led_states[LED_READY_INDEX] == 1);
        const int messages=Serial.messages;
        for (int i=0; i<100; ++i) updateCameraStatusLED(true, false);
        assert(Serial.messages == messages);
        updateCameraStatusLED(true, true);
        assert(led_states[LED_READY_INDEX] == 0 && led_states[LED_ERROR_INDEX] == 1);
    } else { return 2; }
    assert(mock_frames_out == 0);
    std::cout << "PASS " << scenario << '\n';
}
