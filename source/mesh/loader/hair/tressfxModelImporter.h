#pragma once

#include "mesh/loader/common/modelImporter.h"

// 将已校验的 TFX guide 转为正式 ModelResource，供现有网格上传与 World 候选消费。
// 只生成中心线的左右逻辑顶点；相机朝向展开属于 GPU 顶点工厂，不在 CPU 烘焙。
class TressfxModelImporter final : public IModelImporter
{
public:
    void ValidateImportRequest(const ModelImportRequest& request) const override;
    ModelResource Import(const ModelImportRequest& request) override;
};
