# find_unity_sites.py -- locate the libunity.so (Unity 2022.3, arm64) sites the
# loader needs: UnityPlayer native methods (RegisterNatives tables) and the
# Application path icalls (icall registration pairs). Usage:
#   python3 find_unity_sites.py libunity.so
import struct, sys
from elftools.elf.elffile import ELFFile
f = open(sys.argv[1], 'rb'); data = f.read(); elf = ELFFile(open(sys.argv[1], 'rb'))
segs = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
def v2o(va):
    for a, o, n in segs:
        if a <= va < a + n: return o + va - a
def o2v(off):
    for a, o, n in segs:
        if o <= off < o + n: return a + off - o
rel = {}
for s in elf.iter_sections():
    if s.name in ('.rela.dyn',):
        for r in s.iter_relocations():
            if r['r_info_type'] == 1027: rel[r['r_offset']] = r['r_addend']
byaddend = {}
for k, v in rel.items(): byaddend.setdefault(v, []).append(k)
def strva(s):
    b = s.encode() + b'\0'; r = []; p = data.find(b)
    while p != -1:
        if p == 0 or data[p-1] == 0: r.append(o2v(p))
        p = data.find(b, p + 1)
    return r
def natives(name):
    out = []
    for sva in strva(name):
        for e in byaddend.get(sva, []):
            fn = rel.get(e + 16); sig = rel.get(e + 8)
            if fn:
                so = v2o(sig); sg = data[so:data.index(b'\0', so)].decode() if so else '?'
                out.append((e, sg, fn))
    return out
for n in ['initJni', 'nativeRecreateGfxState', 'nativeSendSurfaceChangedEvent', 'nativeRender',
          'nativeInjectEvent', 'nativeResume', 'nativeFocusChanged', 'nativeDone',
          'nativeApplicationUnload', 'nativeIsAutorotationOn', 'nativePause']:
    for e, sg, fn in natives(n): print('%-32s %-60s 0x%x' % (n, sg, fn))
for s in elf.get_section_by_name('.dynsym').iter_symbols():
    if s.name == 'JNI_OnLoad': print('%-32s %-60s 0x%x' % ('JNI_OnLoad', '', s['st_value']))
for n in ['UnityEngine.Application::get_persistentDataPath', 'UnityEngine.Application::get_streamingAssetsPath',
          'UnityEngine.Application::get_dataPath', 'UnityEngine.Application::get_temporaryCachePath']:
    for sva in strva(n):
        for e in byaddend.get(sva, []):
            print('%-50s entry 0x%x -> fn 0x%x / 0x%x' % (n, e, rel.get(e + 8, 0), rel.get(e - 8, 0)))
