"""撮影を何通りかの撮り方で繰り返して、結果と基板のデバッグログを board-test/logs/ に残す。GUIを閉じてから使う。

  python stress_photo.py --mode 1            # 高精細で100回（5通り）
  python stress_photo.py --mode 1 --scale 0.1   # 回数を1/10に
"""
import argparse
import base64
import datetime
import json
import pathlib
import sys
import time
import zlib

from serial.tools import list_ports
from board_client import BoardClient

SCENARIOS = [  # 名前, 回数, 説明
    ("back_to_back", 30, "返信が来たらすぐ次"),
    ("after_cleanup", 20, "縞除去が終わるのを待ってから"),
    ("interval_3s", 20, "返信から3秒あけて"),
    ("status_polling", 20, "撮影中も0.3秒ごとに状態取得（GUIと同じ）"),
    ("reprepare", 10, "毎回prepareしてから"),
]


def find_port():
    ports = [p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]
    return ports[0] if len(ports) == 1 else "COM14"


class Runner:
    def __init__(self, port, out):
        self.client = BoardClient(port)
        self.out = out
        self.log_position = 0
        self.log_file = open(out / "board.log", "a", encoding="utf-8")

    def pull_log(self):
        while True:
            r = self.client.request("log", **{"from": self.log_position}, timeout=10)
            if r["from"] > self.log_position and self.log_position:
                self.log_file.write(f"[{r['from'] - self.log_position} bytes of log lost]\n")
            if r["head"] < self.log_position:  # 電源が切れてリングが初期化された
                self.log_position = 0
                continue
            self.log_file.write(r["log"])
            self.log_position = r["next"]
            if self.log_position >= r["head"]:
                break
        self.log_file.flush()

    def status(self):
        return self.client.request("status", timeout=10)

    def wait_ready(self, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                s = self.status()
                if s.get("ready"):
                    return s
            except TimeoutError:
                pass
            time.sleep(0.5)
        return None

    def wait_cleanup(self, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            s = self.status()
            if not s["cleanup_pending"]:
                return
            time.sleep(0.5)

    def camera_polling(self, timeout=90):
        """撮影要求を出したあと、返事が来るまで状態取得を0.3秒ごとに挟む"""
        c = self.client
        c.sequence += 1
        shot_id = c.sequence
        c.serial.write(("\n" + json.dumps({"id": shot_id, "cmd": "camera"}) + "\n").encode())
        deadline = time.monotonic() + timeout
        next_poll = time.monotonic() + 0.3
        while time.monotonic() < deadline:
            if time.monotonic() >= next_poll:
                c.sequence += 1
                c.serial.write(("\n" + json.dumps({"id": c.sequence, "cmd": "status"}) + "\n").encode())
                next_poll = time.monotonic() + 0.3
            c.buffer.extend(c.serial.read(max(1, min(c.serial.in_waiting, 65536))))
            while b"\n" in c.buffer:
                line, _, rest = c.buffer.partition(b"\n")
                c.buffer = bytearray(rest)
                try:
                    reply = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if isinstance(reply, dict) and reply.get("id") == shot_id:
                    return reply
        raise TimeoutError("camera (polling)")

    def shoot(self, scenario):
        started = time.monotonic()
        record = {"scenario": scenario, "time": datetime.datetime.now().isoformat(timespec="seconds")}
        try:
            if scenario == "reprepare":
                p = self.client.request("prepare", timeout=90)
                record["prepare_ok"] = p.get("ok")
                record["prepare_error"] = p.get("error")
            reply = self.camera_polling() if scenario == "status_polling" else self.client.request("camera", timeout=90)
            record.update(ok=reply.get("ok"), error=reply.get("error"), saved=reply.get("saved_path"),
                          save_ms=reply.get("save_ms"), image_sent=reply.get("image_sent"),
                          quality=reply.get("jpeg_quality"), boost=reply.get("quality_boost"),
                          pending=reply.get("cleanup_pending"), psram_free=reply.get("heap_psram"),
                          psram_block=reply.get("heap_psram_block"), int_free=reply.get("heap_internal"))
            if "jpeg" in reply:
                data = base64.b64decode(reply["jpeg"])
                record["bytes"] = len(data)
                record["crc_match"] = zlib.crc32(data) == reply.get("crc32")
        except TimeoutError as e:
            record.update(ok=False, error=f"PC timeout: {e}")
        record["seconds"] = round(time.monotonic() - started, 2)
        return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?")
    parser.add_argument("--mode", type=int, default=1)
    parser.add_argument("--scale", type=float, default=1.0)
    args = parser.parse_args()
    out = pathlib.Path(__file__).resolve().parent.parent / "logs" / f"stress-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    out.mkdir(parents=True)
    runner = Runner(args.port or find_port(), out)
    results = open(out / "results.jsonl", "w", encoding="utf-8")
    before = runner.wait_ready()
    if before is None:
        sys.exit("基板がReadyになりません")
    original_mode = before["mode"]
    runner.pull_log()
    try:
        if original_mode != args.mode:
            r = runner.client.request("mode", value=args.mode, timeout=120)
            print("mode", args.mode, r.get("error"), flush=True)
        for name, count, _ in SCENARIOS:
            count = max(1, round(count * args.scale))
            for i in range(count):
                if runner.wait_ready() is None:
                    record = {"scenario": name, "ok": False, "error": "not ready within 120 s"}
                else:
                    if name == "after_cleanup":
                        runner.wait_cleanup()
                    record = runner.shoot(name)
                    if name == "interval_3s":
                        time.sleep(3)
                try:
                    runner.pull_log()
                    s = runner.status()
                    record.update(reset_reason=s["reset_reason"], cleanup_done=s["cleanup_done"], cleanup_failed=s["cleanup_failed"])
                except TimeoutError:
                    record["after"] = "status timeout"
                results.write(json.dumps(record, ensure_ascii=False) + "\n")
                results.flush()
                print(name, i + 1, record.get("ok"), record.get("error"), record.get("seconds"), record.get("bytes"), flush=True)
        runner.wait_cleanup(300)
        runner.pull_log()
    finally:
        if original_mode != args.mode and runner.wait_ready():
            runner.client.request("mode", value=original_mode, timeout=120)
        runner.pull_log()
        runner.client.close()
    print("saved", out)


if __name__ == "__main__":
    main()
