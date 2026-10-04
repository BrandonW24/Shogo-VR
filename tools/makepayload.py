#!/usr/bin/env python3
"""Makes payload.bin - the files the Shogo VR installer carries - for setup.rc.

Usage: makepayload.py <output payload.bin> <ShogoVRBridge.exe> <ShogoVR.exe> <Shogo VR Guide.txt> <NOTICE.txt>

The installer builder later adds CShell.dll and AUTHORS.txt to this payload
(ShogoVR-InstallerBuilder.exe --build CShell.dll).
Format (read by PakDeserialize in launcher/Common.h): magic "SHOGOVR-RES-v1"
padded to 16 bytes, file count (u32), per file: name length (u32), name,
size (u64); then the files' data in the same order.
"""
import struct, sys

def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    names = ["ShogoVR/ShogoVRBridge.exe", "ShogoVR/ShogoVR.exe", "ShogoVR/Shogo VR Guide.txt", "ShogoVR/NOTICE.txt"]
    datas = [open(p, "rb").read() for p in sys.argv[2:6]]
    out = bytearray(b"SHOGOVR-RES-v1".ljust(16, b"\0"))
    out += struct.pack("<I", len(names))
    for n, d in zip(names, datas):
        nb = n.encode("utf-8")
        out += struct.pack("<I", len(nb)) + nb + struct.pack("<Q", len(d))
    for d in datas:
        out += d
    open(sys.argv[1], "wb").write(out)
    print(f"{sys.argv[1]}: {len(names)} files, {len(out)} bytes")

if __name__ == "__main__":
    main()
