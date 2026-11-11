"""Replace procedural/reused particle textures with exported UE FX textures."""

from __future__ import annotations

import json
from pathlib import Path


RESOURCE_ROOT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources"
)
FX = "Common/Source/UE/NiagaraExamples/Textures/FX"
DESC = "Common/Textures/UE_FX"

EOO_ATLASES = {
    "T_Explosion_EOO",
    "T_ExplosionBurst_EOO_Loop",
    "T_ExplosionRoil_EOO_Loop",
    "T_FireBall_EOO_Loop",
    "T_FireRoil_EOO_Loop",
    "T_SmokePuff_EOO_Loop",
    "T_SmokePuffLight_EOO_Loop",
    "T_SmokeRoil_EOO_Loop",
    "T_SmokeTrail_EOO_Loop",
}

SPRITE_PATHS = {
    "T_ExplosionBurst_EOO_Loop": FX + "/Textures/Sprites/T_ExplosionBurst_EOO_Loop/T_ExplosionBurst_EOO_Loop.png",
    "T_ExplosionRoil_EOO_Loop": FX + "/Textures/Sprites/T_ExplosionRoil_EOO_Loop/T_ExplosionRoil_EOO_Loop.png",
    "T_FireBall_EOO_Loop": FX + "/Textures/Sprites/T_FireBall_EOO_Loop/T_FireBall_EOO_Loop.png",
    "T_SmokePuffLight_EOO_Loop": FX + "/Textures/Sprites/T_SmokePuffLight_EOO_Loop/T_SmokePuffLight_EOO_Loop.png",
    "T_SmokeRoil_EOO_Loop": FX + "/Textures/Sprites/T_SmokeRoil_EOO_Loop/T_SmokeRoil_EOO_Loop.png",
    "T_Plasma_Wisps": FX + "/Textures/Sprites/T_Plasma_Wisps/T_Plasma_Wisps.png",
    "T_MuzzleFlash": FX + "/FX_Weapons/MuzzleFlashes/Textures/T_MuzzleFlash/T_MuzzleFlash.png",
    "T_MuzzleFlash_Side": FX + "/FX_Weapons/MuzzleFlashes/Textures/T_MuzzleFlash_Side/T_MuzzleFlash_Side.png",
    "T_MuzzleFlash_Sphere": FX + "/FX_Weapons/MuzzleFlashes/Textures/T_MuzzleFlash_Sphere/T_MuzzleFlash_Sphere.png",
    "T_ThinSmoke_FX": FX + "/FX_Weapons/MuzzleFlashes/Textures/T_ThinSmoke_FX/T_ThinSmoke_FX.png",
    "T_Hermite2Noise_CenteredTiling": FX + "/Textures/Noise/T_Hermite2Noise_CenteredTiling/T_Hermite2Noise_CenteredTiling.png",
}


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save(path: Path, document) -> None:
    path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")


def atlas_fields(name: str):
    if name in EOO_ATLASES:
        return {
            "columns": 8, "rows": 8, "fps": 20.0, "startFrame": 0,
            "endFrame": 63, "loop": True, "randomStartFrame": True,
            "perParticle": True, "blend": True, "playRate": 1.0,
        }, True, 2.2
    if name == "T_Plasma_Wisps":
        return {
            "columns": 8, "rows": 8, "fps": 20.0, "startFrame": 0,
            "endFrame": 63, "loop": True, "randomStartFrame": True,
            "perParticle": True, "blend": True, "playRate": 1.0,
        }, False, None
    if name == "T_ThinSmoke_FX":
        return {
            "columns": 8, "rows": 8, "fps": 20.0, "startFrame": 0,
            "endFrame": 63, "loop": True, "randomStartFrame": True,
            "perParticle": True, "blend": True, "playRate": 1.0,
        }, False, None
    if name == "T_MuzzleFlash":
        return {
            "columns": 4, "rows": 1, "fps": 20.0, "startFrame": 0,
            "endFrame": 3, "loop": False, "randomStartFrame": False,
            "perParticle": False, "blend": False, "playRate": 1.0,
        }, False, None
    if name == "T_MuzzleFlash_Side":
        return {
            "columns": 2, "rows": 1, "fps": 20.0, "startFrame": 0,
            "endFrame": 1, "loop": False, "randomStartFrame": False,
            "perParticle": False, "blend": False, "playRate": 1.0,
        }, False, None
    if name == "T_MuzzleFlash_Sphere":
        return {
            "columns": 2, "rows": 2, "fps": 20.0, "startFrame": 0,
            "endFrame": 3, "loop": False, "randomStartFrame": False,
            "perParticle": False, "blend": False, "playRate": 1.0,
        }, False, None
    if name.startswith("T_Hermite") or "Noise" in name:
        return {
            "columns": 1, "rows": 1, "fps": 0.0, "startFrame": 0,
            "endFrame": 0, "loop": False, "randomStartFrame": False,
            "perParticle": False, "blend": False, "playRate": 1.0,
        }, False, None
    return None, False, None


def replacement_for(old: str, emitter: str) -> str | None:
    if old.endswith("T_ue_fireball_detail.png"):
        return SPRITE_PATHS["T_ExplosionBurst_EOO_Loop"]
    if old.endswith("T_ue_roil_smoke.png"):
        return SPRITE_PATHS["T_SmokeRoil_EOO_Loop"]
    if old.endswith("T_ue_dust.png"):
        return SPRITE_PATHS["T_SmokePuffLight_EOO_Loop"]
    if old.endswith("T_advanced_fire.png"):
        return SPRITE_PATHS["T_FireBall_EOO_Loop"]
    if old.endswith("T_advanced_smoke.png"):
        return SPRITE_PATHS["T_SmokePuffLight_EOO_Loop"]
    if old.endswith("T_Muzzle_Cone.tga"):
        return SPRITE_PATHS["T_MuzzleFlash_Side"]
    if old.endswith("T_Muzzle_Smoke.tga"):
        return SPRITE_PATHS["T_ThinSmoke_FX"]
    if old.endswith("T_Muzzle_Distortion.tga"):
        return SPRITE_PATHS["T_Hermite2Noise_CenteredTiling"]
    if old.endswith("T_Muzzle_Star.tga"):
        if emitter == "flash_side":
            return SPRITE_PATHS["T_MuzzleFlash_Side"]
        if emitter in ("flash_front", "flash_center"):
            return SPRITE_PATHS["T_MuzzleFlash_Sphere"]
        return SPRITE_PATHS["T_MuzzleFlash_Sphere"]
    return None


def patch_particle_system(path: Path) -> bool:
    document = load(path)
    changed = False
    for emitter in document.get("emitters", []):
        emitter_name = str(emitter.get("name", ""))
        for module in emitter.get("modules", []):
            if module.get("type") != "flipbook":
                continue
            fields = module.setdefault("fields", {})
            old = str(fields.get("texture", ""))
            new = replacement_for(old, emitter_name)
            if new is None:
                continue
            fields["texture"] = new
            name = Path(new).stem
            atlas, eoo, strength = atlas_fields(name)
            if atlas is not None:
                fields["flipbook"] = atlas
                if eoo:
                    fields["eooPacked"] = True
                    if strength is not None:
                        fields["eooEmissiveStrength"] = strength
                else:
                    fields.pop("eooPacked", None)
                    fields.pop("eooEmissiveStrength", None)
            changed = True
    if changed:
        save(path, document)
    return changed


def fill_empty_sprites(path: Path, mapping: dict[str, str]) -> bool:
    document = load(path)
    changed = False
    for emitter in document.get("emitters", []):
        name = str(emitter.get("name", ""))
        if name not in mapping:
            continue
        modules = emitter.get("modules", [])
        existing = None
        for module in modules:
            if module.get("type") == "flipbook":
                existing = module
                break
        if existing is None:
            existing = {"type": "flipbook", "enabled": True, "fields": {}}
            modules.append(existing)
        fields = existing.setdefault("fields", {})
        if fields.get("texture"):
            continue
        texture_name = mapping[name]
        fields["texture"] = SPRITE_PATHS[texture_name]
        atlas, eoo, strength = atlas_fields(texture_name)
        fields["flipbook"] = atlas
        if eoo:
            fields["eooPacked"] = True
            if strength is not None:
                fields["eooEmissiveStrength"] = strength
        changed = True
    if changed:
        save(path, document)
    return changed


def replace_strings(value, replacements):
    if isinstance(value, str):
        return replacements.get(value, value)
    if isinstance(value, list):
        return [replace_strings(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: replace_strings(item, replacements) for key, item in value.items()}
    return value


def patch_decals_and_materials() -> list[str]:
    touched = []
    particle_dirs = [path for path in (RESOURCE_ROOT / "Maps").glob("SC_particle_*")]
    for folder in particle_dirs:
        for path in list(folder.rglob("SC_*.json")) + list(folder.rglob("PS_*.json")) \
                + list(folder.rglob("MI_*.json")):
            document = load(path)
            replacements = {}
            # Footprint / bullet-hole decals.
            replacements["Common/Source/Textures/NiagaraExamples/T_Footprint_Mask.json"] = \
                DESC + "/T_Footprint_Mask.json"
            replacements["Common/Source/Textures/NiagaraExamples/T_BulletHole_Mask.json"] = \
                DESC + "/BulletHole.json"
            # Impact decals.
            if path.name in ("SC_ue_footstep_gallery.json", "SC_ue_ndc_gallery.json"):
                replacements["Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_Normal.json"] = \
                    DESC + "/T_Footprint_Normals.json"
            else:
                replacements["Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_Normal.json"] = \
                    DESC + "/BulletHole_Normals.json"
            replacements["Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_BaseColor.json"] = \
                DESC + "/BulletHole.json"
            replacements["Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_ORM.json"] = \
                DESC + "/BulletHole_Normals.json"
            # Debris materials.
            if path.name == "MI_ue_wood_chip.json":
                replacements.update({
                    "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_BaseColor.json":
                        DESC + "/T_Wood_Debris_vb2mfeliw_2K_D.json",
                    "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_Normal.json":
                        DESC + "/T_Wood_Debris_vb2mfeliw_2K_N.json",
                })
            if path.name == "MI_ue_concrete_chip.json":
                replacements.update({
                    "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_BaseColor.json":
                        DESC + "/T_Concrete_Rubble_uc1iccejw_1K_D.json",
                    "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_Normal.json":
                        DESC + "/T_Concrete_Rubble_uc1iccejw_1K_N.json",
                })
            if path.name == "MI_ue_metal_chip.json":
                replacements.update({
                    "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_BaseColor.json":
                        DESC + "/T_Rock_shopk_2K_D.json",
                    "Maps/SC_particle_ue_impact/Textures/T_Impact_Concrete_Normal.json":
                        DESC + "/T_Rock_shopk_2K_N.json",
                })
            if path.name == "MI_ue_explosion_debris.json":
                replacements.update({
                    "Maps/SC_particle_ue_explosion/Textures/T_Explosion_Scorch_BaseColor.json":
                        DESC + "/T_Rock_shopk_2K_D.json",
                    "Maps/SC_particle_ue_explosion/Textures/T_Explosion_Scorch_Normal.json":
                        DESC + "/T_Rock_shopk_2K_N.json",
                })
            updated = replace_strings(document, replacements)
            if updated != document:
                save(path, updated)
                touched.append(str(path.relative_to(RESOURCE_ROOT)).replace("\\", "/"))
    return touched


def main() -> None:
    changed = []
    for path in sorted((RESOURCE_ROOT / "Maps").glob("SC_particle_*/PS_*.json")):
        if patch_particle_system(path):
            changed.append(str(path.relative_to(RESOURCE_ROOT)).replace("\\", "/"))

    tesla = RESOURCE_ROOT / "Maps/SC_particle_ue_tesla/PS_ue_tesla_coil.json"
    if tesla.is_file():
        fill_empty_sprites(tesla, {
            "ring_glow_left": "T_Plasma_Wisps",
            "ring_glow_right": "T_Plasma_Wisps",
            "ring_sparks_left": "T_Plasma_Wisps",
            "ring_sparks_right": "T_Plasma_Wisps",
            "tesla_smoke": "T_ThinSmoke_FX",
        })

    firework = RESOURCE_ROOT / "Maps/SC_particle_ue_firework/PS_ue_firework.json"
    if firework.is_file():
        fill_empty_sprites(firework, {
            "rocket_leader": "T_Plasma_Wisps",
            "ring_sparks": "T_Plasma_Wisps",
        })

    touched = patch_decals_and_materials()
    print("particle systems=%d decals/materials=%d" % (len(changed), len(touched)))


if __name__ == "__main__":
    main()
