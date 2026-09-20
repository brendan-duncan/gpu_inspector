// The scene of "Ray Tracing in One Weekend"'s final render (Peter Shirley, Trevor David Black,
// Steve Hollasch; https://raytracing.github.io/books/RayTracingInOneWeekend.html, section 14.1),
// shared by the Vulkan, Direct3D 12 and Metal path tracers so all three trace the same spheres.
//
// The book picks the small spheres with an unseeded random number generator; this one is
// seeded, so every run and every API gets the same scene and the same camera, and a capture from
// one API can be held against a capture from another.
//
// The spheres are grouped by material: each group is one bottom-level structure of bounding
// boxes, and the top level holds one instance per group whose shader binding table offset is the
// material (Vulkan, D3D12) and whose custom index is the group's first sphere. Metal has no hit
// shaders, so its kernel looks both up in a small table by instance index instead.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace rtiow {

enum Material : uint32_t {
    kLambertian = 0,
    kMetal = 1,
    kDielectric = 2,
    kMaterialCount = 3,
};

inline const char* MaterialName(uint32_t m) {
    static const char* const names[kMaterialCount] = {"Lambertian", "Metal", "Dielectric"};
    return m < kMaterialCount ? names[m] : "?";
}

// 32 bytes, and the same layout in GLSL std430 (vec3 + float), an HLSL StructuredBuffer and MSL
// (packed_float3 + float).
struct Sphere {
    float center[3];
    float radius;
    float albedo[3];
    float param;   // metal: fuzz; dielectric: index of refraction; Lambertian: unused
};

// The layout of VkAabbPositionsKHR, D3D12_RAYTRACING_AABB and MTLAxisAlignedBoundingBox.
struct Aabb {
    float min[3];
    float max[3];
};

// The book's camera, reduced to what a ray needs (camera::initialize and camera::get_ray). The
// same layout as a std140 uniform block, an HLSL cbuffer and an MSL struct of packed_float3s:
// each vec3 is followed by a float that fills its row.
struct Camera {
    float center[3];
    float defocusAngle;   // degrees; 0 disables the defocus blur
    float pixel00[3];     // the center of the top left pixel
    float pad0;
    float pixelDeltaU[3];
    float pad1;
    float pixelDeltaV[3];
    float pad2;
    float defocusDiskU[3];
    float pad3;
    float defocusDiskV[3];
    float pad4;
};

// Push constants (Vulkan), root constants (D3D12), inline bytes (Metal).
struct FrameParams {
    uint32_t frame;             // samples already accumulated, divided by samplesPerFrame
    uint32_t samplesPerFrame;
    uint32_t maxDepth;          // bounces before a path is given up as black
    uint32_t accumulate;        // 0: every frame stands alone (and is noisy)
};

// splitmix64: small, and the same on every compiler, unlike std's distributions.
class Random {
public:
    explicit Random(uint64_t seed) : state_(seed) {}
    // [0, 1)
    double Next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return (double)(z >> 11) * (1.0 / 9007199254740992.0);
    }
    double Next(double lo, double hi) { return lo + (hi - lo) * Next(); }

private:
    uint64_t state_;
};

struct Scene {
    std::vector<Sphere> spheres;       // grouped by material, in Material order
    uint32_t first[kMaterialCount]{};  // each group's first sphere
    uint32_t count[kMaterialCount]{};
};

inline Scene MakeScene(uint64_t seed = 1) {
    std::vector<Sphere> groups[kMaterialCount];
    auto add = [&](Material m, float x, float y, float z, float radius, float r, float g, float b, float param) {
        groups[m].push_back(Sphere{{x, y, z}, radius, {r, g, b}, param});
    };

    add(kLambertian, 0.0f, -1000.0f, 0.0f, 1000.0f, 0.5f, 0.5f, 0.5f, 0.0f);   // the ground

    Random random(seed);
    for (int a = -11; a < 11; ++a) {
        for (int b = -11; b < 11; ++b) {
            double chooseMaterial = random.Next();
            float x = (float)(a + 0.9 * random.Next());
            float z = (float)(b + 0.9 * random.Next());
            float dx = x - 4.0f, dz = z;   // clear of the big metal sphere at (4, 0.2, 0)
            if (std::sqrt(dx * dx + dz * dz) <= 0.9f) continue;
            if (chooseMaterial < 0.8) {
                // albedo = random() * random()
                float c[3];
                for (float& v : c) v = (float)(random.Next() * random.Next());
                add(kLambertian, x, 0.2f, z, 0.2f, c[0], c[1], c[2], 0.0f);
            } else if (chooseMaterial < 0.95) {
                float c[3];
                for (float& v : c) v = (float)random.Next(0.5, 1.0);
                add(kMetal, x, 0.2f, z, 0.2f, c[0], c[1], c[2], (float)random.Next(0.0, 0.5));
            } else {
                add(kDielectric, x, 0.2f, z, 0.2f, 1.0f, 1.0f, 1.0f, 1.5f);
            }
        }
    }

    add(kDielectric, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f);
    add(kLambertian, -4.0f, 1.0f, 0.0f, 1.0f, 0.4f, 0.2f, 0.1f, 0.0f);
    add(kMetal, 4.0f, 1.0f, 0.0f, 1.0f, 0.7f, 0.6f, 0.5f, 0.0f);

    Scene scene;
    for (uint32_t m = 0; m < kMaterialCount; ++m) {
        scene.first[m] = (uint32_t)scene.spheres.size();
        scene.count[m] = (uint32_t)groups[m].size();
        scene.spheres.insert(scene.spheres.end(), groups[m].begin(), groups[m].end());
    }
    return scene;
}

// One material's bounding boxes, in the order of its spheres: box i bounds sphere first[m] + i,
// which is how the intersection shaders find a sphere from the primitive index.
inline std::vector<Aabb> Bounds(const Scene& scene, Material m) {
    std::vector<Aabb> boxes;
    for (uint32_t i = 0; i < scene.count[m]; ++i) {
        const Sphere& s = scene.spheres[scene.first[m] + i];
        float r = std::fabs(s.radius);
        boxes.push_back(Aabb{{s.center[0] - r, s.center[1] - r, s.center[2] - r},
                             {s.center[0] + r, s.center[1] + r, s.center[2] + r}});
    }
    return boxes;
}

// The book's final camera: vfov 20, looking from (13, 2, 3) at the origin, defocus angle 0.6
// and focus distance 10.
inline Camera MakeCamera(uint32_t width, uint32_t height) {
    const double lookFrom[3] = {13.0, 2.0, 3.0};
    const double lookAt[3] = {0.0, 0.0, 0.0};
    const double vup[3] = {0.0, 1.0, 0.0};
    const double vfov = 20.0, defocusAngle = 0.6, focusDist = 10.0;
    const double pi = 3.1415926535897932385;

    auto sub = [](const double* a, const double* b, double* out) { for (int i = 0; i < 3; ++i) out[i] = a[i] - b[i]; };
    auto cross = [](const double* a, const double* b, double* out) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    };
    auto normalize = [](double* v) {
        double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int i = 0; i < 3; ++i) v[i] /= l;
    };

    double h = std::tan(vfov * pi / 180.0 / 2.0);
    double viewportHeight = 2.0 * h * focusDist;
    double viewportWidth = viewportHeight * ((double)width / (double)height);

    double w[3], u[3], v[3];
    sub(lookFrom, lookAt, w);
    normalize(w);
    cross(vup, w, u);
    normalize(u);
    cross(w, u, v);

    Camera c{};
    double defocusRadius = focusDist * std::tan(defocusAngle / 2.0 * pi / 180.0);
    for (int i = 0; i < 3; ++i) {
        double viewportU = viewportWidth * u[i];
        double viewportV = viewportHeight * -v[i];
        double deltaU = viewportU / width;
        double deltaV = viewportV / height;
        double upperLeft = lookFrom[i] - focusDist * w[i] - viewportU / 2.0 - viewportV / 2.0;
        c.center[i] = (float)lookFrom[i];
        c.pixelDeltaU[i] = (float)deltaU;
        c.pixelDeltaV[i] = (float)deltaV;
        c.pixel00[i] = (float)(upperLeft + 0.5 * (deltaU + deltaV));
        c.defocusDiskU[i] = (float)(u[i] * defocusRadius);
        c.defocusDiskV[i] = (float)(v[i] * defocusRadius);
    }
    c.defocusAngle = (float)defocusAngle;
    return c;
}

} // namespace rtiow
