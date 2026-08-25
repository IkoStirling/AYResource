#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayt::resource
{

// Canonical mesh-space contract shared by importers, procedural generators,
// persisted .aymesh assets and the renderer:
//   * coordinates are left-handed (+Z forward),
//   * clockwise clip-space winding is front-facing,
//   * the renderer removes back faces with BGFX_STATE_CULL_CCW.
//
// For a non-mirrored mesh in engine space, cross(p1 - p0, p2 - p0) points in
// the same direction as its outward vertex normals. A basis or world transform
// with a negative 3x3 determinant reverses winding exactly once.
enum class MeshFrontFaceWinding : std::uint8_t {
    Clockwise,
    CounterClockwise,
};

inline constexpr bool kMeshCoordinatesAreLeftHanded = true;
inline constexpr MeshFrontFaceWinding kMeshFrontFaceWinding =
    MeshFrontFaceWinding::Clockwise;

struct MeshWindingAudit {
    std::size_t triangleCount = 0;
    std::size_t comparedTriangleCount = 0;
    std::size_t mismatchedTriangleCount = 0;
    std::size_t degenerateTriangleCount = 0;
    std::size_t invalidIndexTriangleCount = 0;
};

inline void reverseTriangleWinding(std::vector<std::uint32_t>& indices)
{
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::swap(indices[i + 1], indices[i + 2]);
    }
}

// Validates the canonical relation between indexed geometry and vertex
// normals. Missing normals are intentionally not guessed. Importers may use
// this as a post-conversion diagnostic, but deterministic basis conversion
// remains authoritative and must not be replaced by heuristic auto-flipping.
inline MeshWindingAudit auditCanonicalMeshWinding(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<std::uint32_t>& indices,
    float relativeEpsilon = 1.0e-6f)
{
    MeshWindingAudit result;
    result.triangleCount = indices.size() / 3u;
    const std::size_t vertexCount = positions.size() / 3u;
    const std::size_t normalCount = normals.size() / 3u;

    for (std::size_t triangle = 0; triangle < result.triangleCount; ++triangle) {
        const std::size_t indexOffset = triangle * 3u;
        const std::uint32_t i0 = indices[indexOffset + 0u];
        const std::uint32_t i1 = indices[indexOffset + 1u];
        const std::uint32_t i2 = indices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            ++result.invalidIndexTriangleCount;
            continue;
        }

        const auto component = [&](std::uint32_t vertex, std::size_t axis) {
            return positions[static_cast<std::size_t>(vertex) * 3u + axis];
        };
        const float ax = component(i1, 0u) - component(i0, 0u);
        const float ay = component(i1, 1u) - component(i0, 1u);
        const float az = component(i1, 2u) - component(i0, 2u);
        const float bx = component(i2, 0u) - component(i0, 0u);
        const float by = component(i2, 1u) - component(i0, 1u);
        const float bz = component(i2, 2u) - component(i0, 2u);
        const float gx = ay * bz - az * by;
        const float gy = az * bx - ax * bz;
        const float gz = ax * by - ay * bx;
        const float geometryLengthSq = gx * gx + gy * gy + gz * gz;
        if (geometryLengthSq <= relativeEpsilon * relativeEpsilon) {
            ++result.degenerateTriangleCount;
            continue;
        }
        if (i0 >= normalCount || i1 >= normalCount || i2 >= normalCount) {
            continue;
        }

        const auto normalComponent = [&](std::uint32_t vertex, std::size_t axis) {
            return normals[static_cast<std::size_t>(vertex) * 3u + axis];
        };
        const float nx = normalComponent(i0, 0u) + normalComponent(i1, 0u)
                       + normalComponent(i2, 0u);
        const float ny = normalComponent(i0, 1u) + normalComponent(i1, 1u)
                       + normalComponent(i2, 1u);
        const float nz = normalComponent(i0, 2u) + normalComponent(i1, 2u)
                       + normalComponent(i2, 2u);
        const float normalLengthSq = nx * nx + ny * ny + nz * nz;
        if (normalLengthSq <= relativeEpsilon * relativeEpsilon) {
            continue;
        }

        ++result.comparedTriangleCount;
        const float dot = gx * nx + gy * ny + gz * nz;
        const float tolerance = relativeEpsilon
            * std::sqrt(geometryLengthSq * normalLengthSq);
        if (dot < -tolerance) {
            ++result.mismatchedTriangleCount;
        }
    }
    return result;
}

} // namespace ayt::resource
