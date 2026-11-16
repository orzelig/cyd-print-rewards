# Builds one flash image (bootloader + partitions + app) for the Wokwi emulator,
# using the same images and offsets as a real upload.
import os
Import("env")

def merge(source, target, env):
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptool"), "esptool")
    images = []
    for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
        images += [env.subst(offset), env.subst(path)]
    images += [env.subst("$ESP32_APP_OFFSET"), str(target[0])]
    out = os.path.join(env.subst("$BUILD_DIR"), "merged.bin")
    cmd = [esptool, "--chip", "esp32", "merge-bin", "-o", out, "--flash-mode", "dio", "--flash-size", "4MB"] + images
    env.Execute(" ".join('"%s"' % c for c in cmd))

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge)
