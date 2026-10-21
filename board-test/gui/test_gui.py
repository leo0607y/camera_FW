"""実機なしで通信待ち、画像受信、切断後の応答を検証する。"""
import time
import tkinter as tk
import unittest
import json
import tempfile
from pathlib import Path
from unittest.mock import patch
from PIL import Image
import io
import base64
import zlib
from board_test import App, valid_response


class GuiTest(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        # 自動ポーリングだと忙しいPCで間隔を見る前に次の要求が出るので、明示的に要求する
        with patch.object(App, "poll", lambda self: None):
            self.app = App(self.root, demo=True)
        self.addCleanup(self.app.close)
        self.spin_until(lambda: self.app.verified and self.app.pending is None)

    def spin_until(self, predicate, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.root.update()
            if predicate():
                return
            time.sleep(0.01)
        self.fail("GUI応答待ちがタイムアウトしました")

    def test_action_during_status_and_capture(self):
        self.assertTrue(self.app.log.winfo_ismapped())
        self.app.send("status")
        self.assertEqual(self.app.pending["cmd"], "status")
        self.app.toggle_led(2)
        self.spin_until(lambda: self.app.demo_leds[2] and self.app.pending is None)
        self.assertEqual(self.app.led_buttons[2]["text"], "消灯")
        self.app.send("camera")
        self.spin_until(lambda: self.app.jpeg is not None)
        self.assertTrue(self.app.jpeg.startswith(b"\xff\xd8"))
        self.assertIsNotNone(self.app.photo)
        self.app.send("off")
        self.spin_until(lambda: not any(self.app.demo_leds[2:]) and self.app.pending is None)

    def test_stale_response_after_disconnect(self):
        self.app.send("camera")
        self.app.disconnect()
        deadline = time.monotonic() + 0.4
        while time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertIsNone(self.app.jpeg)
        self.assertFalse(self.app.verified)

    def test_fragmented_serial_jpeg(self):
        # 起動時のゴミと、USBバッファより大きいJPEGを適当な位置で分割して読ませる
        packet = {"id": 123, "jpeg": "A" * 180000}
        raw = b'ESP-ROM boot log\r\n{"id":2,"jpeg":"interrupted\n\n' + json.dumps(packet).encode() + b"\n"

        class Port:
            is_open = True
            in_waiting = 113

            def read(self, size):
                nonlocal raw
                result, raw = raw[:size], raw[size:]
                if not raw:
                    self.is_open = False
                return result

        self.app.reader(Port(), 54321)
        session, received = self.app.events.get_nowait()
        self.assertEqual(session, 54321)
        self.assertEqual(received, packet)

    def test_malformed_response_does_not_stop_updates(self):
        self.app.send("status")
        self.app.events.put((self.app.session, dict(id=self.app.pending["id"],
                            device="camera-board-test-v1", leds=[True] * 9)))
        self.root.update()
        self.spin_until(lambda: self.app.pending is None)
        self.app.send("camera")
        self.spin_until(lambda: self.app.jpeg is not None)
        self.assertFalse(valid_response({}))

    def test_timeout_disconnects_and_disables_controls(self):
        self.app.send("status")
        self.app.deadline = time.monotonic() - 1
        self.app.pump()
        self.assertIsNone(self.app.port)
        self.assertFalse(self.app.verified)
        self.assertTrue(all(str(b["state"]) == "disabled" for b in self.app.controls))

    def test_save_preserves_received_jpeg(self):
        self.app.orientation.set("そのまま")
        self.app.send("camera")
        self.spin_until(lambda: self.app.jpeg is not None)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "photo.jpg"
            with patch("board_test.filedialog.asksaveasfilename", return_value=str(path)):
                self.app.save_image()
            self.assertEqual(path.read_bytes(), self.app.jpeg)

    def test_image_orientation_and_saved_direction(self):
        image = Image.new("RGB", (40, 40), "red")
        image.paste(Image.new("RGB", (40, 20), "blue"), (0, 20))
        data = io.BytesIO()
        image.save(data, format="JPEG", quality=100)
        self.app.jpeg = data.getvalue()
        self.app.orientation.set("上下反転")
        oriented = self.app.oriented_image()
        self.assertGreater(oriented.getpixel((10, 5))[2], 200)
        self.assertGreater(oriented.getpixel((10, 35))[0], 200)
        self.app.draw_preview()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "corrected.jpg"
            with patch("board_test.filedialog.asksaveasfilename", return_value=str(path)):
                self.app.save_image()
            with Image.open(path) as saved:
                self.assertGreater(saved.getpixel((10, 5))[2], 200)
                self.assertGreater(saved.getpixel((10, 35))[0], 200)

    def test_adc_max_calibration(self):
        self.app.calibrate_adc_max()
        self.assertEqual(self.app.adc_raw, 1830)
        self.assertEqual(self.app.adc_max, 1830)
        self.assertEqual(float(self.app.meter["maximum"]), 1830)
        self.assertEqual(float(self.app.meter["value"]), 1830)

    def test_ready_controls_and_physical_photo_event(self):
        self.app.send("status")
        response=dict(id=self.app.pending["id"],device="camera-board-test-v1",ok=True,error="ESP_OK",
                      adc=123,psram=8388608,leds=[False]*5,buttons=[False]*2,counts=[0]*2,
                      ready=False,busy=True,phase="warming",warmup_frames=10,warmup_ms=3500)
        self.app.events.put((self.app.session,response))
        self.app.pump()
        self.assertEqual(str(self.app.capture_button["state"]),"disabled")
        self.assertEqual(str(self.app.led_buttons[0]["state"]),"disabled")
        self.app.fake_response(dict(id=0,cmd="camera"),self.app.session)
        session,photo=self.app.events.get_nowait()
        photo["event"]="photo"
        self.app.events.put((session,photo))
        self.app.pump()
        self.assertIsNotNone(self.app.jpeg)
        self.assertEqual(str(self.app.capture_button["state"]),"normal")

    def test_bad_received_crc_rejects_image(self):
        self.app.fake_response(dict(id=0,cmd="camera"),self.app.session)
        session,photo=self.app.events.get_nowait()
        photo.update(event="photo",crc32=-1)
        self.app.events.put((session,photo))
        self.app.pump()
        self.assertIsNone(self.app.jpeg)
        self.assertIn("CRC",self.app.status.get())

    def test_recording_stop_control_and_mode_event(self):
        packet=dict(id=0,event="mode",device="camera-board-test-v1",ok=True,error="ESP_OK",
                    adc=2048,psram=8388608,leds=[False,False,True,True,False],buttons=[False]*2,counts=[0]*2,
                    ready=False,busy=True,phase="recording",mode=6,selected_mode=6,recording=True)
        self.app.events.put((self.app.session,packet));self.app.pump()
        self.assertEqual(self.app.mode_choice.get(),"6 動画")
        self.assertEqual(str(self.app.capture_button["state"]),"normal")
        self.assertEqual(self.app.capture_button["text"],"録画停止")
        self.assertEqual(str(self.app.apply_mode_button["state"]),"disabled")
        self.assertFalse(valid_response(dict(packet,mode=8)))

    def test_phone_transfer_disables_shutter_and_shows_connection(self):
        packet=dict(id=0,event="mode",device="camera-board-test-v1",ok=True,error="ESP_OK",
                    adc=2048,psram=8388608,leds=[True,False,True,True,True],buttons=[False]*2,counts=[0]*2,
                    ready=True,busy=False,phase="ap_ready",mode=7,selected_mode=7,recording=False,
                    ap_active=True,ap_ssid="Camera-Test",ap_password="test-password",ap_url="http://192.168.4.1",ap_clients=1)
        self.app.events.put((self.app.session,packet));self.app.pump()
        self.assertEqual(self.app.mode_choice.get(),"7 スマホ転送")
        self.assertEqual(str(self.app.capture_button["state"]),"disabled")
        self.assertIn("Camera-Test",self.app.ap_info.get())
        self.assertIn("192.168.4.1",self.app.ap_info.get())
        self.assertIn("スマホ転送できます",self.app.camera_state.get())

    def test_video_download_crc_and_cleanup(self):
        raw=b"RIFF"+bytes(range(256))*3
        with tempfile.TemporaryDirectory() as directory:
            final=Path(directory)/"video.avi"
            self.app.last_video="/DCIM/CAMERA/VID00001.AVI"
            self.app.board_busy=False
            with patch("board_test.filedialog.asksaveasfilename",return_value=str(final)), patch.object(self.app,"send"):
                self.app.save_video()
                packet=dict(file_data=base64.b64encode(raw).decode(),offset=0,total=len(raw),file_crc32=zlib.crc32(raw))
                self.app.receive_video_chunk(packet)
                self.assertEqual(final.read_bytes(),raw)
                self.assertIsNone(self.app.download)
                final.write_bytes(b"existing")
                self.app.save_video()
                self.app.receive_video_chunk(dict(packet,file_crc32=0))
                self.assertIsNone(self.app.download)
                self.assertEqual(final.read_bytes(),b"existing")
                self.assertEqual(list(Path(directory).glob("*.part")),[])

    def test_responsive_sections_and_scroll_access(self):
        for width, height, expected in ((1280, 760, 3), (1000, 700, 2),
                                        (800, 600, 2), (640, 480, 1),
                                        (360, 300, 1), (280, 240, 1)):
            with self.subTest(size=(width, height)):
                self.root.geometry(f"{width}x{height}")
                self.spin_until(lambda: self.app.layout_columns == expected)
                self.root.update()
                for panel in self.app.sections:
                    self.assertTrue(panel.winfo_ismapped())
                    self.assertGreater(panel.winfo_width(), 250)
                    self.assertGreater(panel.winfo_height(), 100)
                    self.assertLessEqual(panel.winfo_rootx() + panel.winfo_width(),
                                         self.app.page.winfo_rootx() + self.app.page.winfo_width())
                    self.assertLessEqual(panel.winfo_rooty() + panel.winfo_height(),
                                         self.app.page.winfo_rooty() + self.app.page.winfo_height())
                self.app.viewport.yview_moveto(1)
                self.app.viewport.xview_moveto(1)
                self.root.update()
                self.assertLessEqual(self.app.log.winfo_rooty() + self.app.log.winfo_height(),
                                     self.app.viewport.winfo_rooty() + self.app.viewport.winfo_height() + 2)
                self.assertAlmostEqual(self.app.viewport.yview()[1], 1, places=2)
        self.root.geometry("1280x760")
        self.spin_until(lambda: self.app.layout_columns == 3)
        self.assertTrue(self.app.connection.get())


if __name__ == "__main__":
    unittest.main()
