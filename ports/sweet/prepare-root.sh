#!/bin/bash
# Prepare local Apple APFS images. No phone access or partition writes.
set -euo pipefail
cd "$(dirname "$0")/../.."
GLES_DIR=$(realpath "${GLES_DIR:-out/gles-39772020663}")
GPU_FW_DIR=$(realpath "${GPU_FW_DIR:-/home/dan/ios-sweet/research/gpu/device-firmware}")
export GLES_DIR GPU_FW_DIR
python3 - <<'PY'
import hashlib, json, os, pathlib, shutil
root = pathlib.Path.cwd()
gles = pathlib.Path(os.environ['GLES_DIR'])
firmware = pathlib.Path(os.environ['GPU_FW_DIR'])
if json.loads((gles/'upstream-lock.json').read_text()) != json.loads((root/'upstream-lock.json').read_text()):
    raise SystemExit('GLES artifact has a different source lock')
expected = dict(line.split()[::-1] for line in (gles/'SHA256SUMS').read_text().splitlines())
for name in ('libOSMesa.8.dylib', 'libredwood-gl.dylib', 'libredwood.dylib', 'libredwood-sb.dylib'):
    data = (gles/name).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected.get(name):
        raise SystemExit(f'GLES checksum mismatch: {name}')
    if data[:4] != bytes.fromhex('cffaedfe'):
        raise SystemExit(f'GLES library is not Mach-O 64: {name}')
manifest = {item['name']: item for item in json.loads((firmware/'manifest.json').read_text())}
dest = root/'fw-gpu'
dest.mkdir(exist_ok=True)
for name in ('a618_gmu.bin', 'a630_sqe.fw', 'a615_zap.mdt', 'a615_zap.b02'):
    data = (firmware/name).read_bytes()
    if hashlib.sha256(data).hexdigest() != manifest[name]['sha256']:
        raise SystemExit(f'Private firmware checksum mismatch: {name}')
    (dest/name).write_bytes(data)
available = shutil.disk_usage(root).free / (1024**3)
allocated = sum(p.stat().st_blocks*512 for p in (root/'ios165').rglob('*') if p.is_file()) / (1024**3)
if available + allocated < 12 and not (root/'ios165/.done-cryptex').exists():
    raise SystemExit(f'Root preparation needs 12 GiB free; available: {available:.1f} GiB. Firmware and GLES verified.')
print(f'GLES and Sweet firmware verified; {available:.1f} GiB free', flush=True)
PY
bash ios165/fetch.sh
# Do not inherit optional experiments, credentials or device-flash modes.
env -u CHECK -u FLASHBOOT -u CRYPTEXES -u ADSP -u METAL -u AIR -u STOREFRONT \
    -u JB -u JOBS -u INJECT -u RC -u RC_JOBS -u EXTRA_SHARE -u QUIET \
    PREBUILT=1 GL=1 REST=1 SHARE=1 WIFID=0 APFS_JOBS="${APFS_JOBS:-2}" GLES_PREBUILT_DIR="$GLES_DIR" \
    bash userland/build.sh
python3 ports/sweet/record-root.py
