#pragma once

#include <memory>

class Texture;
class PipelineFactory;

class BrdfLutGenerator
{
public:
    static std::shared_ptr<Texture> Generate(PipelineFactory& pipelineFactory);
};
