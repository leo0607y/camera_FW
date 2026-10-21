"""専用ファーム書き込み後のUSB接続・LED出力・撮影・SD検査。"""
import argparse
import base64
import io
import json
import time
from pathlib import Path

import serial
from PIL import Image
from board_test import LEDS, valid_response


def check(port_name, skip_sd=False, skip_camera=False):
    output = Path(__file__).resolve().parents[1] / "captures" / time.strftime("%Y%m%d_%H%M%S")
    output.mkdir(parents=True, exist_ok=True)
    report = {"port": port_name, "results": [], "skipped": (["camera"] if skip_camera else []) + (["sd"] if skip_sd else [])}
    port = serial.Serial(port=None, baudrate=115200, timeout=0.3, write_timeout=2)
    port.port = port_name
    port.dtr = False
    port.rts = False
    sequence = 0

    def request(cmd, **fields):
        nonlocal sequence
        sequence += 1
        port.write(("\n" + json.dumps(dict(id=sequence, cmd=cmd, **fields)) + "\n").encode())
        deadline = time.monotonic() + 30
        buffer = bytearray()
        while time.monotonic() < deadline:
            buffer.extend(port.read(max(1, min(port.in_waiting, 65536))))
            if len(buffer) > 2_000_000:
                raise RuntimeError("受信サイズ上限超過")
            while b"\n" in buffer:
                line, _, rest = buffer.partition(b"\n")
                buffer = bytearray(rest)
                try:
                    data = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if not isinstance(data, dict) or data.get("id") != sequence:
                    continue
                if not valid_response(data):
                    raise RuntimeError("専用ファームの有効な応答ではありません")
                summary = {k: v for k, v in data.items() if k != "jpeg"}
                report["results"].append(dict(command=cmd, **fields, response=summary))
                print(json.dumps(dict(command=cmd, **fields, response=summary), ensure_ascii=False), flush=True)
                if not data["ok"]:
                    raise RuntimeError(f"{cmd}: {data['error']}")
                return data
        raise TimeoutError(f"{cmd}: 応答なし")

    try:
        port.open()
        deadline=time.monotonic()+60
        while True:
            status=request("status")
            if status.get("ready"):
                break
            if status.get("phase")=="error":
                raise RuntimeError(f"準備失敗: {status.get('last_error')} / {status.get('sd_stage')}")
            if time.monotonic()>deadline:
                raise TimeoutError("Readyを待機できませんでした")
            time.sleep(.25)
        request("off")
        for index, (_, pin) in enumerate(LEDS):
            if index<2:
                continue # Ready/Error are automatic status indicators.
            data = request("led", pin=pin, on=True)
            assert data["leds"][index], f"GPIO{pin} ON応答の不一致"
            time.sleep(0.25)
            data = request("led", pin=pin, on=False)
            assert not data["leds"][index], f"GPIO{pin} OFF応答の不一致"
        for i in range(0 if skip_camera else 2):
            data = request("camera")
            content = base64.b64decode(data["jpeg"], validate=True)
            image = Image.open(io.BytesIO(content))
            image.load()
            if image.format != "JPEG" or image.size != (1600, 1200):
                raise RuntimeError(f"予期しない画像形式: {image.format} {image.size}")
            path = output / f"capture_{i+1}.jpg"
            path.write_bytes(content)
            print(f"JPEG saved: {path} ({len(content)} bytes)", flush=True)
        if not skip_sd:
            request("sd")
        report["success"] = True
    except Exception as exc:
        report["success"] = False
        report["error"] = str(exc)
        raise
    finally:
        port.close()
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"Report: {output / 'report.json'}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--skip-sd", action="store_true")
    parser.add_argument("--skip-camera", action="store_true")
    args = parser.parse_args()
    check(args.port, args.skip_sd, args.skip_camera)
