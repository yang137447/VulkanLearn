"""Print UE FBX export option properties for the Third Person audit."""

import unreal


def main():
    options = unreal.FbxExportOption()
    unreal.log("[VL FBX Options] " + ", ".join(
        name for name in dir(options) if not name.startswith("_")))


if __name__ == "__main__":
    main()
