#!/usr/bin/env python3
"""Record completed local Sweet APFS images and their runtime inputs."""
import hashlib, json, os, pathlib, struct, subprocess
root = pathlib.Path.cwd()
gles = pathlib.Path(os.environ.get('GLES_DIR', 'out/gles-39772020663')).resolve()
firmware = pathlib.Path(os.environ.get('GPU_FW_DIR', '/home/dan/ios-sweet/research/gpu/device-firmware'))
runtime = root/'userland/root'
libraries = {}
for name in ('libOSMesa.8.dylib', 'libredwood-gl.dylib', 'libredwood.dylib', 'libredwood-sb.dylib'):
    data = (runtime/'lib'/name).read_bytes()
    if data != (gles/name).read_bytes():
        raise SystemExit(f'Helper does not contain the matching GLES library: {name}')
    libraries[name] = hashlib.sha256(data).hexdigest()
fw_hashes = {}
for name in ('a618_gmu.bin', 'a630_sqe.fw', 'a615_zap.mdt', 'a615_zap.b02'):
    data = (runtime/'fw'/name).read_bytes()
    if data != (firmware/name).read_bytes():
        raise SystemExit(f'Helper does not contain matching Sweet firmware: {name}')
    fw_hashes[name] = hashlib.sha256(data).hexdigest()
injections = {line.split()[0]: line.split()[1:] for line in (runtime/'etc/inject.conf').read_text().splitlines() if line.split()}
if 'backboardd' not in injections.get('/redwood/lib/libredwood.dylib', []):
    raise SystemExit('Missing backboardd frontend injection')
if 'SpringBoard' not in injections.get('/redwood/lib/libredwood-sb.dylib', []):
    raise SystemExit('Missing SpringBoard injection')
if (runtime/'var/root/.ssh/ssh_host_ed25519_key').exists() or (runtime/'fw/network').exists():
    raise SystemExit('Private credentials must not enter this image')
entries = []
for name in ('root.apfs', 'system.apfs'):
    path = root/'userland'/name
    with path.open('rb') as f:
        header = f.read(4096)
        if header[32:36] != b'NXSB':
            raise SystemExit(f'{path}: missing APFS container header')
        block_size, blocks = struct.unpack_from('<IQ', header, 36)
        if block_size != 4096 or blocks*block_size != path.stat().st_size:
            raise SystemExit(f'{path}: inconsistent APFS container size')
        f.seek(0)
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    entries.append({'file': name, 'bytes': path.stat().st_size, 'sha256': digest})
out = root/'out/root'
out.mkdir(parents=True, exist_ok=True)
(out/'SHA256SUMS').write_text(''.join(f"{e['sha256']}  ../../userland/{e['file']}\n" for e in entries))
(out/'manifest.json').write_text(json.dumps({'ios': '16.5', 'build': '20F66', 'device': 'sweet',
    'source_lock': json.loads((root/'upstream-lock.json').read_text()), 'images': entries,
    'gles': str(gles), 'libraries': libraries, 'firmware': fw_hashes,
    'source_commit': os.environ.get('ROOT_SOURCE_COMMIT') or subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'hardware_tested': False}, indent=2)+'\n')
for entry in entries:
    print(f"{entry['file']}: {entry['bytes']:,} bytes; SHA256 {entry['sha256']}")
