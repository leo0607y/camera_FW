"""ファームの設定ページをローカルの偽APIで表示して確認する。"""
import json
from pathlib import Path
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from playwright.sync_api import sync_playwright

root=Path(__file__).resolve().parents[1]
source=(root/"src/setup_page.h").read_text(encoding="utf-8")
html=source.split('R"CAMERA_SETUP(\n',1)[1].split('\n)CAMERA_SETUP"',1)[0]
assert not re.search(r"border-(?:left|inline-start)\s*:",html)
assert not re.search(r"[\U0001f300-\U0001faff\u2600-\u27bf]",html)
state={"saved":False,"last_body":None,"csrf":"test-session-token","test":False}

class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def send(self,status,value,content_type="application/json"):
        raw=value.encode() if isinstance(value,str) else json.dumps(value,ensure_ascii=False).encode()
        self.send_response(status); self.send_header("Content-Type",content_type+"; charset=utf-8")
        self.send_header("Content-Length",str(len(raw))); self.end_headers(); self.wfile.write(raw)
    def do_GET(self):
        if self.path=="/": return self.send(200,html,"text/html")
        if self.path=="/api/settings":
            return self.send(200,{"csrf":state["csrf"],"ap_name":"Camera-123456","change_setup_password":not state["saved"],
                "wifi":[{"ssid":state["last_body"]["wifi"]["ssid"] if state["saved"] else "Home Wi-Fi","has_password":True}],
                "backup":{"enabled":False,"url":"","username":"","ca_pem":"","ntp_server":"pool.ntp.org"}})
        if self.path=="/api/status":
            return self.send(200,{"connected":state["saved"],"ssid":state["last_body"]["wifi"]["ssid"] if state["saved"] else "","setup_seconds":540,
                "sd_ready":True,"camera_error":False,"uploaded":1 if state["test"] else 0,
                "backup_message":"バックアップと保存先の照合が完了しました。" if state["test"] else "設定を終了すると自動転送を開始します。"})
        if self.path=="/api/scan":
            return self.send(200,{"scanning":False,"networks":[{"ssid":"Home Wi-Fi"},{"ssid":"Phone Hotspot"},{"ssid":"<img src=x onerror=alert(1)>"}]})
        self.send(404,{"message":"Not found"})
    def do_POST(self):
        if self.headers.get("X-Setup-Token")!=state["csrf"]: return self.send(403,{"message":"Invalid token"})
        body=self.rfile.read(int(self.headers.get("Content-Length",0)))
        if self.path=="/api/settings":
            value=json.loads(body); state["last_body"]=value
            if value["backup"]["enabled"] and not value["backup"]["url"].startswith("https://"):
                return self.send(400,{"message":"保存先はHTTPSのWebDAVフォルダURLにしてください。"})
            state["saved"]=True
            return self.send(200,{"message":"設定を保存しました。接続状況を確認してから設定を終了してください。"})
        if self.path=="/api/test": state["test"]=True
        self.send(200,{"message":"操作を受け付けました。"})

server=ThreadingHTTPServer(("127.0.0.1",0),Handler)
thread=Thread(target=server.serve_forever,daemon=True);thread.start()
artifacts=root/".pio/ui-qa";artifacts.mkdir(parents=True,exist_ok=True)
errors=[]
try:
    with sync_playwright() as p:
        browser=p.chromium.launch(headless=True)
        for name,width,height in [("desktop",1280,1080),("mobile",390,844)]:
            page=browser.new_page(viewport={"width":width,"height":height},device_scale_factor=1)
            page.on("pageerror",lambda error: errors.append(str(error)))
            page.goto(f"http://127.0.0.1:{server.server_port}/")
            page.get_by_text("設定用Wi-Fi：Camera-123456",exact=True).wait_for()
            assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
            assert page.locator("#wifi-password").input_value()==""
            page.get_by_role("button",name="Wi-Fiを検索",exact=True).click()
            page.get_by_text("3件のWi-Fiが見つかりました。",exact=True).wait_for()
            assert page.locator("img").count()==0
            page.locator("#network").select_option("Phone Hotspot")
            assert page.locator("#ssid").input_value()=="Phone Hotspot"
            page.locator("#wifi-password").fill("phone-password")
            page.locator("#setup-password").fill("new-setup-password")
            page.locator("#backup-enabled").check()
            page.locator("#url").fill("http://example.test/dav")
            page.get_by_role("button",name="設定を保存",exact=True).click()
            page.get_by_text("保存先はHTTPSのWebDAVフォルダURLにしてください。",exact=True).wait_for()
            page.locator("#url").fill("https://example.test/dav/camera")
            page.locator("#username").fill("camera-user")
            page.locator("#backup-password").fill("test-app-password")
            page.get_by_role("button",name="設定を保存",exact=True).click()
            page.get_by_text("設定を保存しました。接続状況を確認してから設定を終了してください。",exact=True).wait_for()
            assert state["last_body"]["wifi"]["ssid"]=="Phone Hotspot"
            assert state["last_body"]["backup"]["provider"]=="webdav"
            assert page.locator("#backup-password").input_value()==""
            assert page.locator("#network").input_value()=="Phone Hotspot"
            page.get_by_role("button",name="バックアップを試す",exact=True).click()
            page.get_by_text("バックアップと保存先の照合が完了しました。",exact=True).wait_for(timeout=10000)
            page.screenshot(path=str(artifacts/f"setup-{name}.png"),full_page=True)
            page.get_by_role("button",name="設定を終了",exact=True).click()
            page.get_by_text("設定を終了しました",exact=True).wait_for()
            assert page.locator("#save").is_disabled()
            page.close()
            print(f"PASS setup UI: {name}")
        assert not errors,errors
        browser.close()
finally:
    server.shutdown();server.server_close()
print(f"Screenshots: {artifacts}")
