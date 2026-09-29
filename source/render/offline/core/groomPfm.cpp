#include "core/groomPfm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace VL::Offline
{
GroomMap ParseGroomPfm(std::string_view bytes, std::string source)
{
    std::istringstream in(std::string(bytes), std::ios::binary);
    GroomMap map;
    map.source = std::move(source);
    std::string magic, dimensions, scaleLine, extra;
    std::getline(in, magic);
    if (!magic.empty() && magic.back() == '\r') magic.pop_back();
    std::getline(in, dimensions);
    std::getline(in, scaleLine);
    std::istringstream sizeInput(dimensions), scaleInput(scaleLine);
    double scale = 0.0;
    if (magic != "PF" || !(sizeInput >> map.width >> map.height) ||
        (sizeInput >> extra) || !(scaleInput >> scale) || (scaleInput >> extra) ||
        map.width < 1 || map.height < 1 || map.width > 4096 ||
        map.height > 4096 || !std::isfinite(scale) || scale == 0.0)
        throw std::runtime_error("groom map requires RGB PFM, 1..4096 dimensions");
    const std::uint16_t endian = 1;
    const bool hostLittle = *reinterpret_cast<const unsigned char*>(&endian) == 1;
    map.directions.resize(static_cast<std::size_t>(map.width) * map.height);
    for (Vec2& direction : map.directions)
    {
        float channels[3]{};
        for (float& value : channels)
        {
            unsigned char data[4]{};
            if (!in.read(reinterpret_cast<char*>(data), 4))
                throw std::runtime_error("truncated groom PFM");
            if ((scale < 0.0) != hostLittle) std::reverse(data, data + 4);
            std::memcpy(&value, data, 4);
            if (!std::isfinite(value))
                throw std::runtime_error("non-finite groom direction");
        }
        const double norm = std::hypot(channels[0], channels[1]);
        if (norm < 1e-8) throw std::runtime_error("zero groom direction");
        direction = {channels[0] / norm, channels[1] / norm};
    }
    if (in.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("unexpected trailing data in groom PFM");
    return map;
}
} // namespace VL::Offline
