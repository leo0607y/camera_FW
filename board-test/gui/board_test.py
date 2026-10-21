"""USB接続の撮影コントローラー。--demo は基板なしで画面を確認するモード。"""
import argparse
import base64
import io
import json
import queue
import threading
import time
import zlib
import os
import tempfile
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from pathlib import Path

import serial
from serial.tools import list_ports
from PIL import Image, ImageDraw, ImageEnhance, ImageTk

LEDS = [("Ready 緑", 46), ("Error 赤", 38), ("Mode 橙", 45), ("Mode 橙", 21), ("Mode 橙", 1)]
MODE_NAMES = ("標準", "高精細", "連写", "セルフタイマー", "タイムラプス", "暗所", "動画", "スマホ転送")
CAPTURE_LABELS = ("撮影・SD保存", "撮影・SD保存", "連写（5枚）・SD保存", "セルフタイマー（10秒）", "タイムラプス開始", "撮影・SD保存", "録画開始", "スマホ転送中")
# 色加工はPC保存時にかける。SDは元のJPEGのまま
LOOKS = ("元のまま", "鮮やか", "白黒", "セピア")
SEPIA = (0.393, 0.769, 0.189, 0, 0.349, 0.686, 0.168, 0, 0.272, 0.534, 0.131, 0)


def apply_look(image, look):
    if look == "鮮やか":
        return ImageEnhance.Contrast(ImageEnhance.Color(image.convert("RGB")).enhance(1.35)).enhance(1.08)
    if look == "白黒":
        return ImageEnhance.Contrast(image.convert("L")).enhance(1.05).convert("RGB")
    if look == "セピア":
        return image.convert("RGB").convert("RGB", SEPIA)
    return image

LED_COLORS = {46: "#239b56", 38: "#e34949", 45: "#e28a1c", 21: "#e28a1c", 1: "#e28a1c"}


def valid_response(data):
    """壊れたパケットや合わないパケットはTkに触る前に弾く。"""
    return (
        data.get("device") == "camera-board-test-v1"
        and type(data.get("id")) is int
        and type(data.get("ok")) is bool
        and isinstance(data.get("error"), str)
        and isinstance(data.get("leds"), list) and len(data["leds"]) == 5
        and all(type(v) is bool for v in data["leds"])
        and isinstance(data.get("buttons"), list) and len(data["buttons"]) == 2
        and all(type(v) is bool for v in data["buttons"])
        and isinstance(data.get("counts"), list) and len(data["counts"]) == 2
        and all(type(v) is int and v >= 0 for v in data["counts"])
        and type(data.get("adc")) is int and -1 <= data["adc"] <= 4095
        and type(data.get("psram")) is int and data["psram"] >= 0
        and ("jpeg" not in data or isinstance(data["jpeg"], str))
        and all(key not in data or type(data[key]) is bool for key in ("ready", "busy", "recording", "selecting"))
        and all(key not in data or type(data[key]) is int and 0 <= data[key] < 8 for key in ("mode", "selected_mode"))
    )


class App:
    def __init__(self, root, demo=False):
        self.root, self.demo = root, demo
        self.port = None
        self.events = queue.Queue()
        self.pending = None
        self.queued = None
        self.sequence = 0
        self.deadline = 0
        self.jpeg = None
        self.verified = False
        self.session = 0
        self.timers = set()
        self.demo_leds = [False] * 5
        self.adc_raw = None
        self.adc_max = 2048
        self.hardware_ready = False
        self.board_busy = True
        self.recording = False
        self.active_mode = None
        self.download = None
        self.last_video = ""
        root.title("Camera 撮影コントローラー" + (" [デモ]" if demo else ""))
        root.geometry(f"{min(1280, root.winfo_screenwidth()-80)}x{min(760, root.winfo_screenheight()-130)}")
        root.minsize(280, 240)
        root.configure(bg="#eff3f8")
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TFrame", background="#eff3f8")
        style.configure("TLabelframe", background="#eff3f8", padding=12)
        style.configure("TLabelframe.Label", background="#eff3f8", font=("Yu Gothic UI", 11, "bold"))
        style.configure("TLabel", background="#eff3f8", font=("Yu Gothic UI", 10))
        style.configure("TButton", font=("Yu Gothic UI", 10), padding=(12, 7))
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        self.viewport = tk.Canvas(root, bg="#eff3f8", highlightthickness=0, width=1, height=1)
        self.viewport.grid(row=0, column=0, sticky="nsew")
        self.vertical_scroll = ttk.Scrollbar(root, orient="vertical", command=self.viewport.yview)
        self.vertical_scroll.grid(row=0, column=1, sticky="ns")
        self.horizontal_scroll = ttk.Scrollbar(root, orient="horizontal", command=self.viewport.xview)
        self.horizontal_scroll.grid(row=1, column=0, sticky="ew")
        self.viewport.configure(yscrollcommand=self.vertical_scroll.set, xscrollcommand=self.horizontal_scroll.set)
        wrap = self.page = ttk.Frame(self.viewport, padding=12)
        self.page_window = self.viewport.create_window((0, 0), window=wrap, anchor="nw")
        self.layout_columns = None
        self.layout_pending = False
        wrap.columnconfigure(0, weight=1)
        wrap.rowconfigure(3, weight=1)
        ttk.Label(wrap, text="Camera 撮影コントローラー", font=("Yu Gothic UI", 21, "bold")).grid(row=0, column=0, sticky="w")
        self.subtitle = ttk.Label(wrap, text="SW6で撮影 / SW7でモード選択・確定 / RV1で8モード切替")
        self.subtitle.grid(row=1, column=0, sticky="w", pady=(0, 12))
        row = ttk.Frame(wrap)
        row.grid(row=2, column=0, sticky="ew")
        self.port_name = tk.StringVar()
        self.ports = ttk.Combobox(row, textvariable=self.port_name, width=12, state="readonly")
        self.ports.grid(row=0, column=0, sticky="w")
        ttk.Button(row, text="ポート更新", command=self.refresh_ports).grid(row=0, column=1, padx=6)
        self.connect_button = ttk.Button(row, text="接続", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=2, sticky="w")
        self.connection = tk.StringVar(value="未接続" if not demo else "デモ表示 / 実機には接続しません")
        self.connection_label = ttk.Label(row, textvariable=self.connection)
        self.connection_label.grid(row=1, column=0, columnspan=3, sticky="w", pady=(6, 0))
        self.camera_state = tk.StringVar(value="基板に接続すると撮影準備を確認します")
        self.camera_state_label = ttk.Label(row, textvariable=self.camera_state)
        self.camera_state_label.grid(row=2, column=0, columnspan=3, sticky="w", pady=(4, 0))
        self.ap_info = tk.StringVar()
        self.ap_info_label = ttk.Label(row, textvariable=self.ap_info, wraplength=650)
        self.ap_info_label.grid(row=3, column=0, columnspan=3, sticky="w", pady=(4,0))
        body = self.body = ttk.Frame(wrap)
        body.grid(row=3, column=0, sticky="nsew", pady=8)
        left = right = body
        leds = ttk.LabelFrame(left, text="撮影モード / 状態ランプ")
        self.lamps, self.led_buttons = [], []
        self.controls = []
        mode_group = None
        self.mode_display = tk.StringVar(value="モード: 未取得")
        for i, (name, pin) in enumerate(LEDS):
            if i < 2:
                r = ttk.Frame(leds)
                r.pack(fill="x", pady=4)
            else:
                if mode_group is None:
                    mode_group = ttk.LabelFrame(leds, text="モード / オレンジ3灯")
                    mode_group.pack(fill="x", pady=(8, 0))
                r = ttk.Frame(mode_group)
                r.grid(row=0, column=i-2, padx=4, sticky="n")
            canvas = tk.Canvas(r, width=28, height=28, bg="#eff3f8", highlightthickness=0)
            canvas.create_oval(6, 6, 22, 22, fill="#a6afba", outline="", tags="lamp")
            canvas.pack(side="left" if i < 2 else "top")
            ttk.Label(r, text=f"{name}  GPIO{pin}" if i < 2 else f"GPIO{pin} / {4 >> (i-2)}", width=16 if i < 2 else 10, anchor="center" if i >= 2 else "w").pack(side="left" if i < 2 else "top")
            button = ttk.Button(r, text="点灯", width=7, command=lambda idx=i: self.toggle_led(idx))
            button.pack(side="right" if i < 2 else "top", pady=2)
            self.lamps.append(canvas)
            self.led_buttons.append(button)
            if i < 2:
                button.configure(text="自動", state="disabled")
            else:
                self.controls.append(button)
        ttk.Label(mode_group, textvariable=self.mode_display, font=("Consolas", 12, "bold")).grid(row=1, column=0, columnspan=3, pady=(8, 0))
        ttk.Label(mode_group, text="左から 4 / 2 / 1、点灯 = 1").grid(row=2, column=0, columnspan=3)
        ttk.Label(leds, text="緑 点灯: 準備完了 / ゆっくり点滅: 準備中・処理中 / 速い点滅: 録画中・セルフタイマー（SW6で停止） / 消灯: 選択中・停止中\n赤 点灯: 直前の撮影・録画が失敗 / 点滅: 準備失敗で停止中（SW6で再準備）", justify="left").pack(anchor="w", pady=(8, 0))
        off = ttk.Button(leds, text="モードLEDを消灯", command=lambda: self.send("off"))
        off.pack(fill="x", pady=(8, 0))
        self.controls.append(off)
        self.mode_off_button = off
        self.mode_choice = tk.StringVar(value="0 標準")
        self.mode_select = ttk.Combobox(leds, textvariable=self.mode_choice, state="readonly", values=tuple(f"{i} {name}" for i,name in enumerate(MODE_NAMES)))
        self.mode_select.pack(fill="x", pady=(8, 3))
        self.apply_mode_button = ttk.Button(leds, text="モードを適用", command=lambda: self.send("mode", value=int(self.mode_choice.get().split()[0])))
        self.apply_mode_button.pack(fill="x")
        self.controls.append(self.apply_mode_button)
        ttk.Label(leds, text="SW7: 選択・確定 / RV1: モード変更\n選択中のSW6: キャンセル").pack(anchor="w", pady=6)
        inputs = ttk.LabelFrame(left, text="入力 / 自動更新")
        self.buttons = [tk.StringVar(value=f"SW{i+6} GPIO{i+47}: 未取得") for i in range(2)]
        for v in self.buttons:
            ttk.Label(inputs, textvariable=v).pack(anchor="w", pady=4)
        self.adc = tk.StringVar(value="RV1 GPIO2: 未取得")
        ttk.Label(inputs, textvariable=self.adc).pack(anchor="w", pady=(8, 3))
        self.meter = ttk.Progressbar(inputs, maximum=self.adc_max, length=260)
        self.meter.pack(fill="x")
        self.adc_range = tk.StringVar(value="表示上限 2048 / 回路からの目安")
        ttk.Label(inputs, textvariable=self.adc_range).pack(anchor="w", pady=4)
        calibrate = ttk.Button(inputs, text="取得値を最大に設定", command=self.calibrate_adc_max)
        calibrate.pack(fill="x")
        self.controls.append(calibrate)
        ttk.Label(inputs, text="R16 + RV1: 最大約1.65V\nADC生値は4095まで届きません").pack(anchor="w", pady=4)
        sd = ttk.LabelFrame(left, text="microSD / メモリー")
        test = ttk.Button(sd, text="SD 読み書きテスト", command=lambda: self.send("sd"))
        self.sd_test_button = test
        test.pack(fill="x")
        self.controls.append(test)
        ttk.Label(sd, text="テストファイルを作成・照合・削除\n既存の写真は変更しません").pack(anchor="w", pady=6)
        self.memory = tk.StringVar(value="PSRAM: 未取得")
        ttk.Label(sd, textvariable=self.memory).pack(anchor="w")
        cam = ttk.LabelFrame(right, text="OV2640 / 撮影画像")
        self.sections = (leds, inputs, sd, cam)
        self.preview = tk.Canvas(cam, bg="#162334", highlightthickness=0, width=1, height=240)
        self.preview.pack(fill="both", expand=True)
        self.preview.bind("<Configure>", lambda _: self.draw_preview())
        bar = ttk.Frame(cam)
        bar.pack(fill="x", pady=(10, 0))
        capture = self.capture_button = ttk.Button(bar, text="撮影・SD保存", command=lambda: self.send("stop" if self.recording else "camera"))
        capture.pack(side="left")
        self.controls.append(capture)
        self.save_button = ttk.Button(bar, text="画像をPCへ保存", command=self.save_image, state="disabled")
        self.save_button.pack(side="left", padx=8)
        direction = ttk.Frame(cam)
        direction.pack(fill="x", pady=(6, 0))
        ttk.Label(direction, text="画像の向き").pack(side="left")
        self.orientation = tk.StringVar(value="そのまま")
        direction_select = ttk.Combobox(direction, textvariable=self.orientation, state="readonly",
                                       values=("そのまま", "上下反転", "180度回転"), width=12)
        direction_select.pack(side="left", padx=8)
        direction_select.bind("<<ComboboxSelected>>", lambda _: self.draw_preview())
        ttk.Label(direction, text="加工").pack(side="left", padx=(8, 0))
        self.look = tk.StringVar(value=LOOKS[0])
        look_select = ttk.Combobox(direction, textvariable=self.look, state="readonly", values=LOOKS, width=9)
        look_select.pack(side="left", padx=8)
        look_select.bind("<<ComboboxSelected>>", lambda _: self.draw_preview())
        self.prepare_button = ttk.Button(cam, text="再準備 / エラーから復帰", command=lambda: self.send("prepare"))
        self.prepare_button.pack(fill="x", pady=(6, 0))
        self.controls.append(self.prepare_button)
        self.video_save_button = ttk.Button(cam, text="動画をPCへ保存", command=self.save_video, state="disabled")
        self.video_save_button.pack(fill="x", pady=(6, 0))
        self.image_info = tk.StringVar(value="撮影ボタンで取得 / JPEG・1600 x 1200")
        ttk.Label(cam, textvariable=self.image_info).pack(anchor="w", pady=(6, 0))
        self.status = tk.StringVar(value="基板に専用ファームを書き込み、USBで接続してください。")
        self.status_label = ttk.Label(wrap, textvariable=self.status, wraplength=960)
        self.status_label.grid(row=4, column=0, sticky="w")
        self.log = tk.Text(wrap, width=1, height=4, wrap="word", font=("Consolas", 9), relief="flat", bg="#fff", state="disabled")
        self.log.grid(row=5, column=0, sticky="ew", pady=(8, 0))
        def bind_wheel(widget):
            if widget is not self.log:
                widget.bind("<MouseWheel>", self.scroll_page)
                widget.bind("<Shift-MouseWheel>", self.scroll_page_horizontal)
                widget.bind("<FocusIn>", self.reveal_focus, add="+")
            for child in widget.winfo_children():
                bind_wheel(child)
        bind_wheel(wrap)
        self.viewport.bind("<MouseWheel>", self.scroll_page)
        self.viewport.bind("<Shift-MouseWheel>", self.scroll_page_horizontal)
        self.viewport.bind("<Configure>", self.request_layout)
        wrap.bind("<Configure>", self.request_layout)
        self.request_layout()
        self.refresh_ports()
        self.set_controls(False)
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.schedule(60, self.pump)
        self.schedule(250, self.poll)
        if demo:
            self.toggle_connection()

    def request_layout(self, event=None):
        if not self.layout_pending:
            self.layout_pending = True
            self.schedule(1, self.update_layout)

    def update_layout(self):
        self.layout_pending = False
        width = max(360, self.viewport.winfo_width())
        columns = 3 if width >= 1100 else 2 if width >= 740 else 1
        if columns != self.layout_columns:
            self.layout_columns = columns
            for panel in self.sections:
                panel.grid_forget()
            for i in range(3):
                self.body.columnconfigure(i, weight=1 if i < columns else 0, uniform="panels" if i < columns else "")
            for i in range(4):
                self.body.rowconfigure(i, weight=0)
            positions = (
                ((0, 0, 2), (0, 1, 1), (1, 1, 1), (0, 2, 2)) if columns == 3 else
                ((0, 0, 1), (0, 1, 1), (1, 0, 1), (1, 1, 1)) if columns == 2 else
                ((0, 0, 1), (1, 0, 1), (2, 0, 1), (3, 0, 1))
            )
            for panel, (row, col, rowspan) in zip(self.sections, positions):
                panel.grid(row=row, column=col, rowspan=rowspan, sticky="nsew", padx=4, pady=4)
        self.viewport.itemconfigure(self.page_window, width=width)
        for label in (self.subtitle, self.status_label, self.connection_label, self.camera_state_label, self.ap_info_label):
            label.configure(wraplength=max(280, width-40))
        self.page.update_idletasks()
        height = max(self.page.winfo_reqheight(), self.viewport.winfo_height())
        self.viewport.itemconfigure(self.page_window, height=height)
        self.viewport.configure(scrollregion=(0, 0, width, height))

    def scroll_page(self, event):
        self.viewport.yview_scroll(-int(event.delta / 120), "units")
        return "break"

    def scroll_page_horizontal(self, event):
        self.viewport.xview_scroll(-int(event.delta / 120), "units")
        return "break"

    def reveal_focus(self, event):
        widget = event.widget
        top = widget.winfo_rooty() - self.page.winfo_rooty()
        left = widget.winfo_rootx() - self.page.winfo_rootx()
        height = max(1, self.page.winfo_height())
        width = max(1, self.page.winfo_width())
        view_top = self.viewport.canvasy(0)
        view_left = self.viewport.canvasx(0)
        if top < view_top or top + widget.winfo_height() > view_top + self.viewport.winfo_height():
            self.viewport.yview_moveto(max(0, top-12)/height)
        if left < view_left or left + widget.winfo_width() > view_left + self.viewport.winfo_width():
            self.viewport.xview_moveto(max(0, left-12)/width)

    def refresh_ports(self):
        ports = list(list_ports.comports())
        names = [p.device for p in ports]
        self.ports["values"] = names
        if names and self.port_name.get() not in names:
            preferred = next((p.device for p in ports if p.vid == 0x303A), names[0])
            self.port_name.set(preferred)

    def schedule(self, delay, callback):
        def run():
            self.timers.discard(timer)
            callback()
        timer = self.root.after(delay, run)
        self.timers.add(timer)

    def record(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", time.strftime("%H:%M:%S ") + text + "\n")
        if int(self.log.index("end-1c").split(".")[0]) > 150:
            self.log.delete("1.0", "30.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def set_controls(self, enabled):
        for b in self.controls:
            b.configure(state="normal" if enabled else "disabled")
        self.capture_button.configure(state="normal" if enabled and (self.hardware_ready or self.recording) and self.active_mode != 7 and self.download is None else "disabled")
        if self.active_mode is not None:
            self.mode_select.configure(state="readonly" if enabled and not self.board_busy and not self.download else "disabled")
            self.apply_mode_button.configure(state="normal" if enabled and not self.board_busy and not self.download else "disabled")
        self.video_save_button.configure(state="normal" if enabled and self.last_video and not self.board_busy and not self.download else "disabled")
        for b in (self.prepare_button, self.sd_test_button):
            b.configure(state="normal" if enabled and not self.board_busy and self.download is None else "disabled")

    def save_video(self):
        if not self.last_video or self.download or self.board_busy:
            return
        path = filedialog.asksaveasfilename(defaultextension=".avi", filetypes=[("AVI動画", "*.avi")], initialfile=Path(self.last_video).name)
        if not path:
            return
        temp = tempfile.NamedTemporaryFile(mode="wb", prefix=".camera-", suffix=".part", dir=Path(path).parent, delete=False)
        self.download = dict(file=temp, temporary=temp.name, path=path, offset=0, crc=0, total=None, expected_crc=None)
        self.send("file_chunk", offset=0)

    def abort_download(self):
        if self.download:
            d, self.download = self.download, None
            d["file"].close()
            Path(d["temporary"]).unlink(missing_ok=True)

    def receive_video_chunk(self, data):
        d = self.download
        if not d:
            return
        try:
            chunk = base64.b64decode(data["file_data"], validate=True)
            if data["offset"] != d["offset"] or not chunk or not 0 < data["total"] <= 256*1024*1024:
                raise ValueError("動画の転送位置またはサイズが不正です")
            if d["total"] is not None and (d["total"] != data["total"] or d["expected_crc"] != data["file_crc32"]):
                raise ValueError("転送中に動画が変更されました")
            d["total"], d["expected_crc"] = data["total"], data["file_crc32"]
            d["file"].write(chunk)
            d["offset"] += len(chunk)
            d["crc"] = zlib.crc32(chunk, d["crc"])
            if d["offset"] > d["total"]:
                raise ValueError("動画サイズが一致しません")
            self.status.set(f"動画をPCへ転送中 {d['offset']:,} / {d['total']:,} bytes")
            if d["offset"] == d["total"]:
                if d["crc"] != d["expected_crc"]:
                    raise ValueError("動画のCRCが一致しません")
                d["file"].flush()
                os.fsync(d["file"].fileno())
                d["file"].close()
                os.replace(d["temporary"], d["path"])
                self.download = None
                self.status.set(f"動画を保存しました。{d['path']}")
            else:
                self.send("file_chunk", offset=d["offset"])
        except (KeyError, ValueError, OSError) as exc:
            self.abort_download()
            self.status.set(f"動画の保存に失敗: {exc}")

    def toggle_connection(self):
        if self.port:
            self.disconnect()
            return
        if self.demo:
            self.port = True
            self.verified = True
            self.connection.set("デモ接続 / 表示値・画像はサンプルです")
            self.connect_button.configure(text="切断")
            self.set_controls(True)
            self.send("status")
            return
        try:
            p = serial.Serial()
            p.port = self.port_name.get()
            p.baudrate = 115200
            p.timeout = 0.25
            p.write_timeout = 1
            p.dtr = False
            p.rts = False
            p.open()
            self.port = p
            self.session += 1
            threading.Thread(target=self.reader, args=(p, self.session), daemon=True).start()
            self.connection.set("基板を確認中")
            self.connect_button.configure(text="切断")
            self.send("status")
        except (serial.SerialException, OSError) as exc:
            self.disconnect()
            messagebox.showerror("接続できません", str(exc))

    def disconnect(self):
        self.abort_download()
        self.recording = False
        p, self.port = self.port, None
        self.session += 1
        if p and p is not True:
            p.close()
        self.pending = None
        self.queued = None
        self.verified = False
        self.hardware_ready = False
        self.board_busy = True
        self.set_controls(False)
        self.connection.set("未接続")
        self.connect_button.configure(text="接続")
        self.status.set("切断しました。LED状態表示は最終取得値です。")

    def reader(self, p, session):
        buffer = bytearray()
        try:
            while p.is_open:
                data = p.read(max(1, min(p.in_waiting, 65536)))
                buffer.extend(data)
                if len(buffer) > 2_000_000:
                    raise ValueError("受信データが上限を超えました")
                while b"\n" in buffer:
                    line, _, rest = buffer.partition(b"\n")
                    buffer = bytearray(rest)
                    try:
                        obj = json.loads(line)
                        if isinstance(obj, dict):
                            self.events.put((session, obj))
                    except (ValueError, UnicodeDecodeError):
                        pass  # 起動時のROMログなどはプロトコル対象外
        except (serial.SerialException, OSError, ValueError) as exc:
            self.events.put((session, {"transport_error": str(exc)}))

    def send(self, cmd, **fields):
        if not self.port:
            return
        if self.pending:
            if cmd != "status" and self.pending["cmd"] == "status" and self.verified:
                self.queued = (cmd, fields)
                self.set_controls(False)
            return
        if cmd != "status" and not self.verified:
            return
        self.sequence += 1
        request = dict(id=self.sequence, cmd=cmd, **fields)
        self.pending = request
        self.deadline = time.monotonic() + (90 if cmd in ("sd", "camera", "prepare", "mode", "file_chunk", "stop") else 5)
        if cmd != "status":
            self.set_controls(False)
        if cmd != "status":
            self.status.set({"camera": "露出確認・撮影・SD保存中…", "sd": "SDの読み書きを確認中…", "prepare": "カメラとSDを再準備しています…"}.get(cmd, "基板に送信中…"))
        if self.demo:
            session = self.session
            self.schedule(120, lambda: self.fake_response(request, session))
            return
        try:
            self.port.write((json.dumps(request) + "\n").encode("ascii"))
        except (serial.SerialException, OSError) as exc:
            self.record(str(exc))
            self.disconnect()

    def fake_response(self, req, session):
        if not self.port or session != self.session:
            return
        if req["cmd"] == "led":
            self.demo_leds[[p for _, p in LEDS].index(req["pin"])] = req["on"]
        elif req["cmd"] == "off":
            self.demo_leds[2:] = [False] * 3
        self.demo_leds[0] = True
        r = dict(id=req["id"], device="camera-board-test-v1", ok=True, error="ESP_OK", leds=self.demo_leds[:], buttons=[False, True], counts=[0, 1], adc=1830, psram=8*1024*1024, ready=True, busy=False, phase="ready", photo_count=0)
        if req["cmd"] == "camera":
            im = Image.new("RGB", (320, 240), "#324964")
            draw = ImageDraw.Draw(im)
            for x in range(0, 320, 40):
                draw.rectangle((x, 50, x+39, 190), fill=(x*255//320, 150, 200-x//2))
            draw.text((16, 16), "DEMO - NO CAMERA CONNECTED", fill="white")
            stream = io.BytesIO()
            im.save(stream, format="JPEG")
            r["jpeg"] = base64.b64encode(stream.getvalue()).decode()
        self.events.put((self.session, r))

    def pump(self):
        try:
            while True:
                session, data = self.events.get_nowait()
                if session != self.session:
                    continue
                if "transport_error" in data:
                    self.record(data["transport_error"])
                    self.disconnect()
                    continue
                photo_event = data.get("event") in ("photo", "mode", "recording")
                if not photo_event and (not self.pending or data.get("id") != self.pending["id"]):
                    continue
                if not valid_response(data):
                    self.record("不正な形式の応答を無視しました。")
                    continue
                command = ("mode" if data.get("event") == "mode" else "camera") if photo_event else self.pending["cmd"]
                if not photo_event:
                    self.pending = None
                if data.get("device") != "camera-board-test-v1":
                    self.disconnect()
                    continue
                self.verified = True
                self.connection.set("デモ接続 / 表示値・画像はサンプルです" if self.demo else "接続済み / 基板応答あり")
                self.hardware_ready = data.get("ready", False)
                self.recording = data.get("recording", False)
                if "mode" in data:
                    mode = data["mode"]
                    if type(mode) is int and 0 <= mode < 8:
                        if mode != self.active_mode or data.get("selecting"):
                            choice = data.get("selected_mode", mode) if data.get("selecting") else mode
                            if type(choice) is int and 0 <= choice < 8:
                                self.mode_choice.set(f"{choice} {MODE_NAMES[choice]}")
                        self.active_mode = mode
                        self.mode_off_button.pack_forget()
                        for button in self.led_buttons[2:]:
                            button.pack_forget()
                        self.capture_button.configure(text=("タイムラプス停止" if mode == 4 else "録画停止") if self.recording else CAPTURE_LABELS[mode])
                self.ap_info.set(f"Wi-Fi: {data.get('ap_ssid','')} / パスワード: {data.get('ap_password','')}\nスマホのブラウザで {data.get('ap_url','http://192.168.4.1')} を開く / 接続 {data.get('ap_clients',0)}台" if data.get("ap_active") else "")
                self.last_video = data.get("last_video", self.last_video)
                self.video_save_button.configure(state="normal" if self.last_video and not data.get("busy") and not self.download else "disabled")
                self.board_busy = data.get("busy", False)
                phase = data.get("phase", "unknown")
                labels = {"starting": "起動中", "starting_ap": "スマホ転送を準備中", "ap_ready": "スマホ転送できます / Ready", "switching": "モード切替中", "checking_sd": "SD確認中", "starting_camera": "カメラ起動中", "warming": "初期画像を安定化中", "queued": "撮影受付", "metering": "露出を確認中", "saving": "SDへ保存中", "sending": "画像受信中", "selecting": "モード選択中 / RV1で選択・SW7で確定", "recording": "録画中 / SW6で停止", "countdown": "セルフタイマー作動中 / SW6で中止", "finalizing": "動画の保存・照合中", "ready": "撮影できます / Ready", "error": "撮影を停止しています"}
                state = labels.get(phase, phase)
                if data.get("last_capture_failed"):
                    state += " / 直前の撮影・保存は失敗"
                if phase == "warming":
                    state += f" / {data.get('warmup_frames', 0)}枚・{data.get('warmup_ms', 0)/1000:.1f}秒"
                if phase == "error":
                    state += f" / {data.get('last_error', '不明')} / SW6または再準備で復帰"
                if data.get("countdown"):
                    state += f" / あと{data.get('countdown_ms', 0)/1000:.0f}秒"
                if self.recording:
                    state += f" / {data.get('record_ms',0)/1000:.1f}秒・{data.get('record_frames',0)}フレーム"
                self.camera_state.set(f"{state} / 写真 {data.get('photo_count', 0)}枚・動画 {data.get('video_count',0)}本")
                self.set_controls(self.pending is None)
                self.demo_leds = data.get("leds", [False]*5)
                for i, on in enumerate(self.demo_leds):
                    self.lamps[i].itemconfigure("lamp", fill=LED_COLORS[LEDS[i][1]] if on else "#a6afba")
                    self.led_buttons[i].configure(text=("点灯中" if on else "消灯中") if i<2 else ("消灯" if on else "点灯"))
                bits = "".join("1" if on else "0" for on in self.demo_leds[2:])
                if self.active_mode is not None:
                    shown = data.get("selected_mode", self.active_mode) if data.get("selecting") else self.active_mode
                    self.mode_display.set(f"{shown:03b} / {shown} {MODE_NAMES[shown]}")
                else:
                    self.mode_display.set(f"{bits}  /  Mode {int(bits, 2)}")
                for i, pressed in enumerate(data.get("buttons", [])):
                    self.buttons[i].set(f"SW{i+6} GPIO{i+47}: {'押下' if pressed else '離している'} / {data['counts'][i]}回")
                value = data.get("adc", -1)
                self.adc_raw = value if value >= 0 else None
                self.adc.set(f"RV1 GPIO2: {value} / 4095" if value >= 0 else "RV1: ADC取得エラー")
                self.meter["value"] = min(self.adc_max, max(0, value))
                size = data.get("psram", 0)
                self.memory.set(f"PSRAM: {size / 1024 / 1024:.1f} MiB" if size else "PSRAM: 未検出")
                if not data.get("ok"):
                    if command == "file_chunk":
                        self.abort_download()
                    msg = f"{command}: {data.get('error', '不明なエラー')}"
                    self.status.set(msg)
                    self.record(msg)
                elif command not in ("status", "file_chunk"):
                    msg = {"sd": "SD読み書き・照合・テストファイル削除: 成功", "camera": "録画を開始しました。SW6または録画停止で終了します。" if self.recording else "セルフタイマーを中止しました。" if data.get("cancelled") else f"連写{data['burst_saved']}枚のSD保存が完了しました。最後の1枚を表示します。" if data.get("burst_saved") else "SD保存が完了しました。" if data.get("saved_path") else "撮影画像を受信しました。", "led": "モードLEDを変更しました。", "off": "モードLEDを消灯しました。", "prepare": "カメラとSDの準備が完了しました。"}.get(command, "操作が完了しました。")
                    if data.get("saved_path"):
                        msg += f" {data['saved_path']}"
                    self.status.set(msg)
                    self.record(msg)
                if "file_data" in data and self.download:
                    self.receive_video_chunk(data)
                if "jpeg" in data:
                    try:
                        content = base64.b64decode(data["jpeg"], validate=True)
                        if "crc32" in data and zlib.crc32(content) != data["crc32"]:
                            raise ValueError("SDに保存した画像と受信画像のCRCが一致しません")
                        im = Image.open(io.BytesIO(content))
                        im.load()
                        self.jpeg = content
                        self.image_info.set(f"{'デモ画像 / ' if self.demo else ''}{im.width} x {im.height} / {len(content):,} bytes / PCへ保存可能")
                        self.save_button.configure(state="normal")
                        self.draw_preview()
                    except (ValueError, OSError) as exc:
                        self.status.set(f"画像の読み込みに失敗: {exc}")
                if self.queued:
                    next_cmd, fields = self.queued
                    self.queued = None
                    self.send(next_cmd, **fields)
        except queue.Empty:
            pass
        if self.pending and time.monotonic() > self.deadline:
            self.record("応答がありません。USB接続と専用ファームの書き込みを確認してください。")
            self.disconnect()
        self.schedule(60, self.pump)

    def poll(self):
        if self.port and not self.pending:
            self.send("status")
        self.schedule(200, self.poll)

    def toggle_led(self, index):
        self.send("led", pin=LEDS[index][1], on=not self.demo_leds[index])

    def calibrate_adc_max(self):
        if self.adc_raw is None or self.adc_raw <= 0 or not self.verified:
            self.status.set("最大位置に回して、有効な値を取得してから設定してください。")
            return
        self.adc_max = self.adc_raw
        self.meter.configure(maximum=self.adc_max, value=self.adc_max)
        self.adc_range.set(f"表示上限 {self.adc_max} / 実測値で設定")
        self.record(f"RV1表示上限を {self.adc_max} に設定しました。ADC生値は変更しません。")

    def oriented_image(self):
        image = Image.open(io.BytesIO(self.jpeg))
        image.load()
        operation = {"上下反転": Image.Transpose.FLIP_TOP_BOTTOM,
                     "180度回転": Image.Transpose.ROTATE_180}.get(self.orientation.get())
        return apply_look(image.transpose(operation) if operation is not None else image, self.look.get())

    def draw_preview(self):
        self.preview.delete("all")
        w, h = self.preview.winfo_width(), self.preview.winfo_height()
        if self.jpeg:
            im = self.oriented_image()
            im.thumbnail((max(1, w-24), max(1, h-24)), Image.Resampling.LANCZOS)
            self.photo = ImageTk.PhotoImage(im)
            self.preview.create_image(w//2, h//2, image=self.photo)
        else:
            self.preview.create_text(w//2, h//2, text="撮影画像がここに表示されます", fill="#d0dceb", font=("Yu Gothic UI", 14))

    def save_image(self):
        if not self.jpeg:
            return
        path = filedialog.asksaveasfilename(defaultextension=".jpg", filetypes=[("JPEG", "*.jpg")], initialfile=time.strftime("camera_%Y%m%d_%H%M%S.jpg"))
        if path:
            try:
                if self.orientation.get() == "そのまま" and self.look.get() == LOOKS[0]:
                    Path(path).write_bytes(self.jpeg)
                else:
                    self.oriented_image().save(path, format="JPEG", quality=95)
                self.record(f"保存: {path}")
            except OSError as exc:
                messagebox.showerror("保存できません", str(exc))

    def close(self):
        self.disconnect()
        for timer in self.timers:
            self.root.after_cancel(timer)
        self.timers.clear()
        self.root.destroy()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", action="store_true")
    args = parser.parse_args()
    root = tk.Tk()
    app = App(root, args.demo)
    root.mainloop()
