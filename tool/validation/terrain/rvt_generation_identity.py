"""Current Cook identity; independent of immutable P0 historical baselines."""

COOK_IDENTITY = {
    "generatorVersion": 3,
    "filterRule": "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3",
    "encoding": "gpu-rvt-adjacency-area4-rgba8-v3",
}

RUNTIME_COMPUTES = (
    "terrain/terrainCompositedBakeWorld.comp",
    "terrain/terrainCompositedBakeWorldPadding.comp",
)

RETAINED_LEGACY_COMPUTES = ()
