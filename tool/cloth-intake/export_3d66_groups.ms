-- Export the 3d66 clothing collection as normalized VulkanLearn source assets.
--
-- Run this from 3ds Max 2018+ with 3d66.com_23736651.max open.  The script
-- exports one FBX per top-level group (and any top-level geometry outside a
-- group), copies every bitmap reachable from that group's materials, and writes
-- groups.csv / materials.csv / textures.csv for
-- tool/cloth-intake/postprocess_3d66_exports.py.
--
-- Output root:
--   <VulkanLearn-worktree>/../VukanLearnResources/Maps/SC_cloth_showcase

(
    local outRoot = "D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/cloth-rendering/VukanLearnResources/Maps/SC_cloth_showcase"
    local modelDir = outRoot + "/Source/Models/"
    local texDir = outRoot + "/Source/Textures/"
    local groupCsv = outRoot + "/groups.csv"
    local materialCsv = outRoot + "/materials.csv"
    local textureCsv = outRoot + "/textures.csv"

    makeDir (outRoot + "/Source")
    makeDir modelDir
    makeDir texDir

    fn csvQuote value =
    (
        local text = value as string
        text = substituteString text "\"" "\"\""
        "\"" + text + "\""
    )

    fn csvRow values =
    (
        local result = ""
        for index = 1 to values.count do
        (
            if index > 1 do result += ","
            result += csvQuote values[index]
        )
        result
    )

    fn sanitizeName value =
    (
        local result = value as string
        result = substituteString result " " "_"
        result = substituteString result "/" "_"
        result = substituteString result "\\" "_"
        result = substituteString result ":" "_"
        result = substituteString result "." "_"
        result = substituteString result "(" "_"
        result = substituteString result ")" "_"
        result = substituteString result "#" "_"
        result = substituteString result "&" "_"
        result = substituteString result "+" "_"
        if result == "" do result = "unnamed"
        result
    )

    fn safeMaterialName material =
    (
        local result = try (material.name) catch ""
        if result == undefined or result == "" do result = "material"
        sanitizeName result
    )

    fn bitmapFile bitmap =
    (
        local result = undefined
        try (result = bitmap.filename) catch ()
        if result == undefined do try (result = bitmap.fileName) catch ()
        if result == undefined do try (result = bitmap.HDRIMapName) catch ()
        result
    )

    fn isBitmapClass value =
    (
        if classOf value == BitmapTexture do return true
        matchPattern (classOf value as string) pattern:"*VRayBitmap*"
    )

    fn collectBitmaps source depth =
    (
        local result = #()
        if source == undefined or depth > 8 do return result
        if isBitmapClass source do append result source
        local count = try (getNumSubTexmaps source) catch 0
        for index = 1 to count do
        (
            local child = try (getSubTexmap source index) catch undefined
            if child != undefined do result += collectBitmaps child (depth + 1)
        )
        result
    )

    fn slotLabel material index =
    (
        local result = try (getSubTexmapSlotName material index) catch undefined
        if result == undefined or result == "" do result = "slot" + index as string
        sanitizeName result
    )

    fn materialListFor objects =
    (
        local result = #()
        for object in objects do
        (
            local material = object.material
            if material == undefined do continue
            if classOf material == Multimaterial then
            (
                for child in material.materialList where child != undefined do
                (
                    if findItem result child == 0 do append result child
                )
            )
            else
            (
                if findItem result material == 0 do append result material
            )
        )
        result
    )

    local groupFile = createFile groupCsv
    format "%\n" (csvRow #("id", "root_name", "kind", "object_count", "material_count", "fbx")) to:groupFile
    local materialFile = createFile materialCsv
    format "%\n" (csvRow #("id", "material_index", "material_name", "normalized_name")) to:materialFile
    local textureFile = createFile textureCsv
    format "%\n" (csvRow #("id", "material_index", "material_name", "slot_index", "slot_name", "source", "normalized")) to:textureFile

    local roots = #()
    for group in groups where group.parent == undefined do append roots group
    for object in geometry where object.parent == undefined and not (isGroupHead object) do append roots object

    local exported = 0
    progressStart "Exporting 3d66 clothing groups"
    for index = 1 to roots.count do
    (
        local root = roots[index]
        local id = formattedPrint index format:"03d"
        local baseName = "cloth_" + id
        local fbxPath = modelDir + "SM_" + baseName + ".fbx"
        local kind = if (isGroupHead root) then "group" else "mesh"
        local selected = #(root)

        if isGroupHead root then
        (
            select root
            selected = selection as array
        )
        else
        (
            select root
        )

        local materials = materialListFor selected
        for materialIndex = 1 to materials.count do
        (
            local material = materials[materialIndex]
            format "%\n" (csvRow #(
                id,
                (formattedPrint materialIndex format:"02d"),
                material.name,
                safeMaterialName material)) to:materialFile
        )

        try
        (
            FBXExportSmoothingGroups true
            FBXExportTangents true
            FBXExportSmoothMesh true
            FBXExportApplyConstantKeyReducer false
            FBXExportScaleFactor 1.0
            FBXExportUpAxis #y
            exportFile fbxPath #noPrompt selectedOnly:true using:FBXEXP
            format "%\n" (csvRow #(
                id,
                root.name,
                kind,
                selected.count,
                materials.count,
                ("Maps/SC_cloth_showcase/Source/Models/SM_" + baseName + ".fbx"))) to:groupFile
            exported += 1
        )
        catch
        (
            format "EXPORT_FAILED % %\n" baseName (getCurrentException())
        )

        for materialIndex = 1 to materials.count do
        (
            local material = materials[materialIndex]
            local materialTag = "m" + (formattedPrint materialIndex format:"02d")
            local writtenSources = #()
            local bitmapIndex = 0
            local count = try (getNumSubTexmaps material) catch 0
            for slotIndex = 1 to count do
            (
                local source = try (getSubTexmap material slotIndex) catch undefined
                local bitmaps = collectBitmaps source 0
                for bitmap in bitmaps do
                (
                    local sourcePath = bitmapFile bitmap
                    if sourcePath == undefined or not (doesFileExist sourcePath) do continue
                    local dedupeKey = (slotIndex as string) + "|" + sourcePath
                    if findItem writtenSources dedupeKey > 0 do continue
                    append writtenSources dedupeKey
                    bitmapIndex += 1
                    local extension = getFilenameType sourcePath
                    if extension == "" do extension = ".png"
                    if extension[1] != "." do extension = "." + extension
                    local label = slotLabel material slotIndex
                    local normalized =
                        "T_" + baseName + "_" + materialTag + "_" +
                        label + "_" + (formattedPrint bitmapIndex format:"02d") + extension
                    local target = texDir + normalized
                    try
                    (
                        if doesFileExist target do deleteFile target
                        copyFile sourcePath target
                        format "%\n" (csvRow #(
                            id,
                            (formattedPrint materialIndex format:"02d"),
                            material.name,
                            (slotIndex as string),
                            label,
                            sourcePath,
                            normalized)) to:textureFile
                    )
                    catch
                    (
                        format "TEXTURE_COPY_FAILED % %\n" sourcePath (getCurrentException())
                    )
                )
            )
        )
        progressUpdate (100.0 * index / roots.count)
    )
    progressEnd()

    close groupFile
    close materialFile
    close textureFile
    messageBox (
        "Exported " + exported as string + " clothing roots.\n" +
        groupCsv + "\n" + materialCsv + "\n" + textureCsv)
)
