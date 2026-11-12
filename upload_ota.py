# Upload firmware over home WiFi (env:esp32s3_ota). Like the MCP fuel station's script:
# PlatformIO builds firmware.bin, then curl posts it to the logger, which flashes the
# other OTA slot, checks it and reboots into it. Windows 10/11 include curl.
#
# The logger's web server serves one connection at a time, and an idle browser connection
# (e.g. a Chrome tab left open on the logger's page) can hold it for 5 s at a stretch. So
# rather than waiting minutes on a stuck attempt, curl gives up when nothing moves for
# 20 s, and the upload is tried up to 3 times. Closing the logger's browser tabs first
# avoids the problem altogether.
Import("env")
import subprocess
import sys
import time

ATTEMPTS = 3


def upload_via_ota(source, target, env):
    firmware = str(source[0])
    if "littlefs" in firmware.lower() or "spiffs" in firmware.lower():
        print("Not needed: the web page is built into the firmware, and LittleFS holds flights.")
        return
    host = env.GetProjectOption("upload_port")
    url = f"http://{host}/api/ota"
    print(f"\n>>> OTA upload {firmware} -> {url}")
    for attempt in range(1, ATTEMPTS + 1):
        r = subprocess.run(["curl", "-s", "-S", "-f",
                            "--connect-timeout", "10",
                            "--speed-limit", "1024", "--speed-time", "20",   # stalled for 20 s: give up
                            "--max-time", "180",
                            "-F", f"firmware=@{firmware}", url],
                           capture_output=True, text=True)
        if r.returncode == 0:
            print("Logger replied:", r.stdout.strip())
            print("OTA complete - the logger is rebooting into the new firmware.")
            return
        print(f"Attempt {attempt} of {ATTEMPTS} failed:", (r.stderr or r.stdout).strip())
        if attempt < ATTEMPTS:
            time.sleep(5)
    print("Upload failed. Check the logger is on WiFi (console: net), not downloading from an ECU,")
    print("and close any browser tabs open on the logger's page, then try again.")
    sys.exit(1)


env.Replace(UPLOADCMD=upload_via_ota)
