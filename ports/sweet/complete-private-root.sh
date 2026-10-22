#!/bin/bash
# Finish a private CI system image with local-only Sweet firmware.
set -euo pipefail
cd "$(dirname "$0")/../.."
base=$(realpath "${1:?usage: bash ports/sweet/complete-private-root.sh PRIVATE_ARTIFACT_DIR}")
(cd "$base" && sha256sum -c SHA256SUMS)
cmp upstream-lock.json "$base/upstream-lock.json"
python3 - "$base/SystemVersion.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'rb') as f:
    version = plistlib.load(f)
if version.get('ProductVersion') != '16.5' or version.get('ProductBuildVersion') != '20F66':
    raise SystemExit(f'Unexpected iOS version: {version}')
print('Apple system version: iOS16.5 / 20F66')
PY
[ ! -e userland/root ] || { echo 'userland/root already exists; preserve or move it before importing' >&2; exit 1; }
staging=$(mktemp -d "$PWD/out/root-import.XXXXXX")
tar --no-same-owner -xzf "$base/root-stage.tar.gz" -C "$staging"
mv "$staging/root" userland/root
rmdir "$staging"
export GPU_FW_DIR="${GPU_FW_DIR:-/home/dan/ios-sweet/research/gpu/device-firmware}"
python3 - <<'PY'
import hashlib, json, os, pathlib
fw = pathlib.Path(os.environ['GPU_FW_DIR'])
dest = pathlib.Path('userland/root/fw')
dest.mkdir(exist_ok=True)
manifest = {item['name']: item for item in json.loads((fw/'manifest.json').read_text())}
for name in ('a618_gmu.bin', 'a630_sqe.fw', 'a615_zap.mdt', 'a615_zap.b02'):
    data = (fw/name).read_bytes()
    if hashlib.sha256(data).hexdigest() != manifest[name]['sha256']:
        raise SystemExit(f'Private firmware checksum mismatch: {name}')
    (dest/name).write_bytes(data)
PY
cp --reflink=auto "$base/system.apfs" userland/system.apfs
APFS_JOBS="${APFS_JOBS:-2}" python3 tools/apfs/mkapfs.py -z --plain fw -o userland/root.apfs redwood=userland/root
ROOT_SOURCE_COMMIT=$(cat "$base/source-commit.txt") python3 ports/sweet/record-root.py
