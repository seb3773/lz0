import os
import sys

def read_exact(path, count):
    with open(path, "rb") as f:
        data = f.read(count)
    return data

def build_blob(out_path, file_specs, target_size):
    buf = bytearray()
    file_count = 0
    for path, max_bytes in file_specs:
        if not os.path.exists(path):
            print(f"Warning: file {path} not found")
            continue
        with open(path, "rb") as f:
            data = f.read(max_bytes if max_bytes > 0 else -1)
        needed = target_size - len(buf)
        if len(data) > needed:
            data = data[:needed]
        buf.extend(data)
        file_count += 1
        if len(buf) >= target_size:
            break
    
    # If not full, pad by repeating content
    while len(buf) < target_size:
        rem = target_size - len(buf)
        buf.extend(buf[:rem])
        
    with open(out_path, "wb") as f:
        f.write(buf)
    print(f"Created {out_path}: {len(buf)} bytes ({file_count} files)")

# 1. blob_png_256k (4 PNGs)
png_files_256k = [
    ("/usr/share/pixmaps/bleachbit.png", 0),
    ("/usr/share/pixmaps/co.anysphere.cursor.png", 0),
    ("/usr/share/pixmaps/qoder-ide.png", 0),
    ("/usr/share/pixmaps/antigravity.png", 0),
]
build_blob("bench/blobs/blob_png_256k.bin", png_files_256k, 256 * 1024)

# 2. blob_mix_256k (5 files)
mix_files_256k = [
    ("bench/corpus/bash_x86_64.bin", 64 * 1024),
    ("bench/corpus/lz0_win64.exe", 64 * 1024),
    ("bench/corpus/kennedy.xls", 64 * 1024),
    ("/usr/share/pixmaps/bleachbit.png", 34 * 1024),
    ("bench/corpus/alice29.txt", 30 * 1024),
]
build_blob("bench/blobs/blob_mix_256k.bin", mix_files_256k, 256 * 1024)

# 3. blob_png_512k (8 PNGs)
png_files_512k = [
    ("/usr/share/pixmaps/bleachbit.png", 0),
    ("/usr/share/pixmaps/co.anysphere.cursor.png", 0),
    ("/usr/share/pixmaps/qoder-ide.png", 0),
    ("/usr/share/pixmaps/antigravity.png", 0),
    ("/usr/share/pixmaps/install-debian.png", 0),
    ("/usr/share/pixmaps/peazip.png", 0),
    ("/usr/share/pixmaps/windsurf.png", 0),
    ("/usr/share/pixmaps/bobide.png", 0),
]
build_blob("bench/blobs/blob_png_512k.bin", png_files_512k, 512 * 1024)

# 4. blob_mix_512k (7 files)
mix_files_512k = [
    ("bench/corpus/bash_x86_64.bin", 128 * 1024),
    ("bench/corpus/lz0_win64.exe", 64 * 1024),
    ("/usr/share/pixmaps/bobide.png", 100 * 1024),
    ("/usr/share/pixmaps/co.anysphere.cursor.png", 48 * 1024),
    ("bench/corpus/kennedy.xls", 100 * 1024),
    ("bench/corpus/fields.c", 0),
    ("bench/corpus/alice29.txt", 60 * 1024),
]
build_blob("bench/blobs/blob_mix_512k.bin", mix_files_512k, 512 * 1024)

# 5. blob_png_2m (12 PNGs)
# 5. blob_png_2m (8 distinct large PNGs)
png_files_2m = [
    ("/usr/share/plymouth/themes/solar/star.png", 0),
    ("/usr/share/icons/hicolor/512x512/apps/display-im7.q16.png", 0),
    ("/usr/share/pixmaps/bobide.png", 0),
    ("/usr/share/pixmaps/code-oss.png", 0),
    ("/usr/share/pixmaps/antigravity.png", 0),
    ("/usr/share/kiro/resources/app/extensions/markdown-mermaid-aws/docs/example.png", 0),
    ("/usr/share/kiro/resources/app/extensions/kiro.kiro-agent/packages/continuedev/gui/dist/logos/wizardlm.png", 0),
    ("/usr/share/kiro/resources/app/node_modules/@kiro/sign-in-page/src/components/kiro-logo/ghost.png", 0),
]
build_blob("bench/blobs/blob_png_2m.bin", png_files_2m, 2 * 1024 * 1024)

# 6. blob_mix_2m (10 files)
mix_files_2m = [
    ("bench/corpus/bash_x86_64.bin", 600 * 1024),
    ("bench/corpus/lz0_win64.exe", 144 * 1024),
    ("/usr/share/pixmaps/code-oss.png", 229 * 1024),
    ("/usr/share/pixmaps/antigravity.png", 172 * 1024),
    ("/usr/share/pixmaps/bobide.png", 204 * 1024),
    ("bench/corpus/kennedy.xls", 350 * 1024),
    ("bench/corpus/dickens_1m.txt", 300 * 1024),
    ("bench/corpus/alice29.txt", 50 * 1024),
    ("bench/corpus/fields.c", 0),
    ("bench/corpus/cp.html", 0),
]
build_blob("bench/blobs/blob_mix_2m.bin", mix_files_2m, 2 * 1024 * 1024)

# 7. blob_png_5m (15 distinct large PNGs)
png_files_5m = [
    ("/usr/share/plymouth/themes/solar/star.png", 0),
    ("/usr/share/icons/hicolor/512x512/apps/display-im7.q16.png", 0),
    ("/usr/share/pixmaps/bobide.png", 0),
    ("/usr/share/pixmaps/code-oss.png", 0),
    ("/usr/share/pixmaps/antigravity.png", 0),
    ("/usr/share/kiro/resources/app/extensions/markdown-mermaid-aws/docs/example.png", 0),
    ("/usr/share/kiro/resources/app/extensions/kiro.kiro-agent/packages/continuedev/gui/dist/logos/wizardlm.png", 0),
    ("/usr/share/kiro/resources/app/node_modules/@kiro/sign-in-page/src/components/kiro-logo/ghost.png", 0),
    ("/usr/share/kiro/resources/app/node_modules/@kiro/sign-in-page/1080.png", 0),
    ("/usr/share/kiro/resources/app/out/media/1080.png", 0),
    ("/usr/share/kiro/resources/app/resources/linux/code.png", 0),
    ("/usr/share/bobide/resources/app/resources/linux/code.png", 0),
    ("/usr/share/qoder-ide/resources/app/resources/media/common/rc-banner-dark.png", 0),
    ("/usr/share/qoder-ide/resources/app/resources/media/common/snapshot-illustration-dark.png", 0),
    ("/usr/share/qoder-ide/resources/app/resources/media/common/snapshot-illustration-light.png", 0),
]
build_blob("bench/blobs/blob_png_5m.bin", png_files_5m, 5 * 1024 * 1024)

# 8. blob_mix_5m (15 files)
mix_files_5m = [
    ("bench/corpus/bash_x86_64.bin", 1200 * 1024),
    ("bench/corpus/lz0_win64.exe", 144 * 1024),
    ("bench/corpus/sum", 0),
    ("/usr/share/pixmaps/code-oss.png", 0),
    ("/usr/share/pixmaps/bobide.png", 0),
    ("/usr/share/pixmaps/antigravity.png", 0),
    ("/usr/share/pixmaps/qoder-ide.png", 0),
    ("/usr/share/pixmaps/bleachbit.png", 0),
    ("bench/corpus/kennedy.xls", 800 * 1024),
    ("bench/corpus/dickens_1m.txt", 1000 * 1024),
    ("bench/corpus/ptt5", 500 * 1024),
    ("bench/corpus/lcet10.txt", 400 * 1024),
    ("bench/corpus/alice29.txt", 0),
    ("bench/corpus/fields.c", 0),
    ("bench/corpus/cp.html", 0),
]
build_blob("bench/blobs/blob_mix_5m.bin", mix_files_5m, 5 * 1024 * 1024)
