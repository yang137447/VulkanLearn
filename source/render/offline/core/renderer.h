#pragma once

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace VL::Offline
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1e-4;
constexpr int kLutSize = 16;
constexpr double kMinNoV = 0.17364817766693033;
constexpr int kTangentSamples = 32;
constexpr int kMaxTangentSamples = 64;

struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;
};
struct Vec2
{
    double x = 0.0, y = 0.0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
inline Vec3 operator*(Vec3 v, double s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator/(Vec3 v, double s) { return v * (1.0 / s); }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline double length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(Vec3 v) { return v / length(v); }

struct YarnMaterial
{
    double coverage;
    Vec3 dye;
    double kd, gammaS, gammaV;
    std::array<double, 8> offsets{};
    std::array<double, 7> lengths{};
    int controlPoints = 0;
    int sampleCount = kTangentSamples;
    std::array<double, kMaxTangentSamples> samples{};
    std::array<Vec2, kMaxTangentSamples> rotated{};
};
struct FabricMaterial
{
    const char* caseName;
    const char* label;
    double eta;
    std::array<YarnMaterial, 2> yarns;
    const char* limitation;
};
struct Ray { Vec3 origin, direction; };
struct Bounds
{
    Vec3 low{std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::infinity()};
    Vec3 high{-std::numeric_limits<double>::infinity(),
              -std::numeric_limits<double>::infinity(),
              -std::numeric_limits<double>::infinity()};
};
struct Triangle
{
    std::array<Vec3, 3> vertices, normals;
    Vec3 dpdu, dpdv;
    std::array<Vec2, 3> uv{};
};
struct BvhNode
{
    Bounds bounds;
    int left = -1, right = -1;
    std::size_t first = 0, count = 0;
};
struct Mesh
{
    std::vector<Triangle> triangles;
    std::vector<std::size_t> order;
    std::vector<BvhNode> nodes;
    Bounds bounds;
    bool hasValidUvFrames = true;
};
struct GroomMap;
struct ColorMap;
constexpr int kVelvetTableSize = 65;
struct VelvetMaterial
{
    double sigma = 0.5;
    double f0 = 0.05;
    Vec3 diffuse{0.018, 0.018, 0.018};
    double normalMean = 0.0;
    double normalization = 0.0;
    double diffuseScale = 0.0;
    std::array<double, kVelvetTableSize> g{}, reflectance{};
};
VelvetMaterial makeVelvetMaterial(double sigma, double f0, Vec3 diffuse);
double velvetSpecular(const VelvetMaterial& material, Vec3 n, Vec3 v, Vec3 l);
Vec3 velvetBrdf(const VelvetMaterial& material, Vec3 n, Vec3 v, Vec3 l);
enum class SurfaceModel {
    ClothSheen, ClothFiber, Lambert, AshikhminVelvet
};
struct MeshInstance
{
    const Mesh* mesh = nullptr;
    // localToWorld 的三列为世界空间基，worldToLocal 为其逆；平移单独保存。
    std::array<Vec3, 3> localToWorld{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    std::array<Vec3, 3> worldToLocal{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    Vec3 translation{};
    SurfaceModel model = SurfaceModel::Lambert;
    Vec3 baseColor{0.5, 0.5, 0.5};
    Vec3 sheenColor{0.95, 0.75, 0.65};
    double roughness = 0.4;
    const FabricMaterial* fabric = nullptr;
    const VelvetMaterial* velvet = nullptr;
    const GroomMap* groom = nullptr;
    const ColorMap* baseColorMap = nullptr;
    const ColorMap* sheenColorMap = nullptr;
    double yarnRotationDegrees = 0.0;
};
struct Hit
{
    double distance = std::numeric_limits<double>::infinity();
    Vec3 position, normal, geometricNormal, dpdu, dpdv;
    Vec2 uv;
    double roughness = 0.0;
    const MeshInstance* instance = nullptr;
};
struct GroomMap
{
    int width = 0, height = 0;
    std::vector<Vec2> directions;
    std::string source;
};
struct ColorMap
{
    int width = 0, height = 0;
    bool repeat = true, linear = true;
    std::vector<Vec3> pixels;
};
Vec3 sampleColorMap(const ColorMap& map, Vec2 uv);

// 纯 CPU 场景输入；实例引用的 mesh/material/map 由 RenderInput 冻结持有。
// 空实例就是空场景，不隐式构造论文固定几何或地面。
struct Scene
{
    std::vector<MeshInstance> instances;
    bool hasAreaEmitter = false;
    Vec3 backdrop{0.035, 0.04, 0.05};
    Vec3 lightCenter{-0.8, 3.8, 1.6};
    double lightWidth = 4.5, lightDepth = 2.0;
    bool directionalLight = false;
    Vec3 toLight{0.0, 1.0, 0.0};
    double directionalIrradiance = 0.0;
    Vec3 cameraPosition{0.0, 1.7, 8.0};
    Vec3 areaU{1, 0, 0}, areaV{0, 0, 1};
    double areaRadiance = 10.0;
    Vec3 areaColor{1, 1, 1};
    Vec3 directionalColor{1, 1, 1};
    std::uint32_t seed = 0;
};

double directionalAlbedo(double noV, double r);

struct AlbedoTable
{
    std::array<double, kLutSize * kLutSize> values{};
    AlbedoTable()
    {
        for (int y = 0; y < kLutSize; ++y) {
            const double u = static_cast<double>(y) / (kLutSize - 1);
            const double r = std::max(0.001, u * u);
            for (int x = 0; x < kLutSize; ++x) {
                const double noV = kMinNoV + (1.0 - kMinNoV) * x / (kLutSize - 1);
                values[y * kLutSize + x] = directionalAlbedo(noV, r);
            }
        }
    }

    double sample(double noV, double r) const
    {
        const double fx = std::clamp((noV - kMinNoV) / (1.0 - kMinNoV), 0.0, 1.0) *
                          (kLutSize - 1);
        const double fy = std::clamp(std::sqrt(r), 0.0, 1.0) * (kLutSize - 1);
        const int x = static_cast<int>(fx);
        const int y = static_cast<int>(fy);
        const int x1 = std::min(x + 1, kLutSize - 1);
        const int y1 = std::min(y + 1, kLutSize - 1);
        const double tx = fx - x;
        const double ty = fy - y;
        const double low = values[y * kLutSize + x] * (1.0 - tx) +
                           values[y * kLutSize + x1] * tx;
        const double high = values[y1 * kLutSize + x] * (1.0 - tx) +
                            values[y1 * kLutSize + x1] * tx;
        return low * (1.0 - ty) + high * ty;
    }
};
struct Random
{
    std::uint32_t state;
    double next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state) / 4294967296.0;
    }
};
struct DirectionSample { Vec3 direction; double pdf; };
struct PathResult { Vec3 direct, indirect; };

void aimAreaLight(Scene& scene, Vec3 target);
bool intersectAreaLight(const Scene& scene, Ray ray, double& distance);
double areaLightPdf(const Scene& scene, Vec3 position, Vec3 direction);
void resampleYarn(YarnMaterial& yarn, int count);
double component(Vec3 v, int axis);
void expand(Bounds& bounds, Vec3 point);
void expand(Bounds& bounds, const Bounds& other);
Bounds triangleBounds(const Triangle& triangle);
int buildBvh(Mesh& mesh, std::size_t first, std::size_t count);
int buildBvh(Mesh& mesh, std::size_t first, std::size_t count,
             const std::atomic<std::uint64_t>* latestGeneration,
             std::uint64_t expectedGeneration);
bool intersectsBounds(const Bounds& bounds, Ray ray, double maxDistance);
void intersectTriangle(const Triangle& triangle, Ray ray, double roughness, Hit& hit);
void intersectBvh(const Mesh& mesh, int index, Ray ray, double roughness, Hit& hit);
bool intersect(const Scene& scene, Ray ray, double maxDistance, Hit& hit);
double logLambda(double noX, double r);
double charlieD(double noH, double r);
double sheenBrdf(Vec3 n, Vec3 v, Vec3 l, double r, bool soften);
double directionalAlbedo(double noV, double r);
double dielectricFresnel(double eta, double cosine);
double gaussian(double widthDegrees, double angle);
double correlated(double a, double b, double delta);
double angleDifference(double a, double b);
Vec2 groomDirection(const GroomMap& map, Vec2 uv);
Hit orientYarns(Hit hit, const GroomMap* groom, double degrees);
std::array<Vec3, 2 * kMaxTangentSamples> yarnTangents(
    const Hit& hit, const FabricMaterial& material);
Vec3 microcylinderResponse(Hit hit, Vec3 view, Vec3 light,
                           const FabricMaterial& material);
Vec3 evaluateBsdf(const AlbedoTable& albedo, Hit hit, Vec3 view, Vec3 light);
Vec3 directLight(const Scene& scene, const AlbedoTable& albedo, Hit hit,
                 Vec3 view, int lightSamples, Random& rng);
double truncatedGaussianPdf(double angle, double mean, double width);
double microcylinderDirectionPdf(const Hit& hit, Vec3 view, Vec3 direction,
                                  const FabricMaterial& material);
DirectionSample sampleMicrocylinderDirection(const Hit& hit, Vec3 view,
                                              const FabricMaterial& material, Random& rng);
DirectionSample sampleDirection(Hit hit, Random& rng);
double directionPdf(Hit hit, Vec3 direction);
double powerWeight(double a, double b);
Vec3 directFabricAreaLight(const Scene& scene, const AlbedoTable& albedo,
                           Hit hit, Vec3 view, int lightSamples, Random& rng);
// A 臂返回 BRDF，后续乘 absCos/pdf；B 臂返回已含方向测度因子的 K，
// 后续只除以立体角 PDF，不能再次乘宏观 NoL。
PathResult trace(const Scene& scene, const AlbedoTable& albedo, Ray ray,
                 int bounces, int lightSamples, Random& rng);

} // namespace VL::Offline
