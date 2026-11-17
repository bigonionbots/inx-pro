# Not part of ESP-IDF: library.json extraScript. Forces inx_usb_config.h into every source of this
# library (the flags list cannot pass "-include <file>": PlatformIO turns the file into a -l link flag).
import os

Import("env")

env.Append(CCFLAGS=["-include", os.path.join(env.subst("$PROJECT_DIR"), "third_party", "usb_host", "esp_idf_usb", "inx_usb_config.h")])
