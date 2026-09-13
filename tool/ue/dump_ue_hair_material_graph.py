#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dump the UE4.26 hair material graph: resolve which textures feed the
Material's Normal/BaseColor/Metallic/Roughness/etc inputs.

FPackageIndex convention observed in this asset: negative = import (|-N|-1),
positive = export (N-1), 0 = null.
"""
import struct
import sys

class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0
    def tell(self): return self.pos
    def seek(self, o): self.pos = o
    def skip(self, n): self.pos += n
    def read(self, n):
        b = self.data[self.pos:self.pos+n]; self.pos += n; return b
    def i32(self):
        v = struct.unpack_from('<i', self.data, self.pos)[0]; self.pos += 4; return v
    def u32(self):
        v = struct.unpack_from('<I', self.data, self.pos)[0]; self.pos += 4; return v
    def i64(self):
        v = struct.unpack_from('<q', self.data, self.pos)[0]; self.pos += 8; return v
    def fstring(self):
        n = self.i32()
        if n == 0: return ""
        if n < 0: return self.read(-n*2).decode('utf-16-le','replace').rstrip('\x00')
        return self.read(n).rstrip(b'\x00').decode('ascii','replace')
    def guid(self): return self.read(16)

def read_fname(r):
    idx = r.i32(); num = r.i32(); return idx, num

def parse_summary(r):
    s = {}
    s['tag'] = r.u32()
    legacy = r.i32()
    s['legacy'] = legacy
    if legacy < 0:
        if legacy != -4: s['ue3'] = r.i32()
        s['ue4'] = r.i32()
        if legacy <= -8: s['ue5'] = r.i32()
        s['licensee'] = r.i32()
        n = r.i32()
        for _ in range(n):
            r.guid(); r.i32()
    s['TotalHeaderSize'] = r.i32()
    s['PackageName'] = r.fstring()
    s['PackageFlags'] = r.u32()
    s['NameCount'] = r.i32()
    s['NameOffset'] = r.i32()
    s['LocalizationId'] = r.fstring()
    s['GatherableTextDataCount'] = r.i32()
    s['GatherableTextDataOffset'] = r.i32()
    s['ExportCount'] = r.i32()
    s['ExportOffset'] = r.i32()
    s['ImportCount'] = r.i32()
    s['ImportOffset'] = r.i32()
    s['DependsOffset'] = r.i32()
    return s

def parse_name_table(r, count, offset):
    r.seek(offset)
    names = []
    for _ in range(count):
        n = r.i32()
        raw = r.read(n)
        names.append(raw.rstrip(b'\x00').decode('ascii','replace'))
        r.skip(4)
    return names

def parse_import_table(r, count, offset, names):
    r.seek(offset)
    imports = []
    for _ in range(count):
        cp,_ = read_fname(r); cn,_ = read_fname(r)
        outer = r.i32()
        on,_ = read_fname(r); pn,_ = read_fname(r)
        imports.append((names[cp] if 0<=cp<len(names) else str(cp),
                        names[cn] if 0<=cn<len(names) else str(cn),
                        outer,
                        names[on] if 0<=on<len(names) else str(on)))
    return imports

def parse_export_table(r, count, offset, names):
    r.seek(offset)
    exports = []
    for _ in range(count):
        class_idx = r.i32(); super_idx = r.i32(); template_idx = r.i32(); outer_idx = r.i32()
        on,_ = read_fname(r)
        flags = r.u32(); size = r.i64(); off = r.i64()
        r.i32(); r.i32(); r.i32()
        r.guid(); r.u32(); r.i32(); r.i32()
        r.i32()
        for _ in range(4):
            n = r.i32(); r.skip(4*n)
        exports.append(dict(class_idx=class_idx, name=names[on] if 0<=on<len(names) else str(on),
                            size=size, off=off))
    return exports

def resolve_idx(idx, imports, exports):
    if idx == 0: return "(null)"
    if idx < 0: return "import[" + str(-idx-1) + "]=" + imports[-idx-1][3]
    return "export[" + str(idx-1) + "]=" + exports[idx-1]['name']

def parse_properties(r, end, names):
    """Parse FProperty tags. Returns list of (prop_name, type_name, size, raw, struct_guid)."""
    props = []
    while r.tell() < end:
        name_idx, _ = read_fname(r)
        if name_idx == 0 or name_idx >= len(names):
            break
        prop_name = names[name_idx]
        type_idx, _ = read_fname(r)
        type_name = names[type_idx] if 0 <= type_idx < len(names) else str(type_idx)
        size = r.i32()
        arr_idx = r.i32()
        is_bool = (type_name == 'BoolProperty')
        if is_bool:
            raw = r.read(1)
            r.skip(max(0, size - 1)) if size > 1 else None
        else:
            raw = r.read(size) if size > 0 else b''
        struct_guid = None
        if type_name in ('StructProperty',):
            struct_guid = r.read(16)  # struct type guid after value
        props.append((prop_name, type_name, size, raw, struct_guid))
    return props

def guid_str(g):
    return g[::-1].hex().upper() if g else None

def main(path):
    data = open(path, 'rb').read()
    r = Reader(data)
    s = parse_summary(r)
    names = parse_name_table(r, s['NameCount'], s['NameOffset'])
    imports = parse_import_table(r, s['ImportCount'], s['ImportOffset'], names)
    exports = parse_export_table(r, s['ExportCount'], s['ExportOffset'], names)

    print("== TEXTURE imports ==")
    tex_imports = {}
    for i, imp in enumerate(imports):
        if imp[3].startswith('/Game/') or imp[1] == 'Texture2D':
            tex_imports[i] = imp[3]
            print(f"  import[{i}] = {imp[3]}")

    # find the material export and texture sample exports
    mat_export = None
    texsample_exports = []
    for i, e in enumerate(exports):
        if e['class_idx'] < 0:
            cls = imports[-e['class_idx']-1][3] if -e['class_idx']-1 < len(imports) else '?'
        else:
            cls = '?'
        if cls == 'Material' or e['name'] == 'MA_HairStyle':
            mat_export = (i, e)
        if cls in ('MaterialExpressionTextureSample', 'MaterialExpressionTextureSampleParameter2D'):
            texsample_exports.append((i, e, cls))

    print(f"\n== MATERIAL export = {mat_export} ==")
    if mat_export:
        i, e = mat_export
        r.seek(e['off'])
        props = parse_properties(r, e['off'] + e['size'], names)
        print(f"  material '{e['name']}' has {len(props)} properties:")
        for pn, tn, size, raw, sg in props:
            if pn in ('BaseColor','Normal','Metallic','Roughness','Anisotropy','Tangent',
                      'EmissiveColor','OpacityMask','Opacity','Refraction','PixelDepthOffset',
                      'ShadingModel','BlendMode','TwoSided','MaterialDomain','bUseMaterialAttributes',
                      'DitherOpacityMask','SubsurfaceProfile','WorldPositionOffset'):
                print(f"    [{pn}] type={tn} size={size} raw={raw[:40].hex()}")
                # FExpressionInput: Expression(int32) OutputIndex(int32) InputName(FName) Mask...
                if tn == 'StructProperty' and size >= 36:
                    expr = struct.unpack_from('<i', raw, 0)[0]
                    outidx = struct.unpack_from('<i', raw, 4)[0]
                    print(f"        Expression={expr} -> {resolve_idx(expr, imports, exports)}  OutputIndex={outidx}")

    print(f"\n== TEXTURE SAMPLE exports ({len(texsample_exports)}) ==")
    for i, e, cls in texsample_exports:
        r.seek(e['off'])
        props = parse_properties(r, e['off'] + e['size'], names)
        tex_ref = None
        guid = None
        param_name = None
        sampler_type = None
        for pn, tn, size, raw, sg in props:
            if pn == 'Texture':
                tex_ref = struct.unpack_from('<i', raw, 0)[0] if size >= 4 else None
            if pn == 'MaterialExpressionGuid':
                guid = guid_str(raw) if size == 16 else raw.hex()
            if pn == 'ParameterName':
                param_name = names[struct.unpack_from('<i', raw, 0)[0]] if size >= 4 else None
            if pn == 'SamplerType':
                sampler_type = raw.hex()
        print(f"  export[{i}] cls={cls} name={e['name']}")
        if tex_ref is not None:
            print(f"      Texture={tex_ref} -> {resolve_idx(tex_ref, imports, exports)}")
        if guid: print(f"      Guid={guid}")
        if param_name: print(f"      ParameterName={param_name}")
        if sampler_type: print(f"      SamplerType={sampler_type}")

if __name__ == '__main__':
    main(sys.argv[1])
