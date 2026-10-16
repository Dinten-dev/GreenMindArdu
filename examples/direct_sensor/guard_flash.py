"""Reject a 16 MiB partition/bootloader upload to an 8 MiB Direct sensor."""
import re
import sys
from pathlib import Path

Import('env')
sys.path.insert(0, str(Path(env.subst('$PROJECT_DIR')).parents[1] / 'tools'))
from flash_hardware import probe_flash, require_capacity


def guard(source, target, env):
    env.AutodetectUploadPort()
    port = env.subst('$UPLOAD_PORT')
    flash = env.BoardConfig().get('upload.flash_size')
    match = re.fullmatch(r'(\d+)MB', flash)
    if not port or not match:
        raise ValueError('Explicit target flash capacity and upload port required')
    size = probe_flash(port, env.subst('$UPLOADER'))
    require_capacity(size, int(match[1]) * 1024**2)
    print(f'Physical flash verified: {size // 1024**2} MiB; target {flash}')


env.AddPreAction('upload', guard)
