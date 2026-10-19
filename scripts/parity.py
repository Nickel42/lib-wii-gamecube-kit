"""Byte-for-byte parity: stock DolphinTool vs libdolphinrvz's drvz-cli.

    python scripts/parity.py <DolphinTool> <drvz-cli> <work dir> <input image>...

Converts each input with both tools at the same settings (every compression
method, several levels and block sizes, scrubbing, GCZ) and checks the outputs
are identical, and that decoding gives back the input. Build DolphinTool from
Dolphin at vendor/DOLPHIN_COMMIT so both use the same zstd/xz/bzip2 versions
(see NOTES.md). DolphinTool's CLI has no WIA "purge", so that isn't compared.
"""
import hashlib
import os
import subprocess
import sys

dolphin_tool, drvz_cli, work = sys.argv[1:4]
inputs = sys.argv[4:]
os.makedirs(work, exist_ok=True)
user_dir = os.path.join(work, 'dolphin-user')
os.makedirs(user_dir, exist_ok=True)

# (name, format, compression, level, block size, scrub)
cases = [
    ('rvz-zstd5-128k', 'rvz', 'zstd', 5, 131072, False),
    ('rvz-zstd19-2m', 'rvz', 'zstd', 19, 2097152, False),
    ('rvz-zstd1-32k', 'rvz', 'zstd', 1, 32768, False),
    ('rvz-lzma2-6-512k', 'rvz', 'lzma2', 6, 524288, False),
    ('rvz-lzma-9-128k', 'rvz', 'lzma', 9, 131072, False),
    ('rvz-bzip2-9-1m', 'rvz', 'bzip2', 9, 1048576, False),
    ('rvz-none-128k', 'rvz', 'none', 0, 131072, False),
    ('rvz-zstd5-scrub', 'rvz', 'zstd', 5, 131072, True),
    ('wia-lzma-5-2m', 'wia', 'lzma', 5, 2097152, False),
    ('wia-lzma2-9-4m', 'wia', 'lzma2', 9, 4194304, False),
    ('wia-bzip2-9-2m', 'wia', 'bzip2', 9, 2097152, False),
    ('wia-none-2m', 'wia', 'none', 0, 2097152, False),
    ('gcz-32k', 'gcz', None, None, 32768, False),
    ('gcz-16k', 'gcz', None, None, 16384, False),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f'failed ({r.returncode}): {" ".join(args)}\n{r.stdout}\n{r.stderr}')


def dolphin(inp, out, fmt, comp, level, block, scrub):
    args = [dolphin_tool, 'convert', '-u', user_dir, '-i', inp, '-o', out, '-f', fmt]
    if block:
        args += ['-b', str(block)]
    if comp:
        args += ['-c', comp]
        args += ['-l', str(level)]
    if scrub:
        args += ['-s']
    if os.path.exists(out):
        os.remove(out)
    run(args)


def drvz(inp, out, fmt, comp, level, block, scrub):
    args = [drvz_cli, 'convert', '-q', '-y', '-i', inp, '-o', out, '-f', fmt]
    if comp:
        args += ['-c', comp, '-l', str(level)]
    if block:
        args += ['-b', str(block)]
    if scrub:
        args += ['-s']
    run(args)


failures = 0
total = 0
for inp in inputs:
    base = os.path.splitext(os.path.basename(inp))[0]
    print(f'{base} ({os.path.getsize(inp)} bytes)')
    for name, fmt, comp, level, block, scrub in cases:
        a = os.path.join(work, f'{base}.{name}.dolphin.{fmt}')
        b = os.path.join(work, f'{base}.{name}.drvz.{fmt}')
        dolphin(inp, a, fmt, comp, level, block, scrub)
        drvz(inp, b, fmt, comp, level, block, scrub)
        same = sha256(a) == sha256(b)
        total += 1
        failures += not same
        print(f'  {"same" if same else "DIFFERENT":9} {name:18} {os.path.getsize(a):>10} {os.path.getsize(b):>10}')
        # Decoding: both tools turn the container back into the same ISO.
        if name == 'rvz-zstd5-128k' or fmt in ('wia', 'gcz') and name.endswith(('lzma-5-2m', '32k')):
            ia = a + '.iso'
            ib = b + '.iso'
            dolphin(a, ia, 'iso', None, None, None, False)
            drvz(b, ib, 'iso', None, None, None, False)
            same_iso = sha256(ia) == sha256(ib) == sha256(inp)
            total += 1
            failures += not same_iso
            print(f'  {"same" if same_iso else "DIFFERENT":9} {name + " -> iso":18} (and equal to the input)')
            os.remove(ia)
            os.remove(ib)
        os.remove(a)
        os.remove(b)

print(f'{total - failures} of {total} outputs byte-identical')
sys.exit(1 if failures else 0)
