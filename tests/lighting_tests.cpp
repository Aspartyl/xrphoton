#include "lighting.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>

#include <glm/geometric.hpp>

namespace
{
int failureCount = 0;

void expect(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

bool nearly(float left, float right, float tolerance = 1.0e-6f)
{
    return std::abs(left - right) <= tolerance;
}

void testPayloadLayout()
{
    expect(
        sizeof(xrphoton::RaygenPushConstants) == 96,
        "raygen payload has the pinned 96-byte size");
    expect(
        offsetof(xrphoton::RaygenPushConstants, camera) == 0
            && offsetof(xrphoton::RaygenPushConstants, sunDirection) == 64
            && offsetof(xrphoton::RaygenPushConstants, sunRadiance) == 80
            && offsetof(xrphoton::RaygenPushConstants, frameIndex) == 92,
        "raygen payload fields have the pinned shader offsets");
}

void testPushConstantConstruction()
{
    xrphoton::CameraPushConstants camera{};
    camera.origin = {1.0f, 2.0f, 3.0f};
    camera.pad0 = 4.0f;
    camera.forward = {5.0f, 6.0f, 7.0f};
    camera.pad1 = 8.0f;
    camera.right = {9.0f, 10.0f, 11.0f};
    camera.pad2 = 12.0f;
    camera.up = {13.0f, 14.0f, 15.0f};
    camera.pad3 = 16.0f;

    const xrphoton::DirectionalSun sun{
        .direction = {3.0f, 4.0f, 0.0f},
        .radiance = {1.25f, 2.5f, 3.75f},
    };
    constexpr std::uint32_t FrameIndex = 0x89abcdefu;
    const xrphoton::RaygenPushConstants result =
        xrphoton::makeRaygenPushConstants(camera, sun, FrameIndex);

    expect(
        std::memcmp(&result.camera, &camera, sizeof(camera)) == 0,
        "camera payload is copied byte-for-byte");
    expect(
        nearly(result.sunDirection.x, 0.6f)
            && nearly(result.sunDirection.y, 0.8f)
            && nearly(result.sunDirection.z, 0.0f)
            && nearly(glm::length(result.sunDirection), 1.0f),
        "sun direction is normalized");
    expect(
        result.pad0 == 0.0f,
        "raygen payload padding is initialized");
    expect(
        result.sunRadiance == sun.radiance,
        "sun radiance is copied without hidden policy");
    expect(
        result.frameIndex == FrameIndex,
        "frame index is passed through");

    const xrphoton::DirectionalSun degenerateSun{
        .direction = {},
        .radiance = {0.25f, 0.5f, 0.75f},
    };
    const xrphoton::RaygenPushConstants degenerate =
        xrphoton::makeRaygenPushConstants(camera, degenerateSun, 17u);
    expect(
        degenerate.sunDirection == glm::vec3{},
        "zero-length sun direction is guarded");
    expect(
        degenerate.sunRadiance == degenerateSun.radiance
            && degenerate.frameIndex == 17u,
        "degenerate direction does not disturb other frame fields");

    const xrphoton::DirectionalSun tinySun{
        .direction = {1.0e-7f, 0.0f, 0.0f},
        .radiance = {1.0f, 1.0f, 1.0f},
    };
    expect(
        xrphoton::makeRaygenPushConstants(camera, tinySun, 0u).sunDirection
            == glm::vec3{},
        "near-zero sun direction is guarded");

    const float quietNaN = std::numeric_limits<float>::quiet_NaN();
    const xrphoton::DirectionalSun nonfiniteSun{
        .direction = {quietNaN, 1.0f, 0.0f},
        .radiance = {1.0f, 1.0f, 1.0f},
    };
    expect(
        xrphoton::makeRaygenPushConstants(camera, nonfiniteSun, 0u).sunDirection
            == glm::vec3{},
        "non-finite sun direction cannot poison the push payload");

    const float maximum = std::numeric_limits<float>::max();
    const xrphoton::DirectionalSun overflowSun{
        .direction = {maximum, maximum, maximum},
        .radiance = {1.0f, 1.0f, 1.0f},
    };
    expect(
        xrphoton::makeRaygenPushConstants(camera, overflowSun, 0u).sunDirection
            == glm::vec3{},
        "overflowed sun length cannot poison the push payload");
}

void testDefaultSun()
{
    const xrphoton::RaygenPushConstants result =
        xrphoton::makeRaygenPushConstants(
            xrphoton::CameraPushConstants{},
            xrphoton::DefaultSun,
            0u);
    expect(
        nearly(glm::length(result.sunDirection), 1.0f)
            && result.sunDirection.y > 0.0f,
        "default sun is a valid above-horizon direction");
    expect(
        result.sunRadiance.x > 0.0f
            && result.sunRadiance.y > 0.0f
            && result.sunRadiance.z > 0.0f,
        "default sun has positive radiance");
}

void testPcgHash()
{
    struct HashVector
    {
        std::uint32_t input;
        std::uint32_t expected;
    };
    constexpr std::array vectors{
        HashVector{0x00000000u, 0x07bb2fe2u},
        HashVector{0x00000001u, 0xa8beea3cu},
        HashVector{0x00000002u, 0x7a7ecc88u},
        HashVector{0x00000003u, 0x7f0ef6bcu},
        HashVector{0x12345678u, 0x995312e1u},
        HashVector{0xffffffffu, 0xe62a4902u},
    };

    for (const HashVector& vector : vectors) {
        expect(
            xrphoton::pcgHash(vector.input) == vector.expected,
            "PCG hash matches a pinned known-answer vector");
    }
}

void testRngSequence()
{
    constexpr std::array expectedStates{
        0x995312e1u,
        0xacc65935u,
        0xd64d9bc8u,
        0x5d39802du,
        0x5918943du,
        0x633434bbu,
    };
    constexpr std::array expectedFloatBits{
        0x3f195312u,
        0x3f2cc659u,
        0x3f564d9bu,
        0x3eba7300u,
        0x3eb23128u,
        0x3ec66868u,
    };

    std::uint32_t state = 0x12345678u;
    for (std::size_t index = 0; index < expectedStates.size(); ++index) {
        const float value = xrphoton::rngNextFloat(state);
        expect(
            state == expectedStates[index],
            "RNG advances to the pinned state");
        expect(
            std::bit_cast<std::uint32_t>(value) == expectedFloatBits[index],
            "RNG float matches the pinned known-answer sequence");
        expect(
            value >= 0.0f && value < 1.0f,
            "RNG float stays in the half-open unit interval");
    }
}
}

static_assert(xrphoton::pcgHash(0u) == 0x07bb2fe2u);
static_assert(xrphoton::pcgHash(0x12345678u) == 0x995312e1u);

int main()
{
    testPayloadLayout();
    testPushConstantConstruction();
    testDefaultSun();
    testPcgHash();
    testRngSequence();

    if (failureCount != 0) {
        std::cerr << failureCount << " lighting test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "lighting tests passed\n";
    return 0;
}
