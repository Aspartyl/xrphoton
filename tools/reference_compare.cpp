#include "capture.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>

namespace
{
using Summaries = std::array<
    xrphoton::ReferenceRegionSummary,
    xrphoton::ReferenceRegionCount>;

bool loadSummaries(const char* path, Summaries* summaries)
{
    if (path == nullptr || summaries == nullptr) {
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        std::cerr << "Failed to open reference log: " << path << '\n';
        return false;
    }
    const std::regex pattern(
        R"(^Reference region=([a-z]+) mean=([^,]+),([^,]+),([^ ]+) standardError=([^,]+),([^,]+),([^ ]+)$)");
    std::array<bool, xrphoton::ReferenceRegionCount> found{};
    std::string line;
    while (std::getline(input, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, pattern)) {
            continue;
        }
        for (std::size_t region = 0;
             region < xrphoton::ReferenceRegionCount;
             ++region) {
            if (match[1].str() != xrphoton::referenceRegionName(region)) {
                continue;
            }
            try {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    (*summaries)[region].mean[channel] =
                        std::stod(match[2 + channel].str());
                    (*summaries)[region].standardError[channel] =
                        std::stod(match[5 + channel].str());
                }
            } catch (...) {
                return false;
            }
            found[region] = true;
        }
    }
    for (const bool present : found) {
        if (!present) {
            std::cerr << "Reference log is missing a pinned region: " << path << '\n';
            return false;
        }
    }
    return true;
}
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 4) {
        std::cerr << "Usage: xrPhotonReferenceCompare <mis.log> <nee.log> <bsdf.log>\n";
        return 1;
    }
    std::array<Summaries, 3> estimators{};
    for (std::size_t estimator = 0; estimator < estimators.size(); ++estimator) {
        if (!loadSummaries(arguments[estimator + 1], &estimators[estimator])) {
            return 1;
        }
    }
    constexpr const char* Names[] = {"mis", "nee", "bsdf"};
    for (std::size_t first = 0; first < estimators.size(); ++first) {
        for (std::size_t second = first + 1; second < estimators.size(); ++second) {
            for (std::size_t region = 0;
                 region < xrphoton::ReferenceRegionCount;
                 ++region) {
                if (!xrphoton::referenceEstimatesAgree(
                        estimators[first][region],
                        estimators[second][region])) {
                    std::cerr << "Reference estimator mismatch: "
                              << Names[first] << " vs " << Names[second]
                              << " region="
                              << xrphoton::referenceRegionName(region) << '\n';
                    return 1;
                }
            }
        }
    }
    std::cout << "Reference estimators agree in all pinned regions.\n";
    return 0;
}
