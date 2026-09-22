#include "vector_integer.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
// Same members as XenonRecomp's PPCVRegister.
union alignas(16) Vector {
    int8_t s8[16];
    uint8_t u8[16];
    int16_t s16[8];
    uint16_t u16[8];
    int32_t s32[4];
    uint32_t u32[4];
    int64_t s64[2];
    uint64_t u64[2];
    float f32[4];
};
Vector floats(float a, float b, float c, float d) {
    Vector v{};
    v.f32[0] = a; v.f32[1] = b; v.f32[2] = c; v.f32[3] = d;
    return v;
}
struct CR { uint8_t lt, gt, eq, so; };

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

Vector halves(std::initializer_list<int> values) {
    Vector v{};
    int i = 0;
    for (int value : values) v.u16[i++] = static_cast<uint16_t>(value);
    return v;
}
Vector words(std::initializer_list<long long> values) {
    Vector v{};
    int i = 0;
    for (long long value : values) v.u32[i++] = static_cast<uint32_t>(value);
    return v;
}
Vector bytes(int fill) {
    Vector v{};
    std::memset(v.u8, fill, 16);
    return v;
}
bool same(const Vector& a, const Vector& b) { return !std::memcmp(&a, &b, sizeof a); }

void run() {
    using namespace sfr::vmx;
    Vector d{};
    // Shift counts use only the low four bits: 17 & 15 == 1.
    vslh(d, halves({1, 0x8001, 0xFFFF, 3, 5, 7, 9, 11}), halves({1, 1, 15, 0, 17, 16, 2, 4}));
    require(same(d, halves({2, 2, 0x8000, 3, 10, 7, 36, 176})), "vslh");
    vsrh(d, halves({0x8000, 0xFFFF, 4, 4, 4, 4, 4, 4}), halves({15, 4, 1, 2, 18, 0, 16, 3}));
    require(same(d, halves({1, 0x0FFF, 2, 1, 1, 4, 4, 0})), "vsrh");
    vsrah(d, halves({0x8000, 0xFFFF, 0x7FFF, -4, 4, 4, 4, 4}), halves({15, 4, 14, 1, 1, 1, 1, 1}));
    require(same(d, halves({0xFFFF, 0xFFFF, 1, 0xFFFE, 2, 2, 2, 2})), "vsrah is arithmetic");
    vrlh(d, halves({0x8001, 0x1234, 0x1234, 0, 0, 0, 0, 0}), halves({1, 4, 16, 0, 0, 0, 0, 0}));
    require(same(d, halves({0x0003, 0x2341, 0x1234, 0, 0, 0, 0, 0})), "vrlh wraps and treats 16 as 0");
    Vector a = bytes(0x80), b = bytes(9);  // -128 >> (9 & 7) == -128 >> 1
    vsrab(d, a, b);
    require(same(d, bytes(0xC0)), "vsrab");

    vsubshs(d, halves({-32768, 32767, 5, 0, 0, 0, 0, 0}), halves({1, -1, 7, 0, 0, 0, 0, 0}));
    require(same(d, halves({-32768, 32767, -2, 0, 0, 0, 0, 0})), "vsubshs saturates both ways");
    vadduhs(d, halves({0xFFFF, 1, 0, 0, 0, 0, 0, 0}), halves({1, 2, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({0xFFFF, 3, 0, 0, 0, 0, 0, 0})), "vadduhs");
    vsubuhs(d, halves({1, 5, 0, 0, 0, 0, 0, 0}), halves({2, 3, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({0, 2, 0, 0, 0, 0, 0, 0})), "vsubuhs clamps at zero");
    vaddsbs(d, bytes(0x7F), bytes(0x01));
    require(same(d, bytes(0x7F)), "vaddsbs");
    vaddsws(d, words({0x7FFFFFFF, -0x80000000LL, 1, -1}), words({1, -1, 2, -2}));
    require(same(d, words({0x7FFFFFFF, -0x80000000LL, 3, -3})), "vaddsws");
    vsububm(d, bytes(0), bytes(1));
    require(same(d, bytes(0xFF)), "vsububm wraps");

    vmaxsh(d, halves({-1, 2, 0, 0, 0, 0, 0, 0}), halves({1, -2, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({1, 2, 0, 0, 0, 0, 0, 0})), "vmaxsh is signed");
    vmaxuh(d, halves({0xFFFF, 2, 0, 0, 0, 0, 0, 0}), halves({1, 3, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({0xFFFF, 3, 0, 0, 0, 0, 0, 0})), "vmaxuh is unsigned");
    vminsh(d, halves({-1, 2, 0, 0, 0, 0, 0, 0}), halves({1, -2, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({-1, -2, 0, 0, 0, 0, 0, 0})), "vminsh");
    vminuh(d, halves({0xFFFF, 2, 0, 0, 0, 0, 0, 0}), halves({1, 3, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({1, 2, 0, 0, 0, 0, 0, 0})), "vminuh");
    vmaxuw(d, words({0xFFFFFFFF, 1, 0, 0}), words({2, 3, 0, 0}));
    require(same(d, words({0xFFFFFFFF, 3, 0, 0})), "vmaxuw");
    vminuw(d, words({0xFFFFFFFF, 1, 0, 0}), words({2, 3, 0, 0}));
    require(same(d, words({2, 1, 0, 0})), "vminuw");
    vavguh(d, halves({0xFFFF, 1, 2, 0, 0, 0, 0, 0}), halves({0xFFFF, 2, 2, 0, 0, 0, 0, 0}));
    require(same(d, halves({0xFFFF, 2, 2, 0, 0, 0, 0, 0})), "vavguh rounds up without overflow");

    // Packs: host lanes are reversed, so vB fills the low host lanes.
    vpkswss(d, words({1, 70000, -70000, -5}), words({2, 3, 4, 5}));
    require(same(d, halves({2, 3, 4, 5, 1, 32767, -32768, -5})), "vpkswss saturates, vB in low host lanes");
    vpkswus(d, words({-1, 70000, 7, 8}), words({1, 2, 3, 4}));
    require(same(d, halves({1, 2, 3, 4, 0, 0xFFFF, 7, 8})), "vpkswus clamps to unsigned");
    vpkuwum(d, words({0x12345678, 1, 2, 3}), words({0xABCD0001, 4, 5, 6}));
    require(same(d, halves({1, 4, 5, 6, 0x5678, 1, 2, 3})), "vpkuwum truncates");
    vpkuwus(d, words({0x10000, 1, 2, 3}), words({0xFFFFFFFF, 4, 5, 6}));
    require(same(d, halves({0xFFFF, 4, 5, 6, 0xFFFF, 1, 2, 3})), "vpkuwus saturates unsigned");
    vpkuhus(d, halves({0x100, 1, 2, 3, 4, 5, 6, 7}), halves({0xFFFF, 9, 0, 0, 0, 0, 0, 0}));
    require(d.u8[0] == 0xFF && d.u8[1] == 9 && d.u8[8] == 0xFF && d.u8[9] == 1 && d.u8[15] == 7, "vpkuhus");
    vpkshss(d, halves({-200, 200, 5, 0, 0, 0, 0, 0}), halves({-3, 0, 0, 0, 0, 0, 0, 0}));
    require(d.s8[0] == -3 && d.s8[8] == -128 && d.s8[9] == 127 && d.s8[10] == 5, "vpkshss");
    // vslo by 2 octets: the guest's first bytes are the host's last lanes.
    Vector source{};
    for (int i = 0; i < 16; ++i) source.u8[i] = uint8_t(i + 1);
    Vector count{};
    count.u8[0] = 2 << 3;
    vslo(d, source, count);
    require(d.u8[0] == 0 && d.u8[1] == 0 && d.u8[2] == 1 && d.u8[15] == 14, "vslo shifts toward guest byte 0");

    vcmpgtsh(d, halves({1, -1, 0, 0, 0, 0, 0, 0}), halves({-1, 1, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({0xFFFF, 0, 0, 0, 0, 0, 0, 0})), "vcmpgtsh is signed");
    vcmpequh(d, halves({1, 2, 0, 0, 0, 0, 0, 0}), halves({1, 3, 0, 0, 0, 0, 0, 0}));
    require(same(d, halves({0xFFFF, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF})), "vcmpequh");
    vcmpgtuw(d, words({0xFFFFFFFF, 1, 0, 0}), words({1, 2, 0, 0}));
    require(same(d, words({0xFFFFFFFF, 0, 0, 0})), "vcmpgtuw is unsigned");
    vcmpgtsw(d, words({-1, 2, 0, 0}), words({1, 1, 0, 0}));
    require(same(d, words({0, 0xFFFFFFFF, 0, 0})), "vcmpgtsw is signed");

    CR cr{9, 9, 9, 9};
    sfr::vmx::set_compare_cr6(cr, bytes(0xFF));
    require(cr.lt == 1 && cr.gt == 0 && cr.eq == 0 && cr.so == 0, "all true sets LT");
    sfr::vmx::set_compare_cr6(cr, bytes(0));
    require(cr.lt == 0 && cr.eq == 1, "none true sets EQ");
    sfr::vmx::set_compare_cr6(cr, halves({0xFFFF, 0, 0, 0, 0, 0, 0, 0}));
    require(cr.lt == 0 && cr.eq == 0, "mixed sets neither");

    vcfpuxws(d, floats(1.75f, -3.0f, 4294967296.0f, std::numeric_limits<float>::quiet_NaN()), 0);
    require(same(d, words({1, 0, 0xFFFFFFFF, 0})), "vcfpuxws truncates, clamps negatives, saturates, NaN is 0");
    vcfpuxws(d, floats(0.5f, 1.0f, 1e-40f, -0.0f), 8);
    require(same(d, words({128, 256, 0, 0})), "vcfpuxws scale; denormal and -0 give 0");
    vcfpuxws(d, floats(2.0f, 0.99999994f, 65535.9f, 1.0f), 31);
    // (1 - 2^-24) * 2^31 = 2^31 - 2^7.
    require(same(d, words({0xFFFFFFFF, 0x7FFFFF80, 0xFFFFFFFF, 0x80000000})), "vcfpuxws scale 31 saturation");

    vspltish(d, -16);
    require(same(d, halves({-16, -16, -16, -16, -16, -16, -16, -16})), "vspltish");
    Vector c = words({0xFFFF0000, 0, 0xFFFFFFFF, 0x0F0F0F0F});
    vsel(d, words({0x11111111, 0x22222222, 0x33333333, 0x44444444}),
         words({0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD}), c);
    require(same(d, words({0xAAAA1111, 0x22222222, 0xCCCCCCCC, 0x4D4D4D4D})), "vsel");

    // The destination may alias a source.
    Vector x = halves({1, 2, 3, 4, 5, 6, 7, 8});
    vslh(x, x, halves({1, 1, 1, 1, 1, 1, 1, 1}));
    require(same(x, halves({2, 4, 6, 8, 10, 12, 14, 16})), "aliasing destination");
}
}

int main() {
    try {
        run();
        std::cout << "vector integer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
