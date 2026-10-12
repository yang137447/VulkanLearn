// 与 autoExposureContract.h 的 dispatch/std140 合同保持一致；参数只来自冻结相机。
const uint kExposureBinCount = 64u;
const float kMinLogLuminance = -12.0;
const float kMaxLogLuminance = 16.0;
const float kLowPercentile = 0.10;
const float kHighPercentile = 0.90;

layout(set = 0, binding = 2, std430) buffer Histogram {
    uint bins[64];
} histogram;
