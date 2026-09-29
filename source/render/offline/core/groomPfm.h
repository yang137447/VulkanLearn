#pragma once

#include <string>
#include <string_view>
#include "core/renderer.h"

namespace VL::Offline
{
// RGB PFM 首行是 v=0；RG 为 signed UV 方向，第三通道只做有限性校验。
GroomMap ParseGroomPfm(std::string_view bytes, std::string source);
}
