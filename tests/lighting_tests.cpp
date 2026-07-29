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
        sizeof(xrphoton::RaygenPushConstants) == 80,
        "raygen payload has the pinned 80-byte size");
    expect(
        offsetof(xrphoton::RaygenPushConstants, camera) == 0
            && offsetof(xrphoton::RaygenPushConstants, frameIndex) == 64
            && offsetof(xrphoton::RaygenPushConstants, cameraJitterX) == 68
            && offsetof(xrphoton::RaygenPushConstants, cameraJitterY) == 72
            && offsetof(xrphoton::RaygenPushConstants, reserved0) == 76,
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

    constexpr std::uint32_t FrameIndex = 0x89abcdefu;
    const xrphoton::RaygenPushConstants result =
        xrphoton::makeRaygenPushConstants(camera, FrameIndex, 0.25f, -0.125f);

    expect(
        std::memcmp(&result.camera, &camera, sizeof(camera)) == 0,
        "camera payload is copied byte-for-byte");
    expect(
        result.frameIndex == FrameIndex
            && nearly(result.cameraJitterX, 0.25f)
            && nearly(result.cameraJitterY, -0.125f)
            && result.reserved0 == 0,
        "temporal view fields are copied and the reserved word is zero");

    const xrphoton::RaygenPushConstants phaseOne =
        xrphoton::makeRaygenPushConstants(camera, 17u);
    expect(
        phaseOne.frameIndex == 17u
            && phaseOne.cameraJitterX == 0.0f
            && phaseOne.cameraJitterY == 0.0f
            && phaseOne.reserved0 == 0,
        "Phase 1 defaults camera jitter and the reserved word to zero");
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
    testPcgHash();
    testRngSequence();

    if (failureCount != 0) {
        std::cerr << failureCount << " lighting test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "lighting tests passed\n";
    return 0;
}
