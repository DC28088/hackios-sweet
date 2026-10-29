#!/usr/bin/env python3
"""Stage the pinned HakiOS helper beside the original APFS images; no device access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PIN = '88513176b1e96ecf0b764cd62c6f4e6ac71345e3'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--upstream', type=Path, required=True)
p.add_argument('--original-helper', type=Path, default=Path('userland/root'))
p.add_argument('--output', type=Path, required=True)
p.add_argument('--gles-dir', type=Path, help='verified current bridge build output')
p.add_argument('--no-demo', action='store_true', help='disable inherited diagnostic/CPU benchmark launch job')
a = p.parse_args()
assert subprocess.check_output(['git', '-C', str(a.upstream), 'rev-parse', 'HEAD'], text=True).strip() == PIN
for line in (a.upstream / 'prebuilt/SHA256SUMS').read_text().splitlines():
    parts = line.split()
    if len(parts) != 2:
        continue
    digest, name = parts
    assert hashlib.sha256((a.upstream / name).read_bytes()).hexdigest() == digest, name
# Refuse to replace a previous prepared image or any original input.
a.output.mkdir(parents=True, exist_ok=False)
root = a.output / 'helper-tree'
shutil.copytree(a.original_helper, root, symlinks=True)
if a.no_demo:
    (root / 'LaunchDaemons/com.redwood.demo.plist').unlink(missing_ok=True)
for name, target in [('init', 'sbin/init'), ('child', 'bin/child')]:
    shutil.copy2(a.upstream / 'prebuilt/userland' / name, root / target)
for lib in (a.upstream / 'prebuilt/userland').glob('libhakios*.dylib'):
    shutil.copy2(lib, root / 'lib' / lib.name)
if a.gles_dir is not None:
    assert (a.gles_dir / 'UPSTREAM_COMMIT').read_text().strip() == PIN
    for line in (a.gles_dir / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split()
        assert Path(name).name == name
        assert hashlib.sha256((a.gles_dir / name).read_bytes()).hexdigest() == digest, name
    for name in ['libOSMesa.8.dylib', 'libhakios.gl.dylib', 'libhakios.dylib', 'libhakios.sb.dylib']:
        shutil.copy2(a.gles_dir / name, root / 'lib' / name)
conf = (root / 'etc/inject.conf').read_text()
for old, new in [('libredwood.dylib', 'libhakios.dylib'),
                 ('libredwood-sb.dylib', 'libhakios.sb.dylib'),
                 ('libredwood-media.dylib', 'libhakios.media.dylib'),
                 ('libredwood-app.dylib', 'libhakios.app.dylib'),
                 ('libredwood-tweaks.dylib', 'libhakios.tweaks.dylib')]:
    assert (root / 'lib' / new).is_file(), new
    conf = conf.replace(old, new)
(root / 'etc/inject.conf').write_text(conf)
subprocess.run(['python3', 'tools/apfs/mkapfs.py', '-z', '--plain', 'fw',
                '-o', str(a.output / 'root.apfs'), 'redwood=' + str(root)], check=True)
(a.output / 'source.json').write_text(json.dumps({
    'upstream_commit': PIN, 'prebuilt_date': '2026-10-25 22:53',
    'helper': 'existing iOS16.5 helper layout with verified current init/child/injection libraries',
    'system': 'existing userland/system.apfs, unchanged', 'hardware_verified': False,
    'helper_sha256': hashlib.sha256((a.output / 'root.apfs').read_bytes()).hexdigest(),
    'gles_provenance': (a.gles_dir / 'PROVENANCE').read_text().strip() if a.gles_dir else None,
    'demo_disabled': a.no_demo,
}, indent=2) + '\n')
