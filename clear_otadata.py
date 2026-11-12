# After a USB upload, erase the otadata partition so the bootloader starts the image
# just written to ota_0. Without this, if an over-the-air update had switched booting
# to ota_1, a USB upload would appear to do nothing. Adapted from the MCP fuel station.
Import("env")
import os
import subprocess


def clear_otadata(source, target, env):
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy") or "", "esptool.py")
    if not os.path.isfile(esptool):
        print("clear_otadata: esptool not found, skipping")
        return
    cmd = [env.subst("$PYTHONEXE"), esptool, "--chip", "esp32s3"]
    port = env.subst("$UPLOAD_PORT")
    if port:
        cmd += ["--port", port]
    cmd += ["--baud", "921600", "erase_region", "0xe000", "0x2000"]
    print("clear_otadata: erasing otadata so the uploaded firmware boots")
    subprocess.run(cmd, check=False)


env.AddPostAction("upload", clear_otadata)
