"""未接続のWi-FiアダプタでWLAN/HTTP転送を実機確認する。"""
import io
import json
import subprocess
import time
import zlib
from pathlib import Path
from urllib.request import build_opener, ProxyHandler
from urllib.error import HTTPError, URLError
from xml.sax.saxutils import escape
from PIL import Image
from board_client import BoardClient

ROOT=Path(__file__).resolve().parents[1]
opener=build_opener(ProxyHandler({}))

def command(*args):
    result=subprocess.run(args,capture_output=True,check=True,creationflags=0x08000000)
    return result.stdout.decode(errors="replace")

def connect(record):
    s=record["status"];out=Path(record["output"])
    profile="CameraAPTest-"+s["ap_ssid"]
    xml=f'''<?xml version="1.0"?><WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1"><name>{escape(profile)}</name><SSIDConfig><SSID><name>{escape(s['ap_ssid'])}</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>manual</connectionMode><MSM><security><authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{escape(s['ap_password'])}</keyMaterial></sharedKey></security></MSM></WLANProfile>'''
    path=out/"wlan-test.xml";path.write_text(xml,encoding="utf-8")
    try:command("netsh","wlan","add","profile",f"filename={path}","user=current")
    finally:path.unlink(missing_ok=True)
    record["test_profile"]=profile
    (ROOT/"captures/ap_current.json").write_text(json.dumps(record,indent=2),encoding="utf-8")
    command("netsh","wlan","connect",f"name={profile}","interface=Wi-Fi")
    deadline=time.monotonic()+45
    while True:
        try:
            with opener.open("http://192.168.4.1/",timeout=2) as response:
                page=response.read();assert "ライブラリ" in page.decode("utf-8")
            return
        except (OSError,URLError):
            if time.monotonic()>deadline:raise
            time.sleep(.5)

def verify(record):
    out=Path(record["output"]);results=[]
    def get(path):
        with opener.open("http://192.168.4.1"+path,timeout=15) as response:return response.read(),dict(response.headers)
    raw,_=get("/api/files?kind=photo")
    listing=json.loads(raw);assert listing["files"]
    expected=Path(record["photo"]["saved_path"]).name
    assert listing["files"][0]["name"]==expected,listing
    assert all(file["name"].endswith(".JPG") for file in listing["files"])
    data,headers=get("/file/"+expected)
    assert data==(out/"reference.jpg").read_bytes()
    assert zlib.crc32(data)==record["photo"]["crc32"]
    im=Image.open(io.BytesIO(data));im.load();assert im.size==(1600,1200)
    assert "attachment" in headers["Content-Disposition"]
    (out/"wifi-download.jpg").write_bytes(data)
    results.append(dict(test="original JPEG over real Wi-Fi",file=expected,bytes=len(data),crc32=zlib.crc32(data),size=im.size))
    cursor=int(expected[3:8]);older=json.loads(get(f"/api/files?kind=photo&before={cursor}")[0])
    assert all(int(item["name"][3:8])<cursor for item in older["files"])
    videos=json.loads(get("/api/files?kind=video")[0]);assert isinstance(videos["files"],list)
    for path,code in (("/file/IMG00024.TMP",404),("/file/../sdkconfig",404),("/file/IMG00000.JPG",404),("/api/files?kind=bad",400),("/api/files?before=abc",400)):
        try:get(path);raise AssertionError(path)
        except HTTPError as error:assert error.code==code,(path,error.code)
    client=BoardClient("COM14")
    try:
        assert not client.request("camera")["ok"]
        s=client.request("status");assert s["ap_active"] and s["ready"] and s["ap_clients"]>=1 and not s["last_capture_failed"] and not s["leds"][1],s
        assert client.request("select")["ok"]
        assert client.request("cancel")["ok"]
        assert client.request("status")["phase"]=="ap_ready"
    finally:client.close()
    results.append(dict(test="pagination, video list, path validation, AP shutter rejection, selection cancellation",ok=True))
    (out/"http-report.json").write_text(json.dumps(results,indent=2),encoding="utf-8")
    print(json.dumps(results),flush=True)

if __name__=="__main__":
    record=json.loads((ROOT/"captures/ap_current.json").read_text(encoding="utf-8"))
    connect(record);verify(record)
