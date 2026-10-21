// Height Adaptive Effects curve (ported from OrcaFS-NeotkoCM by Neotko, AGPL-3.0).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Feature/HeightAdaptive/HeightCurve.hpp"

#include <string>

using namespace Slic3r::HeightAdaptive;
using Catch::Matchers::WithinAbs;

TEST_CASE("An empty or malformed height curve returns the fallback untouched", "[HeightCurve]")
{
    for (const char *blob : { "", "   ", "1:2:3:4", "a:1|2:3", "1:|2:3", "smooth|1:x" }) {
        const HeightCurve curve = HeightCurve::parse(blob, Interp::Smooth);
        CHECK(curve.empty());
        CHECK_THAT(curve.at(3.7, 0.4237), WithinAbs(0.4237, 0.)); // verbatim
    }
}

TEST_CASE("A stepped height curve holds the last node at or below z and clamps outside", "[HeightCurve]")
{
    const HeightCurve curve = HeightCurve::parse("stepped|0:0.3|5:0.5|10:0.4", Interp::Smooth);
    REQUIRE(curve.interp() == Interp::Stepped);
    CHECK_THAT(curve.at(-1., 0.), WithinAbs(0.3, 1e-12));
    CHECK_THAT(curve.at(4.99, 0.), WithinAbs(0.3, 1e-12));
    CHECK_THAT(curve.at(5.01, 0.), WithinAbs(0.5, 1e-12));
    CHECK_THAT(curve.at(12., 0.), WithinAbs(0.4, 1e-12));
}

TEST_CASE("A smooth height curve with zero tension is linear and never overshoots", "[HeightCurve]")
{
    // No mode token: the fallback mode applies.
    CHECK(HeightCurve::parse("0:0|10:1", Interp::Stepped).interp() == Interp::Stepped);
    const HeightCurve linear = HeightCurve::parse("0:0:0|10:1:0", Interp::Smooth);
    REQUIRE(linear.interp() == Interp::Smooth);
    CHECK_THAT(linear.at(2.5, 0.), WithinAbs(0.25, 1e-12));

    const HeightCurve bump = HeightCurve::parse("smooth|0:0:1|5:1:1|10:0:1", Interp::Smooth);
    for (double z = 0.; z <= 10.; z += 0.25) {
        const double v = bump.at(z, 0.);
        CHECK(v >= -1e-12);
        CHECK(v <= 1. + 1e-12);
    }
}

TEST_CASE("A height curve round-trips through serialize", "[HeightCurve]")
{
    HeightCurve curve(Interp::Smooth);
    curve.set_nodes({ { 123.45, 0.4537, 0.25 }, { 0.2, -0.1, 0.5 }, { 0.2, 0.2, 0.5 } });
    REQUIRE(curve.nodes().size() == 2); // sorted, and the later of two nodes at the same z wins
    const std::string blob = curve.serialize();
    CHECK(blob == "smooth|0.2:0.2:0.5|123.45:0.4537:0.25");
    const HeightCurve back = HeightCurve::parse(blob, Interp::Stepped);
    REQUIRE(back.nodes().size() == 2);
    CHECK(back.interp() == Interp::Smooth);
    CHECK_THAT(back.nodes()[1].z, WithinAbs(123.45, 1e-9));
}
