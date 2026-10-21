#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include "libslic3r/libslic3r.h"
#include <vector>
#include <cstddef>
#include <string>

using namespace Slic3r;

// A sphere gives well over ExtruderMax original facets, so every extruder state can be assigned
// to a facet of its own without any splitting getting in the way.
static TriangleMesh test_mesh() { return make_sphere(5., 2 * PI / 24); }

// Read the nibble_idx-th 4-bit group of a serialized bitstream, least significant bit first.
static int nibble_at(const std::vector<bool> &bitstream, size_t nibble_idx)
{
    int n = 0;
    for (size_t bit = 0; bit < 4; ++bit)
        n |= int(bitstream[nibble_idx * 4 + bit]) << bit;
    return n;
}

TEST_CASE("Every extruder state survives a serialize/deserialize round trip", "[TriangleSelector]")
{
    const TriangleMesh mesh      = test_mesh();
    const int          max_state = int(EnforcerBlockerType::ExtruderMax);
    REQUIRE(int(mesh.its.indices.size()) >= max_state);

    TriangleSelector selector(mesh);
    for (int state = 1; state <= max_state; ++state)
        selector.set_facet(state - 1, EnforcerBlockerType(state));

    TriangleSelector restored(mesh);
    restored.deserialize(selector.serialize());

    for (int state = 1; state <= max_state; ++state) {
        INFO("Extruder " << state);
        REQUIRE(restored.has_facets(EnforcerBlockerType(state)));
        REQUIRE(restored.num_facets(EnforcerBlockerType(state)) == 1);
    }
}

TEST_CASE("Serialized data reports the extruder states it uses", "[TriangleSelector]")
{
    const TriangleMesh mesh = test_mesh();
    TriangleSelector   selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder16);
    selector.set_facet(1, EnforcerBlockerType::Extruder32);

    const TriangleSelector::TriangleSplittingData data = selector.serialize();

    REQUIRE(data.used_states.size() == size_t(EnforcerBlockerType::ExtruderMax) + 1);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder16)]);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder32)]);
    REQUIRE_FALSE(data.used_states[size_t(EnforcerBlockerType::Extruder17)]);

    SECTION("used_states recomputed from the bitstream agrees") {
        TriangleSelector::TriangleSplittingData recomputed = data;
        recomputed.reset_used_states();
        recomputed.update_used_states(0);
        REQUIRE(recomputed.used_states == data.used_states);
    }

    SECTION("has_facets on the raw data agrees") {
        REQUIRE(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder32));
        REQUIRE_FALSE(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder17));
    }
}

// States 3..17 must keep the pre-existing encoding ("11" prefix plus one nibble of state-3) so
// projects written by older builds stay readable and newly written ones stay readable by them.
TEST_CASE("Extruder states up to 17 keep the single-nibble encoding", "[TriangleSelector]")
{
    const int state = GENERATE(3, 8, 16, 17);

    TriangleSelector selector(test_mesh());
    selector.set_facet(0, EnforcerBlockerType(state));
    const std::vector<bool> bitstream = selector.serialize().bitstream;

    INFO("Extruder " << state);
    // Two nibbles: the "11"-prefixed leaf code, then the state itself.
    REQUIRE(bitstream.size() == 8);
    REQUIRE(nibble_at(bitstream, 0) == 0b1100);
    REQUIRE(nibble_at(bitstream, 1) == state - 3);
}

// States 18 and above set the state nibble to 0b1111 and carry (state-18) in one more nibble.
TEST_CASE("Extruder states above 17 are encoded in a second nibble", "[TriangleSelector]")
{
    const int state = GENERATE(18, 25, 32);

    TriangleSelector selector(test_mesh());
    selector.set_facet(0, EnforcerBlockerType(state));
    const std::vector<bool> bitstream = selector.serialize().bitstream;

    INFO("Extruder " << state);
    REQUIRE(bitstream.size() == 12);
    REQUIRE(nibble_at(bitstream, 0) == 0b1100);
    REQUIRE(nibble_at(bitstream, 1) == 0b1111);
    REQUIRE(nibble_at(bitstream, 2) == state - 18);
}

// Model.cpp writes these hex strings into the 3MF for colored mesh imports; the selector must
// decode exactly the states CONST_FILAMENTS assigns to them.
TEST_CASE("Extruder states match the CONST_FILAMENTS hex encoding", "[TriangleSelector]")
{
    struct Case { const char *hex; int state; };
    const auto c = GENERATE(values<Case>({
        {"8", 2}, {"0C", 3}, {"DC", 16}, {"EC", 17}, {"0FC", 18}, {"EFC", 32},
    }));

    // get_triangle_as_string emits the nibbles most significant first, so read the hex backwards.
    const std::string hex = c.hex;
    std::vector<bool> bitstream;
    for (auto it = hex.rbegin(); it != hex.rend(); ++it) {
        const int nibble = *it >= 'A' ? (*it - 'A' + 10) : (*it - '0');
        for (int bit = 0; bit < 4; ++bit)
            bitstream.push_back((nibble >> bit) & 1);
    }

    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    data.bitstream = bitstream;

    INFO("Hex " << c.hex << " -> extruder " << c.state);
    REQUIRE(TriangleSelector::has_facets(data, EnforcerBlockerType(c.state)));
}

// Pack 4-bit codes into a bitstream, least significant bit first, in the order the decoder reads them.
static std::vector<bool> pack_nibbles(const std::vector<int> &nibbles)
{
    std::vector<bool> bitstream;
    for (const int nibble : nibbles)
        for (int bit = 0; bit < 4; ++bit)
            bitstream.push_back((nibble >> bit) & 1);
    return bitstream;
}

TEST_CASE("A valid paint stream with nested splits round-trips bit for bit", "[TriangleSelector]")
{
    const TriangleMesh mesh = test_mesh();

    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    // A three-side split whose children, in stream order, are: a one-side split (side 2) into two
    // leaves, a two-side split (side 1) into leaves of states 20, 0 and 8, then two plain leaves.
    const std::vector<int> triangle_0 = {0b0011,
                                         0b1001, 0b1000, 0b0100,
                                         0b0110, 0b1100, 0b1111, 20 - 18, 0b0000, 0b1100, 8 - 3,
                                         0b1000,
                                         0b0100};
    data.bitstream = pack_nibbles(triangle_0);
    data.triangles_to_split.emplace_back(5, int(data.bitstream.size()));
    const std::vector<bool> triangle_5 = pack_nibbles({0b1100, 3 - 3});
    data.bitstream.insert(data.bitstream.end(), triangle_5.begin(), triangle_5.end());
    data.reset_used_states();
    REQUIRE(data.update_used_states(0));

    TriangleSelector restored(mesh);
    restored.deserialize(data);

    REQUIRE(restored.num_facets(EnforcerBlockerType::Extruder20) == 1);
    REQUIRE(restored.num_facets(EnforcerBlockerType::Extruder3) == 1);
    REQUIRE(restored.serialize() == data);
}

TEST_CASE("A truncated or malformed paint stream drops only the damaged triangle", "[TriangleSelector][Regression]")
{
    struct Case { const char *name; std::vector<int> nibbles; };
    const auto c = GENERATE(values<Case>({
        {"three-side split missing two children", {0b0011, 0b1000, 0b1000}},
        {"leaf missing its state nibble",         {0b1100}},
        {"leaf missing its second state nibble",  {0b1100, 0b1111}},
        {"splits nested past the end",            {0b0011, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
                                                    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF}},
        {"one-side split of the nonexistent side 3", {0b1101, 0b1000, 0b1000}},
    }));
    INFO(c.name);

    const TriangleMesh mesh = test_mesh();
    TriangleSelector   intact(mesh);
    intact.set_facet(0, EnforcerBlockerType::Extruder2);

    // Triangle 0 stays intact, triangle 1 carries the damaged stream.
    TriangleSelector::TriangleSplittingData data = intact.serialize();
    data.triangles_to_split.emplace_back(1, int(data.bitstream.size()));
    const std::vector<bool> damaged = pack_nibbles(c.nibbles);
    data.bitstream.insert(data.bitstream.end(), damaged.begin(), damaged.end());

    TriangleSelector restored(mesh);
    REQUIRE_NOTHROW(restored.deserialize(data));
    // Triangle 1 unwinds completely, so the selector holds exactly the intact paint.
    REQUIRE(restored.serialize() == intact.serialize());

    REQUIRE_NOTHROW(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder3));

    TriangleSelector::TriangleSplittingData recomputed = data;
    recomputed.reset_used_states();
    REQUIRE_FALSE(recomputed.update_used_states(0));
    REQUIRE(std::none_of(recomputed.used_states.begin(), recomputed.used_states.end(), [](bool used) { return used; }));
}

// Painter Pro Mode (ported from OrcaFS-NeotkoCM by Neotko).
namespace {
// An orthographic camera on +Z looking down: world x and y in [-10, 10] fill a 200 x 200 pixel viewport.
Matrix4d top_view_projection()
{
    Matrix4d m = Matrix4d::Identity();
    m(0, 0)    = 0.1;
    m(1, 1)    = 0.1;
    m(2, 2)    = -0.01;
    return m;
}
const std::array<int, 4> top_viewport = {0, 0, 200, 200};

// Area of the facets painted with `state` whose normal points along `dir`.
double painted_area_facing(const TriangleSelector &selector, EnforcerBlockerType state, const Vec3f &dir)
{
    const indexed_triangle_set its = selector.get_facets(state);
    double                     out = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3f n = (its.vertices[f(1)] - its.vertices[f(0)]).cross(its.vertices[f(2)] - its.vertices[f(0)]);
        if (n.normalized().dot(dir) > 0.99f)
            out += 0.5 * n.norm();
    }
    return out;
}
} // namespace

TEST_CASE("Painter Pro rectangle mask paints only the facets facing the camera", "[TriangleSelector][PainterPro]")
{
    TriangleMesh mesh = make_cube(4., 4., 4.);
    mesh.translate(-2.f, -2.f, -2.f);
    TriangleSelector selector(mesh);
    // Pixels 50..150 are world -5..5, around the whole cube.
    selector.select_patch(0,
                          TriangleSelector::RectangleProjectionCursor::cursor_factory(Vec3f(0.f, 0.f, 10.f), Vec3f(0.f, 0.f, 9.f), top_view_projection(),
                                                                                      top_viewport, Vec2f(50.f, 50.f), Vec2f(150.f, 150.f),
                                                                                      Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::Extruder2, Transform3d::Identity(), true);
    CHECK(painted_area_facing(selector, EnforcerBlockerType::Extruder2, Vec3f::UnitZ()) == Catch::Approx(16.));
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder2) == int(selector.get_facets(EnforcerBlockerType::Extruder2).indices.size()));
    CHECK(painted_area_facing(selector, EnforcerBlockerType::Extruder2, -Vec3f::UnitZ()) == Catch::Approx(0.));
    CHECK(painted_area_facing(selector, EnforcerBlockerType::Extruder2, Vec3f::UnitX()) == Catch::Approx(0.));
}

TEST_CASE("Painter Pro polygon mask follows a concave outline", "[TriangleSelector][PainterPro]")
{
    // A 16 x 16 mm top face and an L-shaped polygon covering three quarters of it: the quarter at +x +y stays unpainted.
    TriangleMesh mesh = make_cube(16., 16., 2.);
    mesh.translate(-8.f, -8.f, -1.f);
    TriangleSelector selector(mesh);
    // World (x, y) maps to pixel (100 + 10 x, 100 - 10 y).
    auto px = [](float x, float y) { return Vec2f(100.f + 10.f * x, 100.f - 10.f * y); };
    std::vector<Vec2f> polygon = {px(-9.f, -9.f), px(9.f, -9.f), px(9.f, 0.f), px(0.f, 0.f), px(0.f, 9.f), px(-9.f, 9.f)};
    selector.select_patch(0,
                          TriangleSelector::PolygonProjectionCursor::cursor_factory(Vec3f(0.f, 0.f, 10.f), Vec3f(0.f, 0.f, 9.f), top_view_projection(),
                                                                                    top_viewport, polygon, Transform3d::Identity(),
                                                                                    TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::Extruder3, Transform3d::Identity(), true);
    // Three quarters of 256 mm², up to the triangles cut along the inner corner.
    CHECK(painted_area_facing(selector, EnforcerBlockerType::Extruder3, Vec3f::UnitZ()) == Catch::Approx(192.).margin(1.));

    SECTION("a notch inside a facet whose three vertices are all inside the polygon is left out") {
        // The whole face, but for a V notch from the -y edge up to (-4, -4): 3.2 mm² of it lies on the face.
        TriangleSelector notched(mesh);
        std::vector<Vec2f> square_with_notch = {px(-9.f, -9.f), px(-5.f, -9.f), px(-4.f, -4.f), px(-3.f, -9.f), px(9.f, -9.f), px(9.f, 9.f), px(-9.f, 9.f)};
        notched.select_patch(0,
                             TriangleSelector::PolygonProjectionCursor::cursor_factory(Vec3f(0.f, 0.f, 10.f), Vec3f(0.f, 0.f, 9.f), top_view_projection(),
                                                                                       top_viewport, square_with_notch, Transform3d::Identity(),
                                                                                       TriangleSelector::ClippingPlane()),
                             EnforcerBlockerType::Extruder3, Transform3d::Identity(), true);
        CHECK(painted_area_facing(notched, EnforcerBlockerType::Extruder3, Vec3f::UnitZ()) == Catch::Approx(256. - 3.2).margin(0.8));
    }
}

TEST_CASE("Painter Pro brush precision 1x paints as before and a finer one splits more", "[TriangleSelector][PainterPro]")
{
    const TriangleMesh mesh = test_mesh();
    auto paint = [&mesh](float precision, bool set_precision) {
        TriangleSelector selector(mesh);
        if (set_precision)
            selector.set_precision_factor(precision);
        const Vec3f hit = mesh.its.vertices[mesh.its.indices[0](0)];
        selector.select_patch(0,
                              TriangleSelector::SinglePointCursor::cursor_factory(hit, hit * 3.f, 1.5f, TriangleSelector::CIRCLE, Transform3d::Identity(),
                                                                                  TriangleSelector::ClippingPlane()),
                              EnforcerBlockerType::Extruder1, Transform3d::Identity(), true);
        return selector.serialize();
    };
    const TriangleSelector::TriangleSplittingData stock = paint(1.f, false);
    const TriangleSelector::TriangleSplittingData same  = paint(1.f, true);
    const TriangleSelector::TriangleSplittingData finer = paint(4.f, true);
    REQUIRE(!stock.triangles_to_split.empty());
    CHECK(same.bitstream == stock.bitstream);
    CHECK(same.triangles_to_split.size() == stock.triangles_to_split.size());
    CHECK(finer.bitstream.size() > stock.bitstream.size());
}
