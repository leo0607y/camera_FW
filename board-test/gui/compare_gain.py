"""同じシーンでゲイン上限を比べる。"""
import base64
import io
import json
import time
from pathlib import Path
from PIL import Image
from board_client import BoardClient
from characterize import metrics

output=Path(__file__).resolve().parents[1]/"captures"/time.strftime("gain_study_%Y%m%d_%H%M%S")
output.mkdir(parents=True)
client=BoardClient("COM14")
results=[]
try:
    while True:
        state=client.request("status")
        if state.get("ready"):break
        if state.get("phase")=="error":raise RuntimeError(state)
        time.sleep(.3)
    for limit in (0,1,2):
        prepared=client.request("tune",gain_limit=limit)
        if not prepared.get("ok"):raise RuntimeError(prepared)
        for number in range(1,3):
            started=time.monotonic()
            reply=client.request("camera")
            if not reply.get("ok"):raise RuntimeError(reply)
            jpeg=base64.b64decode(reply.pop("jpeg"),validate=True)
            image=Image.open(io.BytesIO(jpeg));image.load()
            name=f"gain{2**(limit+1)}x_{number}.jpg"
            (output/name).write_bytes(jpeg)
            item={**reply,**metrics(image),"name":name,"seconds":time.monotonic()-started}
            item["curtain_noise"]=metrics(image.crop((50,50,350,400)))["row_noise"]
            item["shadow_noise"]=metrics(image.crop((1100,750,1500,1150)))["row_noise"]
            results.append(item)
            print(json.dumps(item,ensure_ascii=False),flush=True)
            (output/"report.json").write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding="utf-8")
finally:
    client.close()
    print("REPORT:",output,flush=True)
