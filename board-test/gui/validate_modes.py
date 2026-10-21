"""全モード、録画、USB取得、NVS保存を実機で確認する。"""
import base64
import io
import json
import shutil
import subprocess
import time
import zlib
from pathlib import Path
from PIL import Image, ImageStat
from board_client import BoardClient


def main():
    out = Path(__file__).resolve().parents[1] / "captures" / time.strftime("modes_%Y%m%d_%H%M%S")
    out.mkdir(parents=True)
    results = []
    client = BoardClient("COM14")
    def request(command, **fields):
        reply = client.request(command, **fields)
        assert reply["ok"], {k:v for k,v in reply.items() if k != "jpeg"}
        return reply
    def ready():
        deadline = time.monotonic()+65
        while True:
            s=request("status")
            # 処理中は緑が点滅するので、たまたま点いているだけのことがある
            assert s["leds"][0] if s["ready"] else s["ready_led"] != "on", s
            if s["ready_led"] == "off": assert not s["leds"][0], s
            if s["ready"]:return s
            assert s["phase"] != "error", s
            assert time.monotonic()<deadline, s
            time.sleep(.2)
    try:
        original=ready()["mode"]
        request("select")
        assert request("status")["selecting"]
        assert not client.request("camera")["ok"]
        request("cancel")
        assert ready()["mode"] == original
        for mode in range(8):
            s=request("mode", value=mode)
            assert s["mode"] == mode and s["ready"] and not s["selecting"], s
            assert s["leds"][2:] == [bool(mode & x) for x in (4,2,1)], s
            if mode == 7:
                continue
            if mode in (0, 1, 2, 3, 5):
                r=request("camera")
                raw=base64.b64decode(r["jpeg"]);assert zlib.crc32(raw)==r["crc32"]
                image=Image.open(io.BytesIO(raw));image.load()
                assert image.size==((800,600) if mode==5 else (1600,1200))
                (out/f"mode{mode}.jpg").write_bytes(raw)
                means=ImageStat.Stat(image.convert("RGB")).mean
                if mode==2:assert r["burst_saved"]==5,r
                result=dict(mode=mode,path=r["saved_path"],size=image.size,mean=means,warmup_ms=s["warmup_ms"])
            else:
                r=request("camera");assert r["recording"] and not r["ready"]
                time.sleep(6)
                assert not client.request("mode",value=0)["ok"]
                request("stop")
                r=ready();assert not r["recording"] and r["last_video_bytes"]>224
                raw=bytearray();crc=0
                while len(raw)<r["last_video_bytes"]:
                    packet=request("file_chunk",offset=len(raw))
                    chunk=base64.b64decode(packet["file_data"]);assert chunk
                    raw.extend(chunk);crc=zlib.crc32(chunk,crc)
                assert crc==r["last_video_crc"] and len(raw)==r["last_video_bytes"]
                path=out/f"mode{mode}.avi";path.write_bytes(raw)
                probe=subprocess.run([shutil.which("ffprobe"),"-v","error","-show_entries","stream=codec_name,width,height,r_frame_rate,nb_frames,duration","-of","json",str(path)],capture_output=True,text=True,check=True,creationflags=0x08000000)
                info=json.loads(probe.stdout)["streams"][0]
                assert info["codec_name"]=="mjpeg" and int(info["nb_frames"])>=2,info
                subprocess.run([shutil.which("ffmpeg"),"-v","error","-i",str(path),"-f","null","-"],capture_output=True,check=True,creationflags=0x08000000)
                result=dict(mode=mode,path=r["last_video"],bytes=len(raw),crc32=crc,probe=info)
            results.append(result);print(json.dumps(result,ensure_ascii=False),flush=True)
        request("mode",value=0)
        request("restart");client.close();time.sleep(.7);client=BoardClient("COM14")
        assert ready()["mode"]==0
        results.append(dict(persistence="mode 0 restored after reboot"))
    finally:
        client.close();(out/"report.json").write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding="utf-8")
        print(out,flush=True)

if __name__ == "__main__":main()
