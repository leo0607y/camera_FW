"""設定、SDバックアップの記録、HTTPSまわりのテスト（Windows用）。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument("--compiler",default=shutil.which("g++"))
args=parser.parse_args()
if not args.compiler: parser.error("Specify --compiler with a C++17 g++ path")
root=Path(__file__).resolve().parents[1]
json_include=root/".pio/libdeps/esp32s3/ArduinoJson/src"
if not json_include.exists(): parser.error("Run pio pkg install -d Dev first")
build=root/".pio/host-tests"
build.mkdir(parents=True,exist_ok=True)
binary=build/"network.exe"
env=os.environ.copy()
env["PATH"]=str(Path(args.compiler).resolve().parent)+os.pathsep+env.get("PATH","")
sources=[root/"src"/f"{name}.cpp" for name in ("network_settings","backup_store","backup_transport","sd_card","storage_lock")]
subprocess.run([args.compiler,"-std=c++17","-Wall","-Wextra","-Werror","-DNOMINMAX",
    "-I"+str(root/"test/host"),"-I"+str(root/"include"),"-I"+str(root/"src"),"-I"+str(json_include),
    *map(str,sources),str(root/"test/host/test_network.cpp"),"-lbcrypt","-o",str(binary)],env=env,check=True)
scenarios=["settings_roundtrip","settings_preserve_secrets","settings_destination_change","settings_profiles",
    "settings_validation","retry_clock_wrap","sha256","snapshot_and_receipt","card_swap","snapshot_limits",
    "receipt_write_failure","remote_roundtrip","remote_corrupt","remote_short","http_auth_failure","http_redirect",
    "lost_ack_idempotent","capture_during_http","snapshot_remount_midread","snapshot_capture_midread"]
for scenario in scenarios: subprocess.run([str(binary),scenario],env=env,check=True,timeout=20)
print(f"All {len(scenarios)} network/backup scenarios passed.")
runtime=build/"connectivity.exe"
subprocess.run([args.compiler,"-std=c++17","-Wall","-Wextra","-Werror","-DNOMINMAX",
    "-I"+str(root/"test/host"),"-I"+str(root/"include"),"-I"+str(root/"src"),"-I"+str(json_include),
    *map(str,sources),str(root/"src/wifi_manager.cpp"),str(root/"src/led_control.cpp"),
    str(root/"test/host/test_connectivity.cpp"),"-lbcrypt","-o",str(runtime)],env=env,check=True)
runtime_scenarios=["setup_first_boot","setup_access_control","settings_api_save","settings_api_write_failure",
    "setup_timeout","wifi_failover","wifi_reconnect","automatic_backup"]
for scenario in runtime_scenarios: subprocess.run([str(runtime),scenario],env=env,check=True,timeout=20)
print(f"All {len(runtime_scenarios)} connectivity runtime scenarios passed.")
