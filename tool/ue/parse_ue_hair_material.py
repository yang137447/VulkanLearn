#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Parse UE4.26 .uasset files to extract material texture connections.

Focused purpose: determine definitively whether fiber_normal (and other
textures) are connected to the material's Normal/BaseColor/Roughness/etc
outputs, and what the material instances actually override.

Layout reference: CUE4Parse FPackageFileSummary (UE4.26, FileVersionUE4=522).
"""
import struct
import sys
import os

# ---------------------------------------------------------------------------
# Binary reader
# ---------------------------------------------------------------------------
class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def tell(self):
        return self.pos

    def seek(self, off):
        self.pos = off

    def skip(self, n):
        self.pos += n

    def read(self, n):
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def i32(self):
        v = struct.unpack_from('<i', self.data, self.pos)[0]
        self.pos += 4
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.data, self.pos)[0]
        self.pos += 4
        return v

    def i64(self):
        v = struct.unpack_from('<q', self.data, self.pos)[0]
        self.pos += 8
        return v

    def fstring(self):
        """UE4 FString: int32 length (neg=UTF-16, pos=ANSI). Length includes null."""
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:
            raw = self.read(-n * 2).decode('utf-16-le', errors='replace')
            return raw.rstrip('\x00')
        raw = self.read(n)
        return raw.rstrip(b'\x00').decode('ascii', errors='replace')

    def guid(self):
        return self.read(16)


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
def parse_summary(r):
    s = {}
    s['tag'] = r.u32()
    legacy = r.i32()
    s['legacyFileVersion'] = legacy
    if legacy < 0:
        if legacy != -4:
            s['FileVersionUE3'] = r.i32()
        s['FileVersionUE4'] = r.i32()
        if legacy <= -8:
            s['FileVersionUE5'] = r.i32()
        s['FileVersionLicenseeUE'] = r.i32()
        # custom version container (guid-based)
        n = r.i32()
        custom = []
        for _ in range(n):
            g = r.guid()
            v = r.i32()
            custom.append((g.hex(), v))
        s['customVersions'] = custom
    # TotalHeaderSize
    s['TotalHeaderSize'] = r.i32()
    s['PackageName'] = r.fstring()
    s['PackageFlags'] = r.u32()
    s['NameCount'] = r.i32()
    s['NameOffset'] = r.i32()
    # LocalizationId (if version >= ADDED_PACKAGE_SUMMARY_LOCALIZATION_ID, not PKG_FilterEditorOnly)
    # 522 >= 513, and PackageFlags likely not FilterEditorOnly -> read it
    s['LocalizationId'] = r.fstring()
    # GatherableTextData (version >= SERIALIZE_TEXT_IN_PACKAGES = 459)
    s['GatherableTextDataCount'] = r.i32()
    s['GatherableTextDataOffset'] = r.i32()
    s['ExportCount'] = r.i32()
    s['ExportOffset'] = r.i32()
    s['ImportCount'] = r.i32()
    s['ImportOffset'] = r.i32()
    s['DependsOffset'] = r.i32()
    return s


# ---------------------------------------------------------------------------
# Name table
# ---------------------------------------------------------------------------
def parse_name_table(r, count, offset):
    r.seek(offset)
    names = []
    for _ in range(count):
        # UE4.26 FNameEntrySerialized: int32 length (incl null) + bytes + uint32 hash
        n = r.i32()
        raw = r.read(n)
        name = raw.rstrip(b'\x00').decode('ascii', errors='replace')
        r.skip(4)  # uint32 hash
        names.append(name)
    return names


# ---------------------------------------------------------------------------
# Import / Export tables
# ---------------------------------------------------------------------------
def read_fname(r):
    idx = r.i32()
    num = r.i32()
    return idx, num


def parse_import_table(r, count, offset, names):
    """FObjectImport (UE4.26): ClassPackage, ClassName, OuterIndex, ObjectName, PackageName."""
    r.seek(offset)
    imports = []
    for _ in range(count):
        class_pkg, _ = read_fname(r)
        class_name, _ = read_fname(r)
        outer = r.i32()
        obj_name, _ = read_fname(r)
        pkg_name, _ = read_fname(r)  # VER_UE4_NON_OUTER_PACKAGE_IMPORT
        imports.append({
            'ClassPackage': names[class_pkg] if 0 <= class_pkg < len(names) else str(class_pkg),
            'ClassName': names[class_name] if 0 <= class_name < len(names) else str(class_name),
            'OuterIndex': outer,
            'ObjectName': names[obj_name] if 0 <= obj_name < len(names) else str(obj_name),
            'PackageName': names[pkg_name] if 0 <= pkg_name < len(names) else str(pkg_name),
        })
    return imports


def parse_export_table(r, count, offset, names):
    """FObjectExport (UE4.26): fixed fields + FirstExportDependency + 4 dependency TArrays."""
    r.seek(offset)
    exports = []
    for _ in range(count):
        class_idx = r.i32()
        super_idx = r.i32()
        template_idx = r.i32()
        outer_idx = r.i32()
        obj_name, _ = read_fname(r)
        obj_flags = r.u32()
        serial_size = r.i64()
        serial_offset = r.i64()
        forced_export = r.i32()
        not_for_client = r.i32()
        not_for_server = r.i32()
        package_guid = r.guid()
        package_flags = r.u32()
        not_always_loaded = r.i32()
        is_asset = r.i32()
        # dependency fields (VER_UE4_FirstExportDependency)
        first_dep = r.i32()
        for _ in range(4):
            n = r.i32()
            r.skip(4 * n)
        exports.append({
            'ClassIndex': class_idx,
            'SuperIndex': super_idx,
            'TemplateIndex': template_idx,
            'OuterIndex': outer_idx,
            'ObjectName': names[obj_name] if 0 <= obj_name < len(names) else str(obj_name),
            'ObjectFlags': obj_flags,
            'SerialSize': serial_size,
            'SerialOffset': serial_offset,
        })
    return exports


def main(path):
    data = open(path, 'rb').read()
    r = Reader(data)
    s = parse_summary(r)
    print('== summary ==')
    for k, v in s.items():
        if k == 'customVersions':
            print(f'  customVersions: {len(v)} entries (first versions: {[c[1] for c in v[:8]]})')
        else:
            print(f'  {k} = {v}')

    names = parse_name_table(r, s['NameCount'], s['NameOffset'])
    print(f'== names ({len(names)}) ==')
    for i, n in enumerate(names):
        if any(k in n for k in ('fiber', 'Normal', 'BaseColor', 'Roughness', 'Metallic',
                                 'Anisotropy', 'Tangent', 'MaterialExpression', 'TextureSample',
                                 'OpacityMask', 'Emissive')):
            print(f'  [{i}] {n}')

    imports = parse_import_table(r, s['ImportCount'], s['ImportOffset'], names)
    print(f'== imports ({len(imports)}) ==')
    for i, imp in enumerate(imports):
        print(f'  [{i}] {imp["ClassName"]} {imp["ObjectName"]} (outer={imp["OuterIndex"]})')

    exports = parse_export_table(r, s['ExportCount'], s['ExportOffset'], names)
    print(f'== exports ({len(exports)}) ==')
    for i, e in enumerate(exports):
        print(f'  [{i}] class={e["ClassIndex"]} name={e["ObjectName"]} '
              f'size={e["SerialSize"]} off={e["SerialOffset"]}')

    return s, names, imports, exports


if __name__ == '__main__':
    main(sys.argv[1])
