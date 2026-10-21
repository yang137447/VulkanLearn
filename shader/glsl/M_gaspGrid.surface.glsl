#include "materialFunction/mf_gaspGrid.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    return EvaluateMFGaspGrid(context);
}
