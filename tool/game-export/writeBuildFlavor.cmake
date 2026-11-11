# Writes the build-flavor marker next to a target executable.
# Inputs: VL_FLAVOR (editor|game), VL_EDITOR_ENABLED (ON|OFF), VL_OUTPUT.
if(NOT DEFINED VL_FLAVOR OR NOT DEFINED VL_EDITOR_ENABLED OR NOT DEFINED VL_OUTPUT)
    message(FATAL_ERROR "writeBuildFlavor.cmake requires VL_FLAVOR, VL_EDITOR_ENABLED and VL_OUTPUT")
endif()

if(VL_EDITOR_ENABLED)
    set(vl_editor_enabled true)
else()
    set(vl_editor_enabled false)
endif()

file(WRITE "${VL_OUTPUT}"
    "{\n  \"flavor\": \"${VL_FLAVOR}\",\n  \"editorEnabled\": ${vl_editor_enabled}\n}\n")
