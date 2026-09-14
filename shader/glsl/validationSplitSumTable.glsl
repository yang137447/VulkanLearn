// 自动生成，请勿手改：tool/validation/verify_split_sum.py --emit-glsl
//
// D-09 split-sum 相对误差表（Karis 2013 §5）：同一环境、同一 G 支（IBL k=α/2）下，
// split-sum 估计与逐样本参考积分之比的相对误差，扫 roughness、F0=1、θv=60°。
// 索引顺序：[env][roughness]；-1 = 该配置参考值太小、相对误差无意义（图上断开）。
// 采样：参考 4096 / 预过滤 4096 / LUT 4096。
#ifndef VL_VALIDATION_SPLIT_SUM_TABLE_GLSL
#define VL_VALIDATION_SPLIT_SUM_TABLE_GLSL

const int PLOT_SPLITSUM_ENV_COUNT = 4;
const int PLOT_SPLITSUM_ROUGHNESS_COUNT = 9;
const float PLOT_SPLITSUM_ROUGHNESS[9] = float[](0.05, 0.15, 0.25, 0.40, 0.50, 0.60, 0.70, 0.85, 1.00);
const float PLOT_SPLITSUM_VIEW_DEG = 60.0;
// arm 0 = constant
// arm 1 = cosine
// arm 2 = sun
// arm 3 = bistro4k
const float PLOT_SPLITSUM_TABLE[36] = float[](
    0.00001, 0.00068, 0.00136, 0.00066, 0.00026, 0.00015, 0.00001, 0.00000, 0.00003,
    -1.00000, -1.00000, -1.00000, 0.34609, 0.41682, 0.47513, 0.51437, 0.54893, 0.56559,
    0.00001, 0.00068, 0.02344, 0.26782, 0.54312, 0.64116, 0.66236, 0.72052, 0.70103,
    0.00263, 0.02710, 0.05441, 0.03124, 0.04260, 0.13100, 0.23845, 0.37416, 0.48655
);

#endif
