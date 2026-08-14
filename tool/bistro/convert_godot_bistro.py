from __future__ import annotations
import argparse
import json
import math
import re
import struct
from pathlib import Path
from typing import Any

JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
FLOAT32 = 5126

def norm(value: str) -> str:
    value = value.lower().replace('t_bistro_', '')
    value = re.sub(r'_(basecolor|normal|specular|emissive|roughness|metallic|ao|dds|png|tga)$', '', value)
    value = value.replace('_blendshader', '')
    return re.sub(r'[^a-z0-9]+', '', value)

def canon(value: str) -> str:
    return re.sub(r'[^A-Za-z0-9_]+', '_', value).strip('_')

def mat_identity():
    return [[1.0,0.0,0.0,0.0],[0.0,1.0,0.0,0.0],[0.0,0.0,1.0,0.0],[0.0,0.0,0.0,1.0]]

def mat_mul(a,b):
    return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]

def mat_inverse_affine(m):
    a=[[m[r][c] for c in range(3)] for r in range(3)]
    det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0])
    if abs(det)<1e-8: raise RuntimeError('Singular authored transform')
    inv=[[0.0]*4 for _ in range(4)]; inv[3][3]=1.0
    inv[0][0]=(a[1][1]*a[2][2]-a[1][2]*a[2][1])/det; inv[0][1]=(a[0][2]*a[2][1]-a[0][1]*a[2][2])/det; inv[0][2]=(a[0][1]*a[1][2]-a[0][2]*a[1][1])/det
    inv[1][0]=(a[1][2]*a[2][0]-a[1][0]*a[2][2])/det; inv[1][1]=(a[0][0]*a[2][2]-a[0][2]*a[2][0])/det; inv[1][2]=(a[0][2]*a[1][0]-a[0][0]*a[1][2])/det
    inv[2][0]=(a[1][0]*a[2][1]-a[1][1]*a[2][0])/det; inv[2][1]=(a[0][1]*a[2][0]-a[0][0]*a[2][1])/det; inv[2][2]=(a[0][0]*a[1][1]-a[0][1]*a[1][0])/det
    t=[m[0][3],m[1][3],m[2][3]]
    for r in range(3): inv[r][3]=-sum(inv[r][c]*t[c] for c in range(3))
    return inv
def quat_mat(q):
    x,y,z,w=q
    return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),0.0],
            [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),0.0],
            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),0.0],[0.0,0.0,0.0,1.0]]

def node_mat(node):
    if 'matrix' in node:
        values=node['matrix']
        return [[float(values[c*4+r]) for c in range(4)] for r in range(4)]
    m=quat_mat(node.get('rotation',[0,0,0,1]))
    t=node.get('translation',[0,0,0]); s=node.get('scale',[1,1,1])
    for r in range(3):
        m[r][0]*=s[0]; m[r][1]*=s[1]; m[r][2]*=s[2]
        m[r][3]=float(t[r])
    return m

def point(m,v):
    return [m[0][0]*v[0]+m[0][1]*v[1]+m[0][2]*v[2]+m[0][3],
            m[1][0]*v[0]+m[1][1]*v[1]+m[1][2]*v[2]+m[1][3],
            m[2][0]*v[0]+m[2][1]*v[1]+m[2][2]*v[2]+m[2][3]]

def direction(m,v):
    a=[[m[0][0],m[0][1],m[0][2]],[m[1][0],m[1][1],m[1][2]],[m[2][0],m[2][1],m[2][2]]]
    det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0])
    if abs(det)<1e-8: out=[sum(a[r][c]*v[c] for c in range(3)) for r in range(3)]
    else:
        inv=[[0.0]*3 for _ in range(3)]
        inv[0][0]=(a[1][1]*a[2][2]-a[1][2]*a[2][1])/det; inv[0][1]=(a[0][2]*a[2][1]-a[0][1]*a[2][2])/det; inv[0][2]=(a[0][1]*a[1][2]-a[0][2]*a[1][1])/det
        inv[1][0]=(a[1][2]*a[2][0]-a[1][0]*a[2][2])/det; inv[1][1]=(a[0][0]*a[2][2]-a[0][2]*a[2][0])/det; inv[1][2]=(a[0][2]*a[1][0]-a[0][0]*a[1][2])/det
        inv[2][0]=(a[1][0]*a[2][1]-a[1][1]*a[2][0])/det; inv[2][1]=(a[0][1]*a[2][0]-a[0][0]*a[2][1])/det; inv[2][2]=(a[0][0]*a[1][1]-a[0][1]*a[1][0])/det
        out=[sum(inv[c][r]*v[c] for c in range(3)) for r in range(3)]
    length=math.sqrt(sum(x*x for x in out))
    return [x/length for x in out] if length>1e-8 else [0.0,0.0,0.0]

def read_glb(path):
    data=path.read_bytes(); magic,version,length=struct.unpack_from('<4sII',data,0)
    if magic!=b'glTF' or version!=2 or length!=len(data): raise RuntimeError(f'Invalid GLB {path}')
    off=12; doc=None; binary=None
    while off<len(data):
        n,t=struct.unpack_from('<II',data,off); chunk=data[off+8:off+8+n]
        if t==JSON_CHUNK: doc=json.loads(chunk.decode('utf-8'))
        elif t==BIN_CHUNK: binary=bytearray(chunk)
        off+=8+n
    if doc is None or binary is None: raise RuntimeError(f'Missing GLB chunks {path}')
    return doc,binary

def write_glb(path,doc,binary):
    raw=json.dumps(doc,separators=(',',':'),ensure_ascii=True).encode('utf-8'); raw += b' ' * ((4-len(raw)%4)%4)
    blob=bytes(binary); blob += b'\0' * ((4-len(blob)%4)%4)
    with path.open('wb') as f:
        f.write(struct.pack('<4sII',b'glTF',2,12+8+len(raw)+8+len(blob)))
        f.write(struct.pack('<II',len(raw),JSON_CHUNK)); f.write(raw)
        f.write(struct.pack('<II',len(blob),BIN_CHUNK)); f.write(blob)

def accessor_info(doc,idx):
    a=doc['accessors'][idx]; view=doc['bufferViews'][a['bufferView']]
    if a['componentType']!=FLOAT32 or a['type'] not in ('VEC3','VEC4'): raise RuntimeError(f'Unsupported accessor {idx}')
    comps=3 if a['type']=='VEC3' else 4
    base=int(view.get('byteOffset',0))+int(a.get('byteOffset',0)); stride=int(view.get('byteStride',comps*4))
    return base,stride,comps,int(a['count'])

def patch_accessor(doc,binary,idx,matrix,is_normal):
    base,stride,comps,count=accessor_info(doc,idx); lo=[float('inf')]*comps; hi=[float('-inf')]*comps
    for i in range(count):
        off=base+i*stride; values=list(struct.unpack_from('<'+'f'*comps,binary,off))
        if is_normal:
            # Assimp keeps glTF CCW front faces. VulkanLearn's pipeline uses
            # clockwise front faces after Vulkan's Y-axis NDC flip, so source
            # normals/tangents are kept in their authored direction.
            values[:3]=direction(matrix,values[:3])
        else:
            values[:3]=point(matrix,values[:3])
        struct.pack_into('<'+'f'*comps,binary,off,*values)
        lo=[min(a,b) for a,b in zip(lo,values)]; hi=[max(a,b) for a,b in zip(hi,values)]
    doc['accessors'][idx]['min']=lo; doc['accessors'][idx]['max']=hi

def parse_tscn(path):
    text=path.read_text(encoding='utf-8',errors='ignore'); ext={}
    for match in re.finditer(r'\[ext_resource[^\]]+type="Material"[^\]]+path="res://Materials/([^\"]+\.tres)"[^\]]+id="([^"]+)"',text): ext[match.group(2)]=Path(match.group(1)).stem
    overrides={}
    for section in re.split(r'(?=^\[node )',text,flags=re.MULTILINE):
        header=re.search(r'^\[node name="([^"]+)"[^\]]*\]',section,flags=re.MULTILINE); override=re.search(r'surface_material_override/0\s*=\s*ExtResource\("([^"]+)"\)',section)
        if header and override and override.group(1) in ext: overrides[canon(header.group(1))]=ext[override.group(1)]
    return text,overrides

def godot_transform_mat(values):
    return [[values[0],values[1],values[2],values[9]],
            [values[3],values[4],values[5],values[10]],
            [values[6],values[7],values[8],values[11]],
            [0.0,0.0,0.0,1.0]]

def parse_tscn_nodes(path):
    text=path.read_text(encoding='utf-8',errors='ignore'); resources={}
    for match in re.finditer(r'^\[ext_resource\s+([^\]]+)\]',text,flags=re.MULTILINE):
        attrs=match.group(1)
        resource_id=re.search(r'(?:^|\s)id="([^"]+)"',attrs); resource_path=re.search(r'path="res://([^"]+)"',attrs); resource_type=re.search(r'type="([^"]+)"',attrs)
        if resource_id and resource_path: resources[resource_id.group(1)]={'path':resource_path.group(1),'type':resource_type.group(1) if resource_type else ''}
    nodes={}
    for section in re.split(r'(?=^\[node )',text,flags=re.MULTILINE):
        header=re.search(r'^\[node\s+([^\]]+)\]',section,flags=re.MULTILINE)
        if not header: continue
        attrs=header.group(1); name_match=re.search(r'name="([^"]+)"',attrs)
        if not name_match: continue
        parent_match=re.search(r'parent="([^"]+)"',attrs); parent=parent_match.group(1) if parent_match else None
        if parent is None: node_path=()
        else:
            prefix=[] if parent=='.' else [canon(x) for x in parent.split('/')]
            node_path=tuple(prefix+[canon(name_match.group(1))])
        entry={}
        transform=re.search(r'^transform\s*=\s*Transform3D\(([^)]+)\)',section,flags=re.MULTILINE)
        if transform: entry['transform']=godot_transform_mat([float(x.strip()) for x in transform.group(1).split(',')])
        visible=re.search(r'^visible\s*=\s*(true|false)',section,flags=re.MULTILINE)
        if visible: entry['visible']=visible.group(1)=='true'
        material=re.search(r'^surface_material_override/0\s*=\s*ExtResource\("([^"]+)"\)',section,flags=re.MULTILINE)
        if material and material.group(1) in resources: entry['material']=Path(resources[material.group(1)]['path']).stem
        if entry: nodes.setdefault(node_path,{}).update(entry)
    return resources,nodes

def merge_node_overrides(*groups):
    result={}
    for group in groups:
        for path,values in group.items(): result.setdefault(path,{}).update(values)
    return result

def glb_node_paths(nodes):
    parents={child:i for i,node in enumerate(nodes) for child in node.get('children',[])}; paths={}
    for i,node in enumerate(nodes):
        parts=[canon(node.get('name',''))]; current=i
        while current in parents:
            current=parents[current]; parts.append(canon(nodes[current].get('name','')))
        paths[i]=tuple(reversed(parts))
    return paths,parents

def find_node_override(overrides,path,node_name,key,allow_leaf_match=False):
    exact=overrides.get(path,{})
    if key in exact: return exact[key]
    if not allow_leaf_match: return None
    matches=[values[key] for candidate,values in overrides.items() if candidate and candidate[-1]==canon(node_name) and key in values]
    return matches[0] if matches and all(value==matches[0] for value in matches[1:]) else None
def material_mapping(resource_root,stage_root):
    mi_by_slot={}; mi_tex={}
    for p in (resource_root/'materials/bistro').glob('MI_*.json'):
        data=json.loads(p.read_text(encoding='utf-8')); slot=data['name'].removeprefix('Bistro '); mi_by_slot[slot]=p
        for value in data.get('textures',{}).values(): mi_tex.setdefault(norm(Path(value).stem),set()).add(slot)
    result={}
    for p in stage_root.glob('Materials/**/*.tres'):
        text=p.read_text(encoding='utf-8',errors='ignore'); tex=[Path(x).stem for x in re.findall(r'path="res://Textures/[^/\"]+/([^\"]+)"',text)]
        scored=[]
        for slot in mi_by_slot:
            score=0; a=norm(p.stem); b=norm(slot)
            if a==b or a in b or b.endswith(a): score+=100
            for t in tex:
                k=norm(t)
                if k==b: score+=30
                elif k in b or b in k: score+=5
            if score: scored.append((score,slot))
        if not scored:
            for t in tex:
                for slot in mi_tex.get(norm(t),[]): scored.append((10,slot))
        if scored: result[p.stem]=sorted(scored,key=lambda x:(-x[0],x[1]))[0][1]
    return result,mi_by_slot

def choose_wrapper(glb,wrappers,doc):
    names={canon(n.get('name','')) for n in doc.get('nodes',[]) if 'mesh' in n}; candidates=[]
    for wrapper in wrappers:
        text,overrides=parse_tscn(wrapper)
        direct=f"res://Meshes/{glb.relative_to(glb.parents[2]).as_posix()}" if len(glb.parents)>=3 else ''
        score=sum(1 for n in names if n in overrides)
        if glb.stem.lower()==wrapper.stem.lower(): score += 1000
        candidates.append((score, len(overrides), wrapper, overrides))
    candidates.sort(key=lambda x:(-x[0],-x[1],str(x[2])))
    if not candidates: raise RuntimeError(f'No material wrapper for {glb}')
    score,_,wrapper,overrides=candidates[0]
    covered=sum(1 for n in names if n in overrides)
    missing=names-set(overrides)
    non_blocker=[name for name in missing if 'blocker' not in name.lower()]
    if non_blocker:
        raise RuntimeError(f'Incomplete material wrapper for {glb}: {covered}/{len(names)}; selected {wrapper}; missing={sorted(non_blocker)[:4]}')
    return wrapper,overrides

def bake_glb(source,output,wrappers,material_map,runtime_materials):
    doc,binary=read_glb(source); wrapper,overrides=choose_wrapper(source,wrappers,doc); nodes=doc.get('nodes',[]); meshes=doc.get('meshes',[]); worlds={}
    def visit(i,parent):
        world=mat_mul(parent,node_mat(nodes[i])); worlds[i]=world
        for child in nodes[i].get('children',[]): visit(child,world)
    roots=doc.get('scenes',[{}])[doc.get('scene',0)].get('nodes',[])
    for root in roots: visit(root,mat_identity())
    mesh_slots={}
    for i,node in enumerate(nodes):
        if 'mesh' not in node: continue
        mi=int(node['mesh']); node_name=canon(node.get('name','')); material_file=overrides.get(node_name)
        if material_file is None and 'blocker' in node_name.lower():
            node.pop('mesh',None); continue
        if material_file is None: raise RuntimeError(f'No material override for {source}:{node.get("name")}')
        base_slot=material_map.get(material_file)
        if base_slot is None or base_slot not in runtime_materials: raise RuntimeError(f'No runtime material mapping for {source}:{node.get("name")}')
        mesh_slots[mi]=base_slot; matrix=worlds[i]
        for primitive in meshes[mi].get('primitives',[]):
            attrs=primitive.get('attributes',{})
            if 'POSITION' in attrs: patch_accessor(doc,binary,int(attrs['POSITION']),matrix,False)
            if 'NORMAL' in attrs: patch_accessor(doc,binary,int(attrs['NORMAL']),matrix,True)
            if 'TANGENT' in attrs: patch_accessor(doc,binary,int(attrs['TANGENT']),matrix,True)
    slots=[]
    for mi,mesh in enumerate(meshes):
        if mi not in mesh_slots: continue
        slot=mesh_slots[mi]
        if slot not in slots: slots.append(slot)
        for primitive in mesh.get('primitives',[]): primitive['material']=slots.index(slot)
    doc['materials']=[{'name':x} for x in slots]
    for node in nodes:
        for key in ('matrix','translation','rotation','scale'): node.pop(key,None)
    output.parent.mkdir(parents=True,exist_ok=True); write_glb(output,doc,binary)
    return slots

def bake_variant_glb(source,output,base_wrapper,variant_wrapper,material_map,runtime_materials,rebase_root=None):
    doc,binary=read_glb(source); nodes=doc.get('nodes',[]); meshes=doc.get('meshes',[])
    _,base_nodes=parse_tscn_nodes(base_wrapper); _,variant_nodes=parse_tscn_nodes(variant_wrapper)
    overrides=merge_node_overrides(base_nodes,variant_nodes); paths,_=glb_node_paths(nodes); worlds={}; visibility={}
    def visit(i,parent,parent_visible):
        node=nodes[i]; path=paths[i]; local=node_mat(node) if rebase_root else (find_node_override(overrides,path,node.get('name',''),'transform') or node_mat(node))
        visible=find_node_override(overrides,path,node.get('name',''),'visible')
        visibility[i]=parent_visible and visible is not False; worlds[i]=mat_mul(parent,local)
        for child in node.get('children',[]): visit(child,worlds[i],visibility[i])
    roots=doc.get('scenes',[{}])[doc.get('scene',0)].get('nodes',[])
    for root in roots: visit(root,mat_identity(),True)
    rebase_delta=None
    if rebase_root:
        matches=[i for i,path in paths.items() if path==tuple(canon(x) for x in rebase_root)]
        if len(matches)!=1: raise RuntimeError(f'Variant rebase root mismatch for {source}: {rebase_root}')
        target=find_node_override(overrides,paths[matches[0]],nodes[matches[0]].get('name',''),'transform')
        if target is None: raise RuntimeError(f'Variant rebase transform missing for {variant_wrapper}')
        rebase_delta=mat_mul(target,mat_inverse_affine(worlds[matches[0]]))
    mesh_slots={}; selected=0
    for i,node in enumerate(nodes):
        if 'mesh' not in node: continue
        if not visibility.get(i,True): node.pop('mesh',None); continue
        mi=int(node['mesh']); path=paths[i]; material_file=find_node_override(overrides,path,node.get('name',''),'material',True)
        if material_file is None and 'blocker' in canon(node.get('name','')).lower(): node.pop('mesh',None); continue
        if material_file is None: raise RuntimeError(f'No variant material override for {source}:{"/".join(path)}')
        base_slot=material_map.get(material_file)
        if base_slot is None or base_slot not in runtime_materials: raise RuntimeError(f'No runtime material mapping for {source}:{material_file}')
        mesh_slots[mi]=base_slot; matrix=mat_mul(rebase_delta,worlds[i]) if rebase_delta else worlds[i]; selected+=1
        for primitive in meshes[mi].get('primitives',[]):
            attrs=primitive.get('attributes',{})
            if 'POSITION' in attrs: patch_accessor(doc,binary,int(attrs['POSITION']),matrix,False)
            if 'NORMAL' in attrs: patch_accessor(doc,binary,int(attrs['NORMAL']),matrix,True)
            if 'TANGENT' in attrs: patch_accessor(doc,binary,int(attrs['TANGENT']),matrix,True)
    if not selected: raise RuntimeError(f'Variant selected no render meshes: {variant_wrapper}')
    slots=[]
    for mi,mesh in enumerate(meshes):
        if mi not in mesh_slots: continue
        slot=mesh_slots[mi]
        if slot not in slots: slots.append(slot)
        for primitive in mesh.get('primitives',[]): primitive['material']=slots.index(slot)
    doc['materials']=[{'name':x} for x in slots]
    for node in nodes:
        for key in ('matrix','translation','rotation','scale'): node.pop(key,None)
    output.parent.mkdir(parents=True,exist_ok=True); write_glb(output,doc,binary)
    return slots,selected

def parse_main_instances(main_scene):
    text=main_scene.read_text(encoding='utf-8',errors='ignore'); resources={}
    for match in re.finditer(r'^\[ext_resource\s+([^\]]+)\]',text,flags=re.MULTILINE):
        attrs=match.group(1); resource_type=re.search(r'type="([^"]+)"',attrs); resource_path=re.search(r'path="res://([^"]+)"',attrs); resource_id=re.search(r'(?:^|\s)id="([^"]+)"',attrs)
        if resource_type and resource_type.group(1)=='PackedScene' and resource_path and resource_id: resources[resource_id.group(1)]=resource_path.group(1)
    nodes=[]; worlds={}
    for section in re.split(r'(?=^\[node )',text,flags=re.MULTILINE):
        header=re.search(r'^\[node\s+([^\]]+)\]',section,flags=re.MULTILINE)
        if not header: continue
        attrs=header.group(1); name_match=re.search(r'name="([^"]+)"',attrs)
        if not name_match: continue
        parent_match=re.search(r'parent="([^"]+)"',attrs); parent_text=parent_match.group(1) if parent_match else None
        parent_path=() if parent_text in (None,'.') else tuple(canon(x) for x in parent_text.split('/'))
        node_path=() if parent_text is None else tuple(list(parent_path)+[canon(name_match.group(1))])
        transform=re.search(r'^transform\s*=\s*Transform3D\(([^)]+)\)',section,flags=re.MULTILINE)
        local=godot_transform_mat([float(x.strip()) for x in transform.group(1).split(',')]) if transform else mat_identity()
        world=mat_mul(worlds.get(parent_path,mat_identity()),local); worlds[node_path]=world
        instance=re.search(r'instance=ExtResource\("([^"]+)"\)',attrs)
        nodes.append({'name':name_match.group(1),'path':node_path,'parent':parent_path,'world':world,'scenePath':resources.get(instance.group(1)) if instance else None})
    return nodes
def clean_transform_value(value):
    if abs(value)<1e-5: return 0.0
    if abs(value-1.0)<1e-5: return 1.0
    if abs(value+1.0)<1e-5: return -1.0
    return value

def decompose_matrix(m):
    sx=math.sqrt(sum(m[r][0]*m[r][0] for r in range(3))); sy=math.sqrt(sum(m[r][1]*m[r][1] for r in range(3))); sz=math.sqrt(sum(m[r][2]*m[r][2] for r in range(3)))
    r=[[m[i][j]/[sx,sy,sz][j] for j in range(3)] for i in range(3)]
    x=math.asin(max(-1,min(1,-r[1][2]))); cx=math.cos(x)
    if abs(cx)>1e-6: y=math.atan2(r[0][2],r[2][2]); z=math.atan2(r[1][0],r[1][1])
    else: y=math.atan2(-r[2][0],r[0][0]); z=0
    position=[clean_transform_value(m[i][3]) for i in range(3)]
    rotation=[clean_transform_value(math.degrees(angle)) for angle in (x,y,z)]
    scale=[clean_transform_value(value) for value in (sx,sy,sz)]
    return position,rotation,scale
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--stage',type=Path,required=True); ap.add_argument('--resources',type=Path,required=True); ap.add_argument('--force',action='store_true'); args=ap.parse_args()
    stage=args.stage.resolve(); resources=args.resources.resolve(); data_out=resources/'models/datas/bistro_modular'; model_out=resources/'models/bistro_modular'; scene_out=resources/'scenes/SC_bistro_exterior_modular.json'
    if args.force:
        for p in (data_out,model_out):
            if p.exists():
                import shutil; shutil.rmtree(p)
        if scene_out.exists(): scene_out.unlink()
    material_map,mi_by_slot=material_mapping(resources,stage)
    from convert_godot_bistro_materials import build_materials
    runtime_materials=build_materials(resources,stage,material_map,mi_by_slot,args.force)
    wrappers=list((stage/'Scenes').glob('**/*.tscn')); glbs=sorted((stage/'Meshes').glob('**/*.glb'))
    main_nodes=parse_main_instances(stage/'MainScene.tscn'); instances_by_scene={}
    for node in main_nodes:
        if node['scenePath']: instances_by_scene.setdefault(node['scenePath'],[]).append(node)
    variant_specs=[
        {'name':'FillOut_Bollard','scene':'Scenes/FillOut/Bollard.tscn','source':'Section02/S2Details.glb','base':'Scenes/FillOut/Bollard.tscn'},
        {'name':'FillOut_BushTree','scene':'Scenes/FillOut/BushTree.tscn','source':'Section01/S1Details.glb','base':'Scenes/FillOut/BushTree.tscn'},
        {'name':'FillOut_Cobblestone','scene':'Scenes/FillOut/Cobblestone.tscn','source':'Ground.glb','base':'Scenes/FillOut/Cobblestone.tscn'},
        {'name':'FillOut_Lamp','scene':'Scenes/FillOut/Lamp.tscn','source':'Section03/S3Lamps.glb','base':'Scenes/FillOut/Lamp.tscn'},
        {'name':'FillOut_Lamp_Props','scene':'Scenes/FillOut/Lamp_Props.tscn','source':'Section03/S3Lamps_Props.glb','base':'Scenes/Section03/S3Lamps_Props.tscn','rebase':['Bistro_Research_Exterior_Paris_StreetLight_01a___3__6267']}
    ]
    objects=[]; model_count=0; shared_counts={}
    for glb in glbs:
        rel=glb.relative_to(stage/'Meshes'); rel_out=Path(str(rel).replace('\\','/')); out_glb=data_out/rel_out; slots=bake_glb(glb,out_glb,wrappers,material_map,runtime_materials)
        stem='bistro_'+re.sub(r'[^A-Za-z0-9]+','_',str(rel.with_suffix(''))).strip('_'); descriptor_path=model_out/f'SM_{stem.removeprefix("bistro_")}.json'
        descriptor_path.parent.mkdir(parents=True,exist_ok=True)
        descriptor={'name':'Bistro Modular '+stem,'type':'mesh','modelDataPath':'models/datas/bistro_modular/'+rel_out.as_posix(),'materialSlots':[{'name':slot,'materialInstancePath':runtime_materials[slot]} for slot in slots]}
        descriptor_path.write_text(json.dumps(descriptor,indent=2)+'\n',encoding='utf-8'); model_count+=1
        model_path='models/bistro_modular/'+descriptor_path.name
        if glb.name=='PotPlants.glb':
            pot_instances=instances_by_scene.get('Scenes/FillOut/PotPlants.tscn',[]); shared_counts['FillOut_PotPlants']=len(pot_instances)
            for node in pot_instances:
                pos,rot,scale=decompose_matrix(node['world']); objects.append({'name':node['name'],'type':'mesh','modelPath':model_path,'position':pos,'scale':scale,'rotation':rot})
        else:
            objects.append({'name':'BistroModular_'+stem,'type':'mesh','modelPath':model_path,'position':[0.0,0.0,0.0],'scale':[1.0,1.0,1.0],'rotation':[0.0,0.0,0.0]})
    for spec in variant_specs:
        rel_out=Path('FillOut')/(spec['name']+'.glb'); slots,selected=bake_variant_glb(stage/'Meshes'/spec['source'],data_out/rel_out,stage/spec['base'],stage/spec['scene'],material_map,runtime_materials,spec.get('rebase'))
        descriptor_path=model_out/f'SM_{spec["name"]}.json'; descriptor={'name':'Bistro Modular '+spec['name'],'type':'mesh','modelDataPath':'models/datas/bistro_modular/'+rel_out.as_posix(),'materialSlots':[{'name':slot,'materialInstancePath':runtime_materials[slot]} for slot in slots]}
        descriptor_path.write_text(json.dumps(descriptor,indent=2)+'\n',encoding='utf-8'); model_count+=1
        authored_instances=instances_by_scene.get(spec['scene'],[]); shared_counts[spec['name']]=len(authored_instances)
        for node in authored_instances:
            name=node['name']
            if spec['name']=='FillOut_Lamp_Props' and node['parent']: name=node['parent'][-1]+'_Props'
            pos,rot,scale=decompose_matrix(node['world']); objects.append({'name':name,'type':'mesh','modelPath':'models/bistro_modular/'+descriptor_path.name,'position':pos,'scale':scale,'rotation':rot})
        print(f'{spec["name"]}: {selected} visible meshes, {len(authored_instances)} scene instances')
    objects += [
        {'name':'Sun_Light','type':'directionalLight','position':[0.0,40.0,0.0],'rotation':[-42.0,-32.0,0.0],'color':[1.0,0.94,0.84],'intensity':7.0},
        {'name':'Camera_Street','type':'camera','fov':72.0,'near_clip':0.1,'far_clip':1000.0,'position':[24.0,6.0,72.0],'rotation':[-3.0,0.0,0.0],'scale':[1.0,1.0,1.0]},
        {'name':'Environment_01','type':'environment','environment':{'type':'hdri','hdrPath':'hdri/bistro_san_giuseppe_bridge_4k.hdr','cubeSize':512,'intensity':1.0}}
    ]
    scene={'name':'Amazon Lumberyard Bistro Exterior Modular','type':'scene','objects':objects}; scene_out.parent.mkdir(parents=True,exist_ok=True); scene_out.write_text(json.dumps(scene,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'models':model_count,'scene_objects':len(objects),'mesh_objects':sum(x['type']=='mesh' for x in objects),'shared_instances':shared_counts,'material_mappings':len(runtime_materials)},indent=2))
if __name__=='__main__': main()
