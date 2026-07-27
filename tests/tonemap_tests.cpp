#include "tonemap.hpp"

#include <cstddef>
#include <iostream>
#include <string_view>

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
}

int main()
{
    using namespace xrphoton;

    expect(
        sizeof(TonemapState) == sizeof(float)
            && offsetof(TonemapState, exposure) == 0,
        "tonemap push constant has the pinned shader ABI");
    expect(
        DefaultTonemapState.exposure == 1.0f,
        "default exposure remains fixed at one");

    constexpr TonemapDispatch exact = makeTonemapDispatch(1920, 1080);
    expect(exact.x == 240 && exact.y == 135, "exact workgroups are not over-dispatched");

    constexpr TonemapDispatch partial = makeTonemapDispatch(1919, 1079);
    expect(partial.x == 240 && partial.y == 135, "partial edge workgroups round up");

    constexpr TonemapDispatch tiny = makeTonemapDispatch(1, 1);
    expect(tiny.x == 1 && tiny.y == 1, "one pixel dispatches one workgroup");

    constexpr TonemapDispatch empty = makeTonemapDispatch(0, 0);
    expect(empty.x == 0 && empty.y == 0, "empty extent does not dispatch");

    if (failureCount == 0) {
        std::cout << "All tonemap tests passed.\n";
    }
    return failureCount == 0 ? 0 : 1;
}
