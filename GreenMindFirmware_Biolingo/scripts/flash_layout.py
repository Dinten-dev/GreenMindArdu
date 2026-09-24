"""Align Arduino's fixed USB flash defaults with our retained 16 MB layout."""

import csv
from pathlib import Path

Import("env")  # noqa: F821 - PlatformIO/SCons injects the build environment

project = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
table = project / env.GetProjectOption("board_build.partitions")  # noqa: F821
rows = {}
for row in csv.reader(table.read_text().splitlines()):
    if not row or row[0].lstrip().startswith("#"):
        continue
    rows[row[0].strip()] = [value.strip() for value in row]

app_offset = int(rows["app0"][3], 0)
ota_offset = int(rows["otadata"][3], 0)
assert app_offset == 0x20000 and ota_offset == 0xF000, "Review changed flash layout"
assert int(rows["otadata"][4], 0) == 0x2000

images = []
for offset, path in env["FLASH_EXTRA_IMAGES"]:  # noqa: F821
    images.append((hex(ota_offset) if Path(path).name == "boot_app0.bin" else offset, path))

flags = list(env["UPLOADERFLAGS"])  # noqa: F821
matches = 0
for index, value in enumerate(flags):
    if Path(str(value)).name == "boot_app0.bin":
        assert index > 0 and flags[index - 1] == "0xe000"
        flags[index - 1] = hex(ota_offset)
        matches += 1
assert matches == 1, "Review changed PlatformIO uploader defaults"
env.Replace(ESP32_APP_OFFSET=hex(app_offset), FLASH_EXTRA_IMAGES=images, UPLOADERFLAGS=flags)  # noqa: F821
