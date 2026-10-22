#!/usr/bin/env python3
"""Fetch one member of the iOS 16.5 iPhone X firmware archive without downloading the rest.

usage: fetch-from-ipsw.py MEMBER OUTPUT

The IPSW is a ZIP64 archive on Apple's CDN; its directory is read with HTTP range requests.
Concurrent connections fetch bounded ranges. Completed ranges survive an interruption,
including compressed members; the assembled file must match the ZIP size and CRC-32.
"""
import concurrent.futures
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import time
import urllib.request
import zlib

CHUNK = 1024 * 1024
WORKERS = int(os.environ.get('IPSW_WORKERS', '24'))

URL = ('https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/'
       'iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw')
SIZE = 6165698198
DATA_URL = os.environ.get('IPSW_DATA_URL', URL)


def fetch(start, end):
    request = urllib.request.Request(URL, headers={'Range': f'bytes={start}-{end}'})
    return urllib.request.urlopen(request, timeout=120)


def directory():
    tail = fetch(SIZE - 65536, SIZE - 1).read()
    at = tail.rfind(b'PK\x06\x06')
    cd_size, cd_offset = struct.unpack('<QQ', tail[at + 40:at + 56])
    cd = fetch(cd_offset, cd_offset + cd_size - 1).read()
    p = 0
    while cd[p:p + 4] == b'PK\x01\x02':
        method, = struct.unpack('<H', cd[p + 10:p + 12])
        crc, = struct.unpack('<I', cd[p + 16:p + 20])
        csize, usize, nl, el, cl = struct.unpack('<IIHHH', cd[p + 20:p + 34])
        offset, = struct.unpack('<I', cd[p + 42:p + 46])
        name = cd[p + 46:p + 46 + nl].decode()
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q + 4 <= len(extra):
            tag, length = struct.unpack('<HH', extra[q:q + 4])
            if tag == 1:
                values = list(struct.unpack('<' + 'Q' * (length // 8), extra[q + 4:q + 4 + length // 8 * 8]))
                if usize == 0xFFFFFFFF:
                    usize = values.pop(0)
                if csize == 0xFFFFFFFF:
                    csize = values.pop(0)
                if offset == 0xFFFFFFFF:
                    offset = values.pop(0)
            q += 4 + length
        yield name, method, csize, usize, offset, crc
        p += 46 + nl + el + cl


def parallel_download(data, method, csize, usize, crc, output):
    """Bounded resumable ranges; assemble only after every response is verified."""
    ranges = pathlib.Path(output + '.ranges')
    ranges.mkdir(exist_ok=True)
    spec = {'url': URL, 'data': data, 'compressed': csize, 'size': usize,
            'method': method, 'crc32': crc, 'chunk_size': CHUNK}
    manifest = ranges/'source.json'
    if manifest.exists() and json.loads(manifest.read_text()) != spec:
        raise SystemExit(f'{ranges}: source differs; preserving existing ranges')
    manifest.write_text(json.dumps(spec))
    items = [(i, off, min(CHUNK, csize-off)) for i, off in enumerate(range(0, csize, CHUNK))]
    # Preserve complete ranges from an earlier single-connection stored-member download.
    if method == 0 and os.path.exists(output):
        with open(output, 'rb') as previous:
            for index, off, length in items:
                if off + length > os.path.getsize(output):
                    break
                chunk = ranges/f'{index:06d}'
                block = previous.read(length)
                if not chunk.exists():
                    chunk.write_bytes(block)

    def one(item):
        index, offset, length = item
        dest = ranges/f'{index:06d}'
        if dest.exists() and dest.stat().st_size == length:
            return length
        start, end = data + offset, data + offset + length - 1
        part, headers = str(dest)+'.part', str(dest)+'.headers'
        for attempt in range(1, 6):
            result = subprocess.run(['curl', '-4', '--http1.1', '-sfL',
                '--connect-timeout', '15', '--max-time', '120',
                '-r', f'{start}-{end}', '-D', headers, '-o', part, DATA_URL], capture_output=True)
            response = pathlib.Path(headers).read_text(errors='replace') if os.path.exists(headers) else ''
            got = re.findall(r'content-range:\s*bytes\s+(\d+)-(\d+)/(\d+)', response, re.I)
            if (result.returncode == 0 and os.path.exists(part) and os.path.getsize(part) == length
                    and got and tuple(map(int, got[-1])) == (start, end, SIZE)):
                os.replace(part, dest)
                os.unlink(headers)
                return length
            print(f'{output}: range {index} retry {attempt}, curl={result.returncode}', flush=True)
            time.sleep(min(2**attempt, 10))
        raise RuntimeError(f'{output}: range {index} failed')

    done, report = 0, 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
        jobs = [pool.submit(one, item) for item in items]
        for job in concurrent.futures.as_completed(jobs):
            done += job.result()
            if time.monotonic()-report >= 15 or done == csize:
                print(f'{output}: {100*done/csize:.1f}% ({done>>20}/{csize>>20} MiB)', flush=True)
                report = time.monotonic()
    inflater = zlib.decompressobj(-15) if method == 8 else None
    checksum, written = 0, 0
    with open(output, 'wb') as out:
        for index, _, _ in items:
            block = (ranges/f'{index:06d}').read_bytes()
            raw = inflater.decompress(block) if inflater else block
            out.write(raw)
            written += len(raw)
            checksum = zlib.crc32(raw, checksum)
        if inflater:
            raw = inflater.flush()
            out.write(raw)
            written += len(raw)
            checksum = zlib.crc32(raw, checksum)
            if not inflater.eof:
                raise RuntimeError('Incomplete deflate stream')
    if written != usize or checksum != crc:
        raise RuntimeError(f'{output}: size or ZIP CRC-32 mismatch; ranges preserved')
    for index, _, _ in items:
        (ranges/f'{index:06d}').unlink()
    manifest.unlink()
    ranges.rmdir()
    print(f'{output}: {written:,} bytes, ZIP CRC-32 verified', flush=True)


def main():
    member, output = sys.argv[1], sys.argv[2]
    for name, method, csize, usize, offset, crc in directory():
        if name != member:
            continue
        if method not in (0, 8):
            sys.exit(f'{member}: compression method {method} is not supported')
        header = fetch(offset, offset + 29).read()
        nl, el = struct.unpack('<HH', header[26:30])
        data = offset + 30 + nl + el
        parallel_download(data, method, csize, usize, crc, output)
        return
    sys.exit(f'{member}: not in the archive')


if __name__ == '__main__':
    main()
