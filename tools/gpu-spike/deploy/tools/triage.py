# triage.py <minidump>: bugcheck, faulting module+RVA and the stack's driver addresses from a Windows kernel minidump
import struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'PAGEDU64', b[:8]
code, = struct.unpack_from('<I', b, 0x38); params = struct.unpack_from('<4Q', b, 0x40)
print('bugcheck 0x%x' % code, ' '.join('0x%x' % p for p in params))
t = 0x2000
f = struct.unpack_from('<16I', b, t)
ctx_off, call_off, call_size, drv_off, drv_cnt, pool_off = f[3], f[10], f[11], f[12], f[13], f[14]
mods = []
for i in range(drv_cnt):
    e = drv_off + i * 0x90  # DUMP_DRIVER_ENTRY64: name offset (padded to 8), then KLDR_DATA_TABLE_ENTRY
    name_off, = struct.unpack_from('<I', b, e)
    base, entry, size = struct.unpack_from('<QQI', b, e + 0x38)
    n, = struct.unpack_from('<I', b, name_off)
    name = b[name_off + 4:name_off + 4 + n * 2].decode('utf-16-le', 'replace')
    mods.append((base, size, name))
def where(a):
    for base, size, name in mods:
        if base <= a < base + size:
            return '%s+0x%x' % (name, a - base)
    return None
print('fault', where(params[1]))
# stack: every value that lands in a module
stack = b[call_off:call_off + call_size]
for i in range(0, len(stack) - 7, 8):
    v, = struct.unpack_from('<Q', stack, i)
    w = where(v)
    if w and 'viogpu' in w.lower():
        print('stack +0x%x: %s' % (i, w))
rip, = struct.unpack_from('<Q', b, ctx_off + 0xf8)
print('context rip', where(rip) or hex(rip))
