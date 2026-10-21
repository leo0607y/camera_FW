#pragma once
#include <Arduino.h>
static const char SETUP_PAGE[] PROGMEM = R"CAMERA_SETUP(
<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Camera / 接続とバックアップ</title>
<style>
:root{color-scheme:light;--ink:#19302a;--muted:#66746d;--line:#d7dfd9;--accent:#206747;--paper:#f4f6f1}*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:15px/1.7 system-ui,-apple-system,"Noto Sans JP",sans-serif}header{background:#173e31;color:#f9fcf8;padding:38px max(24px,calc((100% - 1040px)/2)) 32px}.eyebrow{font-size:12px;letter-spacing:.16em;font-weight:700;opacity:.7}h1{font-size:clamp(24px,4vw,34px);line-height:1.4;margin:9px 0}header p{margin:0;color:#cfddd4;max-width:670px}main{max-width:1088px;margin:auto;padding:24px}.statusbar{display:flex;gap:24px;justify-content:space-between;align-items:center;padding:18px 0 24px;border-bottom:1px solid var(--line);margin-bottom:28px}.statusbar p{margin:0}.tag{display:inline-block;border:1px solid var(--line);border-radius:20px;padding:4px 13px;font-size:13px;background:white}.grid{display:grid;grid-template-columns:1fr 1fr;gap:24px;align-items:start}section{background:white;border:1px solid var(--line);border-radius:12px;padding:26px}h2{font-size:19px;margin:0 0 6px}.intro,.hint{color:var(--muted);font-size:13px}.intro{margin:0 0 22px}.hint{margin:7px 0 0}label{display:block;font-weight:600;font-size:14px;margin-top:18px}input:not([type=checkbox]),select,textarea{display:block;width:100%;font:inherit;border:1px solid #b6c5bb;border-radius:6px;background:#fff;padding:10px 12px;margin-top:6px;color:var(--ink)}input:focus,select:focus,textarea:focus,button:focus-visible{outline:3px solid #b7d9bd;outline-offset:2px}textarea{min-height:130px;font-family:monospace;font-size:12px;resize:vertical}.check{display:flex;align-items:flex-start;gap:9px;font-weight:400}.check input{margin-top:6px;accent-color:var(--accent);width:16px;height:16px;flex-shrink:0}.inline{display:flex;align-items:center;gap:12px;flex-wrap:wrap}.inline label{margin:0;flex:1}.smallbtn{margin-top:12px}button{font:inherit;font-weight:600;padding:10px 17px;border-radius:6px;border:1px solid #aabcaf;background:white;color:var(--ink);cursor:pointer}button.primary{background:var(--accent);border-color:var(--accent);color:white}button:disabled{opacity:.55;cursor:wait}button:hover:not(:disabled){filter:brightness(.96)}details{border-top:1px solid var(--line);margin-top:24px;padding-top:17px}summary{cursor:pointer;font-size:14px;font-weight:600}.setupkey{margin-top:24px;padding-top:20px;border-top:1px solid var(--line)}.actions{margin-top:24px;padding:24px;background:#e9eee6;border-radius:10px}.actions p{margin:0 0 14px;font-size:13px;color:#4e6255}.buttons{display:flex;flex-wrap:wrap;gap:12px}.notice{display:none;margin-top:18px;padding:16px;background:#fff;border:1px solid var(--line);border-radius:8px;overflow-wrap:anywhere}.notice.visible{display:block}.notice[data-error=true]{border-color:#b85446;color:#843426}.backupstate{padding-top:18px;margin-top:18px;border-top:1px solid var(--line);font-size:13px;overflow-wrap:anywhere}footer{font-size:12px;color:var(--muted);padding:26px 0 10px}input:disabled,textarea:disabled{background:#f5f5f5}@media(max-width:720px){header{padding:28px 20px}.grid{grid-template-columns:1fr}main{padding:20px}.statusbar{align-items:flex-start;gap:12px;flex-direction:column}section{padding:21px}.buttons button{flex:1 1 160px}}
</style>
</head>
<body>
<header><div class="eyebrow">CAMERA / SETUP</div><h1>接続とバックアップ</h1><p>スマートフォンからカメラを設定します。写真はSDカードに保存し、通信できるときにバックアップします。</p></header>
<main>
<div class="statusbar"><div><span class="tag" id="connection">接続状況を確認中</span><p class="hint" id="device">カメラの設定用Wi-Fiに接続してください。</p></div><p class="hint" id="remaining">設定用Wi-Fiは10分で終了します。</p></div>
<form id="settings">
<div class="grid">
<section aria-labelledby="wifi-heading"><h2 id="wifi-heading">1. Wi-Fiを選ぶ</h2><p class="intro">自宅のWi-Fiやスマートフォンのテザリングを選べます。2.4GHzのネットワークを使用してください。</p>
<label for="network">見つかったWi-Fi・保存済みのWi-Fi</label><select id="network"><option value="">検索するか、下にWi-Fi名を入力</option></select><button class="smallbtn" type="button" id="scan">Wi-Fiを検索</button><p class="hint" id="scanstate" role="status"></p>
<label for="ssid">接続するWi-Fi名</label><input id="ssid" maxlength="32" required autocomplete="off" placeholder="ネットワーク名を入力">
<label for="wifi-password">Wi-Fiのパスワード</label><input type="password" id="wifi-password" maxlength="63" autocomplete="new-password" placeholder="保存済みなら空欄で維持">
<label class="check"><input type="checkbox" id="open-wifi">パスワードのないWi-Fiに接続する</label>
<label class="check"><input type="checkbox" id="forget">ほかの保存済みWi-Fiを忘れる</label><p class="hint">最大3件を保存します。選んだWi-Fiを優先し、つながらないときはほかの保存済みWi-Fiを試します。</p>
<div class="setupkey"><label for="setup-password">カメラ設定用Wi-Fiの新しいパスワード</label><input type="password" id="setup-password" minlength="12" maxlength="63" autocomplete="new-password" placeholder="12～63文字。初回は変更が必要"><p class="hint">次にこの設定画面を開くときのパスワードです。保存後に控えてください。変更しない場合は空欄にします。</p></div>
</section>
<section aria-labelledby="backup-heading"><h2 id="backup-heading">2. バックアップ先を決める</h2><p class="intro">WebDAV対応のNASやNextcloudを利用できます。Google Drive・OneDrive・Dropboxへの直接接続は未対応です。</p>
<label class="check"><input type="checkbox" id="backup-enabled">写真を自動でバックアップする</label><p class="hint">有効にすると、SDカードにある既存のJPEG写真も送信対象になります。元の写真は自動削除しません。</p>
<label for="url">WebDAVの保存先フォルダURL</label><input id="url" type="url" maxlength="512" placeholder="https://example.com/dav/camera" autocomplete="off"><p class="hint">保存先のフォルダは、あらかじめ作成してください。</p>
<label for="username">保存先のユーザー名</label><input id="username" maxlength="128" autocomplete="off">
<label for="backup-password">保存先のパスワード・アプリパスワード</label><input id="backup-password" type="password" maxlength="256" autocomplete="new-password" placeholder="同じ保存先なら空欄で維持">
<label class="check"><input type="checkbox" id="clear-password">保存先のパスワードを削除する</label>
<details><summary>証明書・時刻の設定</summary><label for="ca">独自CA証明書（必要な場合のみ）</label><textarea id="ca" maxlength="4096" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----"></textarea><p class="hint">空欄では本体の信頼済み証明書を使用します。HTTPSの証明書検証は省略しません。</p><label for="ntp">時刻サーバー</label><input id="ntp" value="pool.ntp.org" maxlength="128"></details>
<div class="backupstate"><strong>バックアップの状態</strong><p id="backup-status" role="status">設定用Wi-Fiを終了すると、自動転送を開始します。</p><p class="hint" id="uploaded"></p></div>
</section>
</div>
<div class="actions"><p>まず設定を保存し、Wi-Fiの接続状況を確認してください。設定用Wi-Fiに接続している間、自動転送は一時停止します。「バックアップを試す」では実際の写真を1件送信・照合します。</p><div class="buttons"><button class="primary" id="save" type="submit">設定を保存</button><button id="test" type="button">バックアップを試す</button><button id="finish" type="button">設定を終了</button></div><div id="notice" class="notice" role="status" aria-live="polite"></div></div>
</form>
<footer>設定を開き直すには、撮影ボタンを押したまま電源を入れ、起動後に離してください。設定用Wi-Fiに再接続し、ブラウザで http://192.168.4.1 を開きます。</footer>
</main>
<script>
'use strict';
let csrf='',saved=[],pollTimer=null,scanTimer=null,finished=false;
const $=id=>document.getElementById(id);
function notice(message,error=false){$('notice').textContent=message;$('notice').dataset.error=String(error);$('notice').classList.add('visible');}
async function api(path,method='GET',body){const response=await fetch(path,{method,cache:'no-store',headers:{'Content-Type':'application/json','X-Setup-Token':csrf},body:body?JSON.stringify(body):undefined});let data;try{data=await response.json();}catch{throw new Error('カメラから応答を取得できません。設定用Wi-Fiへの接続を確認してください。');}if(!response.ok)throw new Error(data.message||'操作を完了できませんでした。');return data;}
function networkOptions(found=[]){const previous=$('network').value;const select=$('network');select.replaceChildren(new Option('Wi-Fiを選ぶか、名前を手入力',''));const used=new Set();for(const w of [...saved,...found]){if(!w.ssid||used.has(w.ssid))continue;used.add(w.ssid);select.add(new Option(w.ssid+(saved.some(s=>s.ssid===w.ssid)?'（保存済み）':''),w.ssid));}select.value=previous;}
async function scanResults(){try{const data=await api('/api/scan');if(data.scanning){$('scanstate').textContent='Wi-Fiを検索しています。';scanTimer=setTimeout(scanResults,1500);}else{networkOptions(data.networks);$('scanstate').textContent=(data.networks?.length||0)+'件のWi-Fiが見つかりました。';$('scan').disabled=false;}}catch(e){$('scan').disabled=false;notice(e.message,true);}}
async function status(){if(finished)return;try{const s=await api('/api/status');$('connection').textContent=s.connected?'接続済み：'+s.ssid:'接続待ち・オフライン';$('remaining').textContent='設定用Wi-Fiの残り時間：約'+Math.ceil(s.setup_seconds/60)+'分';$('backup-status').textContent=s.backup_message;$('uploaded').textContent='この起動中に照合した写真：'+s.uploaded+'件';if(!s.sd_ready||s.camera_error)$('device').textContent='カメラまたはSDカードの状態を本体で確認してください。';}catch(e){$('connection').textContent='カメラとの接続を確認してください';}pollTimer=setTimeout(status,2500);}
async function load(){try{const s=await api('/api/settings');csrf=s.csrf;saved=s.wifi||[];networkOptions();$('device').textContent='設定用Wi-Fi：'+s.ap_name;$('ssid').value=saved[0]?.ssid||'';$('network').value=$('ssid').value;$('setup-password').required=s.change_setup_password;const b=s.backup;$('backup-enabled').checked=b.enabled;$('url').value=b.url;$('username').value=b.username;$('ca').value=b.ca_pem;$('ntp').value=b.ntp_server;if(s.change_setup_password)notice('初回設定では、カメラ設定用Wi-Fiのパスワードを新しく決めてください。');status();}catch(e){notice(e.message,true);}}
$('network').addEventListener('change',()=>{if($('network').value)$('ssid').value=$('network').value;$('wifi-password').value='';$('open-wifi').checked=false;});
$('scan').addEventListener('click',async()=>{clearTimeout(scanTimer);$('scan').disabled=true;try{await api('/api/rescan','POST');scanResults();}catch(e){$('scan').disabled=false;notice(e.message,true);}});
$('settings').addEventListener('submit',async event=>{event.preventDefault();$('save').disabled=true;try{const payload={wifi:{ssid:$('ssid').value,password:$('wifi-password').value,open:$('open-wifi').checked},forget_others:$('forget').checked,setup_password:$('setup-password').value,backup:{enabled:$('backup-enabled').checked,provider:'webdav',url:$('url').value,username:$('username').value,password:$('backup-password').value,clear_password:$('clear-password').checked,ca_pem:$('ca').value,ntp_server:$('ntp').value}};const data=await api('/api/settings','POST',payload);notice(data.message);$('wifi-password').value='';$('backup-password').value='';$('setup-password').value='';$('setup-password').required=false;$('clear-password').checked=false;const updated=await api('/api/settings');saved=updated.wifi;networkOptions();}catch(e){notice(e.message,true);}finally{$('save').disabled=false;}});
$('test').addEventListener('click',async()=>{try{const data=await api('/api/test','POST');notice(data.message);}catch(e){notice(e.message,true);}});
$('finish').addEventListener('click',async()=>{try{const data=await api('/api/finish','POST');finished=true;clearTimeout(pollTimer);clearTimeout(scanTimer);notice(data.message);$('connection').textContent='設定を終了しました';for(const button of document.querySelectorAll('button'))button.disabled=true;}catch(e){notice(e.message,true);}});
load();
</script>
</body></html>
)CAMERA_SETUP";
