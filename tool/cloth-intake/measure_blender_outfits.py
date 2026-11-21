import bpy
import numpy as np


def outfit_bounds(collection):
    minimum = None
    maximum = None
    for obj in collection.objects:
        if obj.type != "MESH" or len(obj.data.vertices) == 0:
            continue
        coordinates = np.empty(len(obj.data.vertices) * 3, dtype=np.float64)
        obj.data.vertices.foreach_get("co", coordinates)
        coordinates = coordinates.reshape(-1, 3)
        matrix = np.array(obj.matrix_world, dtype=np.float64)
        points = coordinates @ matrix[:3, :3].T + matrix[:3, 3]
        object_minimum = points.min(axis=0)
        object_maximum = points.max(axis=0)
        if minimum is None:
            minimum = object_minimum
            maximum = object_maximum
        else:
            minimum = np.minimum(minimum, object_minimum)
            maximum = np.maximum(maximum, object_maximum)
    return minimum, maximum


root = bpy.data.collections.get("3d66 Cloth Showcase")
heights = []
for collection in sorted(root.children, key=lambda item: item.name):
    minimum, maximum = outfit_bounds(collection)
    if minimum is None:
        continue
    height = float(maximum[2] - minimum[2])
    heights.append(height)
    print(
        collection.name,
        f"minz={float(minimum[2]):.4f}",
        f"maxz={float(maximum[2]):.4f}",
        f"height={height:.4f}",
    )
print(
    f"heights min={min(heights):.4f} max={max(heights):.4f} "
    f"mean={sum(heights)/len(heights):.4f}"
)
