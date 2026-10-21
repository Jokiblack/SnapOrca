// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <vector>

#include "libslic3r/Slicing.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("Rescaling a layer height profile scales its Z and keeps the layer heights", "[Slicing]")
{
    // 10 mm tall profile: 0.2 at the base, 0.1 at 5 mm, 0.3 at the top. Scaled to 20 mm.
    std::vector<coordf_t> profile = {0., 0.2, 5., 0.1, 10., 0.3};
    REQUIRE(rescale_layer_height_profile(profile, 20.));
    REQUIRE(profile.size() == 6);
    CHECK_THAT(profile[0], WithinAbs(0., 1e-9));
    CHECK_THAT(profile[2], WithinAbs(10., 1e-9));
    CHECK_THAT(profile[4], WithinAbs(20., 1e-9));
    CHECK_THAT(profile[1], WithinAbs(0.2, 1e-9));
    CHECK_THAT(profile[3], WithinAbs(0.1, 1e-9));
    CHECK_THAT(profile[5], WithinAbs(0.3, 1e-9));
}

TEST_CASE("Rescaling a layer height profile leaves a profile it cannot defend intact", "[Slicing]")
{
    // Odd length, a profile that does not start at the base, and a x100 factor.
    std::vector<coordf_t> odd = {0., 0.2, 10.};
    std::vector<coordf_t> not_at_base = {1., 0.2, 10., 0.2};
    std::vector<coordf_t> too_far = {0., 0.2, 10., 0.2};
    const std::vector<coordf_t> odd_before = odd, not_at_base_before = not_at_base, too_far_before = too_far;
    CHECK_FALSE(rescale_layer_height_profile(odd, 20.));
    CHECK_FALSE(rescale_layer_height_profile(not_at_base, 20.));
    CHECK_FALSE(rescale_layer_height_profile(too_far, 1000.));
    CHECK(odd == odd_before);
    CHECK(not_at_base == not_at_base_before);
    CHECK(too_far == too_far_before);
}
