"""从 erode.blend 渲染一张能看清地形的效果图（后台，不受窗口遮挡影响）。"""

import math

import bpy

BLEND = (r"D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld"
         r"\VulkanLearn\artifacts\pegasus\erode\erode.blend")
OUT = (r"D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld"
       r"\VulkanLearn\artifacts\pegasus\erode\erode_preview.png")

bpy.ops.wm.open_mainfile(filepath=BLEND)
scene = bpy.context.scene
obj = bpy.data.objects["VL_Pegasus_Erode"]

# 相机：从斜上方 45° 俯视地形中心
camera_data = bpy.data.cameras.new("VL_Cam")
camera_data.lens = 50.0
camera = bpy.data.objects.new("VL_Cam", camera_data)
bpy.context.collection.objects.link(camera)
# 地形在 XY 平面、边长 1000，高度只有 ±20 m。相机要贴近并俯视中心。
camera.location = (0.0, -850.0, 520.0)
camera.rotation_euler = (math.radians(58), 0.0, 0.0)
scene.camera = camera

# 太阳光
if bpy.data.objects.get("VL_Sun") is None:
    light_data = bpy.data.lights.new("VL_Sun", type="SUN")
    light_data.energy = 4.0
    light_data.angle = math.radians(2.0)
    sun = bpy.data.objects.new("VL_Sun", light_data)
    bpy.context.collection.objects.link(sun)
    sun.rotation_euler = (math.radians(52), 0.0, math.radians(35))

# 材质
mat = bpy.data.materials.get("VL_Terrain_Preview")
if mat is None:
    mat = bpy.data.materials.new("VL_Terrain_Preview")
mat.use_nodes = True
bsdf = mat.node_tree.nodes.get("Principled BSDF")
if bsdf:
    bsdf.inputs["Base Color"].default_value = (0.58, 0.54, 0.47, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.9
obj.data.materials.clear()
obj.data.materials.append(mat)

# 世界：淡天空
world = bpy.data.worlds.get("World") or bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
background = world.node_tree.nodes.get("Background")
if background:
    background.inputs["Color"].default_value = (0.35, 0.45, 0.60, 1.0)
    background.inputs["Strength"].default_value = 1.2

# 渲染设置：EEVEE 快速出图
scene.render.engine = "BLENDER_EEVEE"
scene.render.resolution_x = 1280
scene.render.resolution_y = 720
scene.render.filepath = OUT
scene.render.image_settings.file_format = "PNG"
scene.render.film_transparent = False

bpy.ops.render.render(write_still=True)
print(f"RENDER written={OUT}")
