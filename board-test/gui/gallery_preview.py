"""埋め込みページをテストデータで表示する（localhostのみ）。"""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs

ROOT=Path(__file__).resolve().parents[1]
PAGE=ROOT/"firmware/main/gallery.html"
PHOTO=ROOT/"captures/final_20261017_182002/trial1.jpg"
DELETED=set()

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        route=urlsplit(self.path)
        if route.path=="/":
            data=PAGE.read_bytes();content_type="text/html; charset=utf-8"
        elif route.path=="/api/albums":
            data=json.dumps(dict(albums=["S0000002","S0000001"],legacy=True,more=False,before=1)).encode();content_type="application/json"
        elif route.path=="/api/files":
            files=[] if parse_qs(route.query).get("kind")==["video"] else [dict(name=f"IMG{i:05}.JPG",path="S0000002/"+f"IMG{i:05}.JPG",bytes=PHOTO.stat().st_size) for i in range(6,0,-1) if f"S0000002/IMG{i:05}.JPG" not in DELETED]
            data=json.dumps(dict(files=files,more=False,before=1)).encode();content_type="application/json"
        elif route.path.startswith("/file/") and route.path.endswith(".JPG"):
            data=PHOTO.read_bytes();content_type="image/jpeg"
        else:
            self.send_error(404);return
        self.send_response(200);self.send_header("Content-Type",content_type);self.send_header("Content-Length",str(len(data)));self.end_headers();self.wfile.write(data)
    def do_POST(self):
        route=urlsplit(self.path);path=parse_qs(route.query).get("path",[""])[0]
        if route.path!="/api/delete" or self.headers.get("X-Camera-Action")!="delete":self.send_error(403);return
        DELETED.add(path);data=b'{"deleted":true}'
        self.send_response(200);self.send_header("Content-Type","application/json");self.send_header("Content-Length",str(len(data)));self.end_headers();self.wfile.write(data)
    def log_message(self,*args):pass

if __name__=="__main__":ThreadingHTTPServer(("127.0.0.1",8766),Handler).serve_forever()
