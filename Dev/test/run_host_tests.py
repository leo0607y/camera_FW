"""本番のソースを偽ハードと一緒にビルドしてテストを回す。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default=shutil.which("g++"))
args = parser.parse_args()
if not args.compiler:
    parser.error("Pass --compiler with the path to g++ (C++17 or newer)")
root = Path(__file__).resolve().parents[1]
build = root / ".pio" / "host-tests"
build.mkdir(parents=True, exist_ok=True)
binary = build / ("stability.exe" if os.name == "nt" else "stability")
env = os.environ.copy()
env["PATH"] = str(Path(args.compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
sources = [root / "src" / f"{name}.cpp" for name in ("camera_control", "sd_card", "storage_lock", "led_control", "main")]
subprocess.run([args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(root / "test" / "host"), "-I" + str(root / "include"),
                "-I" + str(root / "src"), *map(str, sources),
                str(root / "test" / "host" / "test_stability.cpp"), "-o", str(binary)],
               env=env, check=True)
scenarios = ["existing_photos", "restart_and_temporary", "short_write", "flush_truncation",
             "rename_failure", "invalid_input", "open_failure", "cooldown_hold", "bounce",
             "debounce_clock_wrap", "sd_recovery", "camera_startup_failure", "capture_failure",
             "empty_frame", "led_state_and_logging", "warmup_failure", "readback_corruption",
             "readback_failure", "large_photo", "cooldown_boundary", "cooldown_clock_wrap",
             "final_capture_failure", "startup_held", "busy_press", "press_during_write",
             "save_failure_recovery", "reset_failure", "cooperative_warmup"]
for scenario in scenarios:
    subprocess.run([str(binary), scenario], env=env, check=True)
print(f"All {len(scenarios)} regression scenarios passed.")
