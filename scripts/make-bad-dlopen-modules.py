#!/usr/bin/env python3
"""Byte-patches a good ET_DYN module into deliberately-broken fixtures
for test-dlopen.c's error-path checks -- dlopen()'s validation code in
port/dlfcn_shim.c only runs on genuinely malformed input, and there's no
compiler flag that produces "wrong ELF class" or "PT_DYNAMIC missing"
on demand, so these are made by hand instead.

Usage: make-bad-dlopen-modules.py <good.so> <output-dir>
Produces: truncated.so, bad_magic.so, bad_class.so, bad_type.so,
          no_dynamic.so
"""
import struct
import sys
import os

PT_DYNAMIC = 2
PT_NULL = 0


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <good.so> <output-dir>", file=sys.stderr)
        return 1

    src_path, out_dir = sys.argv[1], sys.argv[2]
    with open(src_path, "rb") as f:
        data = bytearray(f.read())

    os.makedirs(out_dir, exist_ok=True)

    # Too small to even hold an Elf64_Ehdr (64 bytes) -- dlopen() must
    # reject this before it dereferences anything into the file.
    with open(os.path.join(out_dir, "truncated.so"), "wb") as f:
        f.write(data[:32])

    # Corrupt e_ident[0] (should be 0x7f) -- fails the ELF magic check.
    bad_magic = bytearray(data)
    bad_magic[0] = 0x00
    with open(os.path.join(out_dir, "bad_magic.so"), "wb") as f:
        f.write(bad_magic)

    # Flip e_ident[EI_CLASS] (offset 4) from ELFCLASS64 (2) to
    # ELFCLASS32 (1) -- fails the "little-endian ELF64" check.
    bad_class = bytearray(data)
    bad_class[4] = 1
    with open(os.path.join(out_dir, "bad_class.so"), "wb") as f:
        f.write(bad_class)

    # Overwrite e_type (offset 16, u16 LE) from ET_DYN (3) to ET_EXEC
    # (2) -- fails the "ET_DYN shared object" check.
    bad_type = bytearray(data)
    struct.pack_into("<H", bad_type, 16, 2)
    with open(os.path.join(out_dir, "bad_type.so"), "wb") as f:
        f.write(bad_type)

    # Walk the program header table and retype the PT_DYNAMIC entry to
    # PT_NULL, so dlopen() finds PT_LOAD segments (still loads fine)
    # but no PT_DYNAMIC -- fails the "module has no PT_DYNAMIC segment"
    # check specifically, rather than failing earlier for an unrelated
    # reason.
    no_dyn = bytearray(data)
    e_phoff, = struct.unpack_from("<Q", no_dyn, 32)
    e_phentsize, = struct.unpack_from("<H", no_dyn, 54)
    e_phnum, = struct.unpack_from("<H", no_dyn, 56)
    patched = False
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from("<I", no_dyn, off)
        if p_type == PT_DYNAMIC:
            struct.pack_into("<I", no_dyn, off, PT_NULL)
            patched = True
            break
    if not patched:
        print("error: no PT_DYNAMIC program header found in input", file=sys.stderr)
        return 1
    with open(os.path.join(out_dir, "no_dynamic.so"), "wb") as f:
        f.write(no_dyn)

    print(f"Wrote truncated.so, bad_magic.so, bad_class.so, bad_type.so, "
          f"no_dynamic.so to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
