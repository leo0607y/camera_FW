"""起動直後のフレームを捨てずに記録して、色の落ち着き方と横縞を比べる。"""
import argparse
import base64
import hashlib
import io
import json
import time
from pathlib import Path
from PIL import Image, ImageStat, ImageChops, ImageDraw
from board_client import BoardClient


def metrics(image):
    rgb = image.convert("RGB")
    means = ImageStat.Stat(rgb.resize((200, 150))).mean
    rows = list(rgb.resize((1, rgb.height), Image.Resampling.BOX).getdata())
    noise = sum(abs(rows[y][c] - (rows[y-1][c] + rows[y+1][c])/2)
                for y in range(1, len(rows)-1) for c in range(3)) / ((len(rows)-2)*3)
    total = max(1, sum(means))
    return dict(rgb_mean=means, rgb_fraction=[m/total for m in means],
                row_noise=noise, luma=sum(m*w for m, w in zip(means, (.2126, .7152, .0722))))


def run(port, clocks, trials, count, output, profiles):
    output.mkdir(parents=True, exist_ok=True)
    report = dict(started=time.strftime("%Y-%m-%d %H:%M:%S"), trials=[])
    client = BoardClient(port)
    try:
        for cmd in ("status", "sd", "sd_info"):
            reply = client.request(cmd)
            report[cmd] = reply
            print(cmd, json.dumps(reply, ensure_ascii=False), flush=True)
        for clock, profile in [(clock, profile) for clock in clocks for profile in profiles]:
            for trial in range(1, trials+1):
                folder = output / f"clock{clock}_profile{profile}_trial{trial}"
                folder.mkdir()
                data = dict(clock_mhz=clock, profile=profile, trial=trial, frames=[])
                report["trials"].append(data)
                images = []
                started = time.monotonic()
                while True:
                    state=client.request("status")
                    if "ready" not in state or state.get("ready"):
                        break
                    if state.get("phase")=="error":
                        raise RuntimeError(f"Camera not ready: {state.get('last_error')}")
                    if time.monotonic()-started>30:
                        raise TimeoutError("Camera preparation exceeded 30 seconds")
                    time.sleep(.25)
                for reply in client.responses("series", expected=count, clock_mhz=clock, count=count, profile=profile):
                    if not reply.get("ok"):
                        data["error"] = reply
                        break
                    jpeg = base64.b64decode(reply.pop("jpeg"), validate=True)
                    image = Image.open(io.BytesIO(jpeg)); image.load()
                    index = reply["series_index"]
                    (folder / f"frame_{index:02}.jpg").write_bytes(jpeg)
                    item = dict(reply, **metrics(image), bytes=len(jpeg), sha256=hashlib.sha256(jpeg).hexdigest())
                    data["frames"].append(item)
                    images.append(image.resize((240, 180)))
                if images:
                    reference = images[-1]
                    for item, image in zip(data["frames"], images):
                        item["difference_to_last"] = sum(ImageStat.Stat(ImageChops.difference(image, reference)).mean)/3
                    grid = Image.new("RGB", (4*240, ((len(images)+3)//4)*210), "#eef2f6")
                    draw = ImageDraw.Draw(grid)
                    for i, (image, item) in enumerate(zip(images, data["frames"])):
                        x, y = (i%4)*240, (i//4)*210
                        grid.paste(image, (x, y+25))
                        draw.text((x+4, y+4), f"#{i+1} {item['camera_ms']:.0f}ms", fill="#142236")
                    grid.save(folder / "contact.jpg")
                    print(f"clock={clock} profile={profile} trial={trial} frames={len(images)} duration={time.monotonic()-started:.1f}s "
                          f"first_rgb={data['frames'][0]['rgb_mean']} last_rgb={data['frames'][-1]['rgb_mean']} "
                          f"last_noise={data['frames'][-1]['row_noise']:.2f}", flush=True)
                (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    finally:
        client.close()
        (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"REPORT: {output / 'report.json'}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM14")
    parser.add_argument("--clocks", nargs="+", type=int, default=[20, 10, 16])
    parser.add_argument("--trials", type=int, default=2)
    parser.add_argument("--profiles", nargs="+", type=int, default=[0])
    parser.add_argument("--count", type=int, default=12)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1]/"captures"/time.strftime("study_%Y%m%d_%H%M%S"))
    args = parser.parse_args()
    run(args.port, args.clocks, args.trials, args.count, args.output, args.profiles)
