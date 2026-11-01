#!/usr/bin/env python3
"""
Self-contained build script for MaixCAM Native WebRTC + Bluetooth Integration.
Compiles src/maix_webrtc_maixcam.cpp and links build/stream_webrtc-m2.
"""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src/maix_webrtc_maixcam.cpp"
BUILD = ROOT / "build"
META_FILE = BUILD / "build-metadata.json"
BASE_ARCHIVE = BUILD / "libvision.a"

OBJ_OUT = BUILD / "maix_webrtc_maixcam-m2.o"
ARCHIVE_OUT = BUILD / "libvision-m2.a"
BIN_OUT = BUILD / "stream_webrtc-m2"

COMPILE_LOG = BUILD / "compile.log"
LINK_LOG = BUILD / "link.log"

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1048576), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    print("=== MAIXCAM WEBRTC BUILD SYSTEM ===")
    if not SRC.is_file():
        print(f"ERROR: Source file missing at {SRC}", file=sys.stderr)
        sys.exit(1)
    if not BASE_ARCHIVE.is_file():
        print(f"ERROR: Base archive missing at {BASE_ARCHIVE}", file=sys.stderr)
        sys.exit(1)
    if not META_FILE.is_file():
        print(f"ERROR: Metadata missing at {META_FILE}", file=sys.stderr)
        sys.exit(1)

    meta = json.loads(META_FILE.read_text())
    compiler = Path(meta["compile_argv"][0])
    ar = compiler.with_name("riscv64-unknown-linux-musl-ar")
    nm = compiler.with_name("riscv64-unknown-linux-musl-nm")

    if not (compiler.is_file() and ar.is_file() and nm.is_file()):
        print(f"ERROR: Toolchain binaries missing in {compiler.parent}", file=sys.stderr)
        sys.exit(1)

    print("1. Compiling C++ candidate object...")
    compile_cmd = list(meta["compile_argv"])
    src_idx = compile_cmd.index("-c") + 1
    compile_cmd[src_idx] = str(SRC)
    out_idx = compile_cmd.index("-o") + 1
    compile_cmd[out_idx] = str(OBJ_OUT)

    src_inc = str(ROOT / "src")
    base_cdk = Path.home() / "maixcam-build/MaixCDK-opus-g4a"
    alsa_inc = str(base_cdk / "components/3rd_party/alsa_lib/include")

    if f"-I{src_inc}" not in compile_cmd:
        compile_cmd.insert(4, f"-I{src_inc}")
    if f"-I{alsa_inc}" not in compile_cmd:
        compile_cmd.insert(5, f"-I{alsa_inc}")

    with COMPILE_LOG.open("w") as out:
        res = subprocess.run(compile_cmd, cwd=base_cdk / "examples/stream_webrtc/build", stdout=out, stderr=subprocess.STDOUT)
    if res.returncode != 0:
        print("COMPILE=FAIL")
        print(COMPILE_LOG.read_text(errors="replace")[-3000:])
        sys.exit(1)

    print(f"COMPILE=PASS (Object: {OBJ_OUT.name}, {OBJ_OUT.stat().st_size} bytes)")

    print("2. Packing archive...")
    if ARCHIVE_OUT.exists():
        ARCHIVE_OUT.unlink()
    shutil.copy2(BASE_ARCHIVE, ARCHIVE_OUT)

    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        staged_obj = Path(tmpdir) / "maix_webrtc_maixcam.cpp.o"
        shutil.copy2(OBJ_OUT, staged_obj)
        subprocess.run([str(ar), "r", str(ARCHIVE_OUT), str(staged_obj)], check=True)

    print(f"ARCHIVE=PASS ({ARCHIVE_OUT.name})")

    print("3. Linking target binary...")
    link_cmd = list(meta["link_argv"])
    for i, arg in enumerate(link_cmd):
        if arg.endswith("libvision.a"):
            link_cmd[i] = str(ARCHIVE_OUT)
        if arg == "-o":
            link_cmd[i + 1] = str(BIN_OUT)

    alsa_lib_dir = str(base_cdk / "components/3rd_party/alsa_lib/lib/maixcam")
    link_cmd.append(f"-L{alsa_lib_dir}")
    link_cmd.append("-lasound")

    if BIN_OUT.exists():
        BIN_OUT.unlink()

    with LINK_LOG.open("w") as out:
        res = subprocess.run(link_cmd, cwd=base_cdk / "examples/stream_webrtc/build", stdout=out, stderr=subprocess.STDOUT)
    if res.returncode != 0:
        print("LINK=FAIL")
        print(LINK_LOG.read_text(errors="replace")[-3000:])
        sys.exit(1)

    print(f"LINK=PASS ({BIN_OUT.name}, {BIN_OUT.stat().st_size} bytes)")
    print(f"BINARY_SHA256={sha256_file(BIN_OUT)}")
    print("BUILD=SUCCESS")

if __name__ == "__main__":
    main()
