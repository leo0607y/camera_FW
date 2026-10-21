"""基板のデバッグログを吸い出して board-test/logs/ に保存する。GUIを閉じてから使う。

  python pull_log.py            # COMは自動（ESP32-S3が1台のとき）
  python pull_log.py COM14 --clear   # 読んだあと基板側を空にする

基板側は内部RAMに24KB（再起動をまたいで残る、電源を切ると消える）。SDの /DEBUG.LOG にも追記している。
"""
import argparse
import datetime
import pathlib
import sys

from serial.tools import list_ports
from board_client import BoardClient


def find_port():
    ports = [p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]
    if len(ports) != 1:
        sys.exit(f"ESP32-S3が一意に見つかりません: {ports}。COMを指定してください")
    return ports[0]


def pull(client):
    text, position = [], 0
    while True:
        reply = client.request("log", **{"from": position}, timeout=10)
        if not reply.get("ok"):
            raise RuntimeError(reply.get("error"))
        if reply["from"] > position and position:
            text.append(f"[{reply['from'] - position} bytes of log lost]\n")
        text.append(reply["log"])
        position = reply["next"]
        if position >= reply["head"]:
            return "".join(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?")
    parser.add_argument("--clear", action="store_true")
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent / "logs")
    args = parser.parse_args()
    client = BoardClient(args.port or find_port())
    try:
        text = pull(client)
        if args.clear:
            client.request("log_clear", timeout=10)
    finally:
        client.close()
    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / f"board-{datetime.datetime.now():%Y%m%d-%H%M%S}.log"
    path.write_text(text, encoding="utf-8")
    print(f"{path} ({len(text)} bytes)")


if __name__ == "__main__":
    main()
