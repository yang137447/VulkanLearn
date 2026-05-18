# Schema Usage

Keep schema binding explicit and narrow.

- Bind `model_descriptor.schema.json` only to `resources/models/SM_*.json`.
- Bind `material_instance.schema.json` only to `resources/materials/**/MI_*.json`.
- Bind `scene.schema.json` only to `resources/scenes/*.json`.
- Bind `render_graph.schema.json` only to `config/renderGraphConfig.json`.
- Do not bind any schema to broad patterns like `*.json`.

Recommended VS Code workspace settings:

```json
{
    "json.schemas": [
        {
            "fileMatch": [
                "/resources/models/SM_*.json"
            ],
            "url": "./schema/model_descriptor.schema.json"
        },
        {
            "fileMatch": [
                "/resources/materials/MI_*.json",
                "/resources/materials/**/MI_*.json"
            ],
            "url": "./schema/material_instance.schema.json"
        },
        {
            "fileMatch": [
                "/resources/scenes/*.json"
            ],
            "url": "./schema/scene.schema.json"
        },
        {
            "fileMatch": [
                "/config/renderGraphConfig.json"
            ],
            "url": "./schema/render_graph.schema.json"
        }
    ]
}
```
