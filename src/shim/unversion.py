# Neutralise DT_VERSYM/DT_VERNEED/DT_VERNEEDNUM so glibc ignores bionic symbol versions.
import struct, sys
DT = {0x6ffffff0: 'VERSYM', 0x6ffffffe: 'VERNEED', 0x6fffffff: 'VERNEEDNUM'}
IGNORED = 0x6ffffdf5  # DT_GNU_PRELINKED: ignored by ld.so at runtime
for path in sys.argv[1:]:
    with open(path, 'r+b') as f:
        d = bytearray(f.read())
        phoff, = struct.unpack_from('<Q', d, 0x20); phnum, = struct.unpack_from('<H', d, 0x38)
        for i in range(phnum):
            o = phoff + i * 56
            ptype, = struct.unpack_from('<I', d, o)
            if ptype != 2: continue  # PT_DYNAMIC
            off, = struct.unpack_from('<Q', d, o + 8); sz, = struct.unpack_from('<Q', d, o + 32)
            for e in range(off, off + sz, 16):
                tag, = struct.unpack_from('<q', d, e)
                if tag == 0x1e:  # DT_FLAGS: add DF_SYMBOLIC so each lib uses its own libc++abi
                    v, = struct.unpack_from('<Q', d, e + 8); struct.pack_into('<Q', d, e + 8, v | 0x2)
                if tag in DT:
                    struct.pack_into('<q', d, e, IGNORED); print(path.split('/')[-1], 'removed', DT[tag])
        f.seek(0); f.write(d)
