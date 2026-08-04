#include <string/core/tonemap.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>

namespace string::core::tonemap
{
namespace
{

// --- Colour spaces ------------------------------------------------------------------------------

// Rec.709 (D65) <-> XYZ.
const glm::mat3 kRec709ToXyz = glm::mat3(   // column-major constructor: columns are basis vectors
    0.4123908f, 0.2126390f, 0.0193308f,     // column 0 (contribution of R)
    0.3575843f, 0.7151687f, 0.1191948f,
    0.1804808f, 0.0721923f, 0.9505322f);
const glm::mat3 kXyzToRec709 = glm::inverse(kRec709ToXyz);

// CAT16 (used by the Hellwig2022 / CIECAM16 appearance model).
const glm::mat3 kCat16 = glm::transpose(glm::mat3(
    0.401288f, 0.650173f, -0.051461f,
    -0.250268f, 1.204414f, 0.045854f,
    -0.002079f, 0.048952f, 0.953127f));
const glm::mat3 kCat16Inv = glm::inverse(kCat16);

// --- Hellwig2022/CIECAM16-style appearance model, ACES-2.0 reference viewing conditions ----------
// White D65 (Y = 100), adapting luminance L_A = 100 cd/m^2, background Y_b = 20, dim surround.
// Everything below is derived once; forward/inverse are exact inverses of each other, which is
// the property the DRT relies on (tonescale + compression happen in JMh, then invert out).
struct Cam
{
    glm::vec3 d_rgb;       // von Kries adaptation scales in CAT16 LMS
    float fl = 0.0f;       // luminance adaptation factor
    float nbb = 0.0f;
    float c = 0.59f;       // dim surround impact
    float nc = 0.9f;
    float z = 0.0f;
    float aw = 0.0f;       // achromatic response of the adapted white
    glm::mat3 ab_to_rgba;  // inverse of the {A/Nbb-row, a-row, b-row} opponent matrix

    Cam()
    {
        const glm::vec3 xyz_w = kRec709ToXyz * glm::vec3(1.0f) * 100.0f;   // Rec.709 white, Y=100
        const float la = 100.0f, yb = 20.0f, f = 0.9f;
        const glm::vec3 rgb_w = kCat16 * xyz_w;
        const float d = std::clamp(f * (1.0f - (1.0f / 3.6f) * std::exp((-la - 42.0f) / 92.0f)),
                                   0.0f, 1.0f);
        d_rgb = glm::vec3(d * xyz_w.y / rgb_w.x + 1.0f - d,
                          d * xyz_w.y / rgb_w.y + 1.0f - d,
                          d * xyz_w.y / rgb_w.z + 1.0f - d);
        const float k = 1.0f / (5.0f * la + 1.0f);
        const float k4 = k * k * k * k;
        fl = 0.2f * k4 * (5.0f * la)
           + 0.1f * (1.0f - k4) * (1.0f - k4) * std::cbrt(5.0f * la);
        const float n = yb / xyz_w.y;
        z = 1.48f + std::sqrt(n);
        nbb = 0.725f * std::pow(n, -0.2f);
        const glm::vec3 rgb_aw = adapt(d_rgb * rgb_w);
        aw = (2.0f * rgb_aw.x + rgb_aw.y + 0.05f * rgb_aw.z - 0.305f) * nbb;
        // Opponent decomposition p = 2R+G+0.05B, a = R - 12G/11 + B/11, b = (R+G-2B)/9.
        const glm::mat3 rgba_to_ab = glm::transpose(glm::mat3(
            2.0f, 1.0f, 0.05f,
            1.0f, -12.0f / 11.0f, 1.0f / 11.0f,
            1.0f / 9.0f, 1.0f / 9.0f, -2.0f / 9.0f));
        ab_to_rgba = glm::inverse(rgba_to_ab);
    }

    static glm::vec3 adapt(glm::vec3 v)
    {
        const auto one = [](float x) {
            const float t = std::pow(std::abs(x) / 100.0f, 0.42f);
            return std::copysign(400.0f * t / (t + 27.13f), x);
        };
        return { one(v.x), one(v.y), one(v.z) };
    }
    static glm::vec3 unadapt(glm::vec3 v)
    {
        const auto one = [](float x) {
            const float a = std::min(std::abs(x), 399.9f);
            return std::copysign(100.0f * std::pow(27.13f * a / (400.0f - a), 1.0f / 0.42f), x);
        };
        return { one(v.x), one(v.y), one(v.z) };
    }
    // fl is folded into adapt's |x|/100 by pre-scaling the input.
    glm::vec3 adapt_fl(glm::vec3 v) const { return adapt(v * fl); }
    glm::vec3 unadapt_fl(glm::vec3 v) const { return unadapt(v) / fl; }

    static float eccentricity(float h)
    {
        return 0.25f * (std::cos(h + 2.0f) + 3.8f);
    }

    // XYZ (relative, white Y = 100) -> J (lightness 0..100), M (colourfulness), h (radians).
    glm::vec3 to_jmh(glm::vec3 xyz) const
    {
        const glm::vec3 rgb_a = adapt_fl(d_rgb * (kCat16 * xyz));
        const float a = rgb_a.x - 12.0f * rgb_a.y / 11.0f + rgb_a.z / 11.0f;
        const float b = (rgb_a.x + rgb_a.y - 2.0f * rgb_a.z) / 9.0f;
        const float h = std::atan2(b, a);
        const float big_a = (2.0f * rgb_a.x + rgb_a.y + 0.05f * rgb_a.z - 0.305f) * nbb;
        const float j = 100.0f * std::pow(std::max(big_a, 0.0f) / aw, c * z);
        const float m = 43.0f * nc * eccentricity(h) * std::sqrt(a * a + b * b);
        return { j, m, h };
    }

    glm::vec3 from_jmh(glm::vec3 jmh) const
    {
        const float j = std::max(jmh.x, 0.0f), m = std::max(jmh.y, 0.0f), h = jmh.z;
        const float big_a = aw * std::pow(j / 100.0f, 1.0f / (c * z));
        const float mag = m / std::max(43.0f * nc * eccentricity(h), 1e-6f);
        const glm::vec3 pab(big_a / nbb + 0.305f, mag * std::cos(h), mag * std::sin(h));
        const glm::vec3 rgb_a = ab_to_rgba * pab;
        return kCat16Inv * (unadapt_fl(rgb_a) / d_rgb);
    }
};

const Cam& cam()
{
    static const Cam c;
    return c;
}

// --- ACES 2.0 tonescale (Daniele Evo), SDR 100-nit parameterization ------------------------------
struct Tonescale
{
    float m2 = 0.0f, s2 = 0.0f, g = 1.15f, fl = 0.01f, n = 100.0f, nr = 100.0f;
    Tonescale()
    {
        const float c = 0.18f, cd = 10.013f, wg = 0.14f, t1 = 0.041f;
        const float rhm = 128.0f, rhx = 896.0f;
        const float m0 = n / nr;
        const float m1 = 0.5f * (m0 + std::sqrt(m0 * (m0 + 4.0f * t1)));
        const float r_hit = rhm + (rhx - rhm) * (std::log(m0) / std::log(10000.0f / 100.0f));
        const float u = std::pow((r_hit / m1) / (r_hit / m1 + 1.0f), g);
        const float m = m1 / u;
        const float wi = std::log(n / 100.0f) / std::log(2.0f);
        const float ct = cd / nr * (1.0f + wi * wg);
        const float g_ip = 0.5f * (ct + std::sqrt(ct * (ct + 4.0f * t1)));
        const float g_ipp2 = -(m1 * std::pow(g_ip / m, 1.0f / g))
                           / (std::pow(g_ip / m, 1.0f / g) - 1.0f);
        const float w2 = c / g_ipp2;
        s2 = w2 * m1;
        const float u2 = std::pow((r_hit / m1) / (r_hit / m1 + w2), g);
        m2 = m1 / u2;
    }
    // Scene-linear (0.18 = mid grey) -> display luminance in n_r units (1.0 = 100 nits).
    float fwd(float x) const
    {
        if (x <= 0.0f) return 0.0f;
        const float f = m2 * std::pow(x / (x + s2), g);
        return std::max(0.0f, f * f / (f + fl));
    }
};

const Tonescale& tonescale()
{
    static const Tonescale t;
    return t;
}

// Monotone J <-> scene/display luminance through the CAM's achromatic axis, tabulated once (the
// closed form exists but a table keeps J/Y exactly consistent with to_jmh/from_jmh above).
struct JTable
{
    static constexpr int kN = 2048;
    std::array<float, kN> j_of_index{};   // index i -> J at Y = y_of(i)
    static float y_of(int i)
    {
        // log spacing over Y in [1e-5, 400] (relative luminance, white = 100)
        const float t = float(i) / float(kN - 1);
        return std::exp(std::lerp(std::log(1e-5f), std::log(400.0f), t));
    }
    JTable()
    {
        const glm::vec3 white = kRec709ToXyz * glm::vec3(1.0f);
        for (int i = 0; i < kN; ++i)
            j_of_index[size_t(i)] = cam().to_jmh(white * y_of(i)).x;
    }
    float j_from_y(float y) const
    {
        y = std::clamp(y, 1e-5f, 400.0f);
        const float t = (std::log(y) - std::log(1e-5f)) / (std::log(400.0f) - std::log(1e-5f));
        const float fi = t * float(kN - 1);
        const int i = std::min(int(fi), kN - 2);
        return std::lerp(j_of_index[size_t(i)], j_of_index[size_t(i) + 1], fi - float(i));
    }
    float y_from_j(float j) const
    {
        // binary search the monotone table
        int lo = 0, hi = kN - 1;
        j = std::clamp(j, j_of_index[0], j_of_index[kN - 1]);
        while (hi - lo > 1)
        {
            const int mid = (lo + hi) / 2;
            (j_of_index[size_t(mid)] <= j ? lo : hi) = mid;
        }
        const float j0 = j_of_index[size_t(lo)], j1 = j_of_index[size_t(hi)];
        const float t = j1 > j0 ? (j - j0) / (j1 - j0) : 0.0f;
        return std::lerp(y_of(lo), y_of(hi), t);
    }
};

const JTable& jtable()
{
    static const JTable t;
    return t;
}

// --- The ACES-2.0-style CAM DRT -------------------------------------------------------------------
glm::vec3 drt_aces2(glm::vec3 lin)
{
    lin = glm::max(lin, glm::vec3(0.0f));
    const glm::vec3 xyz = kRec709ToXyz * lin * 100.0f;
    const glm::vec3 jmh = cam().to_jmh(xyz);

    // Tonescale the lightness through the achromatic axis: J -> scene Y -> Daniele Evo -> J_ts.
    const float y_scene = jtable().y_from_j(jmh.x) / 100.0f;      // 0.18 = mid grey
    const float y_ts = tonescale().fwd(y_scene) * 100.0f;         // display-relative, white = 100
    const float j_ts = jtable().j_from_y(y_ts);

    // Chroma compression: colourfulness tracks the lightness rescale (hue-preserving; per-channel
    // curves are exactly what caused the 1.x red->orange / blue->purple skews). Approximation of
    // the reference's chroma compression — flagged in the brief log.
    const float m_c = jmh.y * std::pow(j_ts / std::max(jmh.x, 1e-3f), 0.9f);

    // Gamut map to Rec.709: largest in-gamut colourfulness along constant (J_ts, h) — bisection.
    const auto display_rgb = [&](float m) {
        return kXyzToRec709 * cam().from_jmh({ j_ts, m, jmh.z }) / 100.0f;
    };
    const auto in_gamut = [](glm::vec3 c) {
        const float eps = 1e-3f;
        return c.x >= -eps && c.y >= -eps && c.z >= -eps
            && c.x <= 1.0f + eps && c.y <= 1.0f + eps && c.z <= 1.0f + eps;
    };
    glm::vec3 out = display_rgb(m_c);
    if (!in_gamut(out))
    {
        float lo = 0.0f, hi = m_c;
        for (int i = 0; i < 16; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            (in_gamut(display_rgb(mid)) ? lo : hi) = mid;
        }
        out = display_rgb(lo);
    }
    return glm::clamp(out, glm::vec3(0.0f), glm::vec3(1.0f));
}

glm::vec3 drt_aces1(glm::vec3 x)
{
    // Narkowicz 2015 fitted curve — byte-matches the pre-09 composite/capture path.
    const auto one = [](float v) {
        return std::clamp((v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f),
                          0.0f, 1.0f);
    };
    return { one(x.x), one(x.y), one(x.z) };
}

// --- Grading (working space, pre-transform) ------------------------------------------------------
float shaper_encode_1(float c)
{
    const float l = std::log2(std::max(c, std::exp2(kLog2Min)));
    return std::clamp((l - kLog2Min) / (kLog2Max - kLog2Min), 0.0f, 1.0f);
}
float shaper_decode_1(float e)
{
    return std::exp2(e * (kLog2Max - kLog2Min) + kLog2Min);
}

glm::vec3 apply_grading(const Grading& g, glm::vec3 c)
{
    if (g.exposure_stops != 0.0f) c *= std::exp2(g.exposure_stops);
    if (g.temperature != 0.0f || g.tint != 0.0f)
    {
        // von Kries in CAT16 LMS; luma-renormalized so white balance doesn't change exposure.
        const glm::vec3 luma_w(0.2126f, 0.7152f, 0.0722f);
        const float y0 = std::max(glm::dot(c, luma_w), 1e-8f);
        glm::vec3 lms = kCat16 * (kRec709ToXyz * c);
        lms *= glm::vec3(1.0f + 0.15f * g.temperature,
                         1.0f + 0.15f * g.tint,
                         1.0f - 0.15f * g.temperature);
        c = kXyzToRec709 * (kCat16Inv * lms);
        const float y1 = std::max(glm::dot(c, luma_w), 1e-8f);
        c *= y0 / y1;
    }
    if (g.contrast != 1.0f)
    {
        // log contrast around the 0.18 pivot, per channel
        const auto one = [&](float v) {
            return v <= 0.0f ? 0.0f : 0.18f * std::pow(v / 0.18f, g.contrast);
        };
        c = { one(c.x), one(c.y), one(c.z) };
    }
    if (g.saturation != 1.0f)
    {
        const float y = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        c = glm::max(glm::vec3(y) + (c - glm::vec3(y)) * g.saturation, glm::vec3(0.0f));
    }
    if (g.lift != 0.0f || g.gamma != 1.0f || g.gain != 1.0f)
    {
        // shaper (log) space lift/gamma/gain — range-sane over the HDR domain; neutral == skip
        // (the encode clamps sub-2^-12 values, so the block must not run on a neutral grade).
        const auto one = [&](float v) {
            float e = shaper_encode_1(v);
            e = std::clamp(e * g.gain + g.lift * (1.0f - e), 0.0f, 1.0f);
            e = std::pow(e, 1.0f / std::max(g.gamma, 1e-3f));
            return shaper_decode_1(e);
        };
        c = { one(c.x), one(c.y), one(c.z) };
    }
    return c;
}

}  // namespace

glm::vec3 shaper_encode(glm::vec3 c)
{
    return { shaper_encode_1(c.x), shaper_encode_1(c.y), shaper_encode_1(c.z) };
}

glm::vec3 transform(Curve curve, const Grading& grading, glm::vec3 exposed)
{
    const glm::vec3 graded = apply_grading(grading, glm::max(exposed, glm::vec3(0.0f)));
    return curve == Curve::Aces2 ? drt_aces2(graded) : drt_aces1(graded);
}

std::vector<float> bake_lut(Curve curve, const Grading& grading, uint32_t size)
{
    // Warm the CAM/tonescale/J tables before fanning out (their lazy statics must not race).
    (void)cam();
    (void)tonescale();
    (void)jtable();
    std::vector<float> lut(size_t(size) * size * size * 3);
    const auto bake_slice = [&](uint32_t b) {
        for (uint32_t g = 0; g < size; ++g)
            for (uint32_t r = 0; r < size; ++r)
            {
                const glm::vec3 enc(float(r) / float(size - 1),
                                    float(g) / float(size - 1),
                                    float(b) / float(size - 1));
                const glm::vec3 lin(shaper_decode_1(enc.x), shaper_decode_1(enc.y),
                                    shaper_decode_1(enc.z));
                const glm::vec3 out = transform(curve, grading, lin);
                // 2D strip layout: x = b*size + r, y = g (width size*size, height size)
                const size_t idx = (size_t(g) * size * size + size_t(b) * size + r) * 3;
                lut[idx + 0] = out.x;
                lut[idx + 1] = out.y;
                lut[idx + 2] = out.z;
            }
    };
    // The aces2 CAM DRT is ~20 evals/texel worst case (gamut bisection); parallelize over blue
    // slices (each thread owns disjoint output ranges).
    const uint32_t workers = std::min(size, std::max(2u, std::thread::hardware_concurrency()));
    std::atomic<uint32_t> next{ 0 };
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (uint32_t w = 0; w < workers; ++w)
        pool.emplace_back([&] {
            for (uint32_t b = next.fetch_add(1); b < size; b = next.fetch_add(1)) bake_slice(b);
        });
    for (std::thread& t : pool) t.join();
    return lut;
}

glm::vec3 sample_lut(const std::vector<float>& lut, uint32_t size, glm::vec3 exposed)
{
    const glm::vec3 enc = shaper_encode(exposed);
    const float fs = float(size - 1);
    const glm::vec3 f = enc * fs;
    const glm::ivec3 i0 = glm::clamp(glm::ivec3(f), glm::ivec3(0), glm::ivec3(int(size) - 2));
    const glm::vec3 t = f - glm::vec3(i0);
    const auto texel = [&](int r, int g, int b) {
        const size_t idx = (size_t(g) * size * size + size_t(b) * size + size_t(r)) * 3;
        return glm::vec3(lut[idx], lut[idx + 1], lut[idx + 2]);
    };
    glm::vec3 c00 = glm::mix(texel(i0.x, i0.y, i0.z), texel(i0.x + 1, i0.y, i0.z), t.x);
    glm::vec3 c10 = glm::mix(texel(i0.x, i0.y + 1, i0.z), texel(i0.x + 1, i0.y + 1, i0.z), t.x);
    glm::vec3 c01 = glm::mix(texel(i0.x, i0.y, i0.z + 1), texel(i0.x + 1, i0.y, i0.z + 1), t.x);
    glm::vec3 c11 = glm::mix(texel(i0.x, i0.y + 1, i0.z + 1), texel(i0.x + 1, i0.y + 1, i0.z + 1), t.x);
    return glm::mix(glm::mix(c00, c10, t.y), glm::mix(c01, c11, t.y), t.z);
}

}  // namespace string::core::tonemap
