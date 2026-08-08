// Pins the math behind meshlet.slang::transform_normal, which is what carries vertex normals and
// meshlet backface-cone axes from model space to world space.
//
// SCOPE, honestly: this is a C++ MIRROR of the shader's formula, not a test of the shader itself —
// there is no harness that can execute Slang here. It guards the maths (that the cofactor really is
// the inverse transpose up to scale, and that normals need it at all) and it fails loudly if someone
// "simplifies" the shader back to the model matrix. It cannot catch a transcription slip in the
// .slang. If you change one, change both.
//
// The reason this exists: normals were transformed by the plain model matrix, which is correct for
// rotation, uniform scale and mirroring — everything in the test scene — and badly wrong for
// non-uniform scale, in the WRONG DIRECTION. A stretched surface gets flatter, so its normal must
// tilt AWAY from the stretched axis; the model matrix tilts it TOWARD.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace
{

// The exact operation meshlet.slang::transform_normal performs: the cofactor matrix applied as a
// weighted sum of its columns, each column being the cross product of the other two model basis
// vectors. Written this way in the shader so no float3x3 row/column convention can be got backwards.
glm::vec3 transform_normal(const glm::mat4& model, const glm::vec3& n)
{
    const glm::vec3 c0(model[0][0], model[0][1], model[0][2]);
    const glm::vec3 c1(model[1][0], model[1][1], model[1][2]);
    const glm::vec3 c2(model[2][0], model[2][1], model[2][2]);
    const glm::vec3 n0 = glm::cross(c1, c2);
    const glm::vec3 n1 = glm::cross(c2, c0);
    const glm::vec3 n2 = glm::cross(c0, c1);
    const float det = glm::dot(c0, n0);
    // The cofactor is det * inverse-transpose; divide the SIGN back out or mirrored transforms come
    // out pointing the wrong way. Magnitude is left alone — every consumer normalizes.
    return (n0 * n.x + n1 * n.y + n2 * n.z) * (det < 0.0f ? -1.0f : 1.0f);
}

// The textbook normal matrix, computed a completely different way, as the oracle.
glm::vec3 reference(const glm::mat4& model, const glm::vec3& n)
{
    return glm::transpose(glm::inverse(glm::mat3(model))) * n;
}

void expect_same_direction(const glm::vec3& a, const glm::vec3& b, const char* what)
{
    const float cosine = glm::dot(glm::normalize(a), glm::normalize(b));
    EXPECT_NEAR(cosine, 1.0f, 1e-4f) << what;
}

}  // namespace

// The cofactor is the inverse transpose scaled by the determinant, so after normalisation the two
// agree — for every kind of transform, including the awkward ones.
TEST(NormalMatrix, MatchesInverseTransposeForEveryTransformKind)
{
    const glm::vec3 n = glm::normalize(glm::vec3(0.3f, 1.0f, -0.7f));
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), 0.9f, glm::normalize(glm::vec3(1, 2, 3)));

    struct Case { const char* name; glm::mat4 m; };
    const Case cases[] = {
        { "identity",              glm::mat4(1.0f) },
        { "rotation",              rot },
        { "uniform scale",         glm::scale(glm::mat4(1.0f), glm::vec3(2.5f)) },
        { "non-uniform scale",     glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 4.0f)) },
        { "rotated + stretched",   rot * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f, 3.0f, 1.0f)) },
        { "translated",            glm::translate(glm::mat4(1.0f), glm::vec3(10, -4, 7)) * rot },
        // Mirrors were missing here at first, and the raw cofactor passed everything above while
        // being sign-flipped for them. A negative determinant is not an edge case in this engine —
        // it is what an applied Mirror modifier produces.
        { "mirror (1 axis)",       glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)) },
        { "mirror + rotation",     rot * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f)) },
        { "mirror + stretch",      glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, -0.5f, 1.0f)) },
    };

    for (const Case& c : cases)
        expect_same_direction(transform_normal(c.m, n), reference(c.m, n), c.name);
}

// The geometric statement, independent of any matrix identity: a normal must stay perpendicular to
// the surface it belongs to. Take two directions spanning the surface, transform them as POSITIONS,
// and the transformed normal has to be perpendicular to both.
TEST(NormalMatrix, StaysPerpendicularToTheTransformedSurface)
{
    // A 45-degree slope in YZ: the surface contains (0,1,-1), so its normal is (0,1,1).
    const glm::vec3 along_a(1.0f, 0.0f, 0.0f);
    const glm::vec3 along_b(0.0f, 1.0f, -1.0f);
    const glm::vec3 n = glm::normalize(glm::cross(along_b, along_a));

    const glm::mat4 stretch_z = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 4.0f));
    const glm::mat3 m3(stretch_z);

    const glm::vec3 world_n = glm::normalize(transform_normal(stretch_z, n));
    EXPECT_NEAR(glm::dot(world_n, glm::normalize(m3 * along_a)), 0.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(world_n, glm::normalize(m3 * along_b)), 0.0f, 1e-4f);
}

// The regression itself, stated as a number. Under a 4x stretch the old code put this normal 62
// degrees away from the surface it belongs to; if this ever passes, the shader has been reverted.
TEST(NormalMatrix, ModelMatrixIsWrongUnderNonUniformScaleAndTiltsTheWrongWay)
{
    const glm::vec3 n = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
    const glm::mat4 stretch_z = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 4.0f));

    const glm::vec3 correct = glm::normalize(transform_normal(stretch_z, n));
    const glm::vec3 naive   = glm::normalize(glm::mat3(stretch_z) * n);

    const float up(0.0f);
    (void)up;
    const float correct_deg = glm::degrees(std::atan2(correct.z, correct.y));
    const float naive_deg   = glm::degrees(std::atan2(naive.z, naive.y));

    EXPECT_NEAR(correct_deg, 14.036f, 0.01f);   // tilts AWAY from the stretched axis
    EXPECT_NEAR(naive_deg,   75.964f, 0.01f);   // tilts TOWARD it — the bug
    EXPECT_GT(std::abs(naive_deg - correct_deg), 60.0f);
}

// Mirroring keeps the determinant's sign, so the normal flips with the geometry. That has to hold:
// the cook reverses triangle winding for mirrored draws, and a normal that did NOT flip would then
// disagree with the front face.
TEST(NormalMatrix, MirrorFlipsTheNormalToMatchTheReversedWinding)
{
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    const glm::mat4 mirror_z = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));

    const glm::vec3 out = glm::normalize(transform_normal(mirror_z, n));
    EXPECT_NEAR(out.z, -1.0f, 1e-5f);
}

// Uniform scale and rotation must be untouched in DIRECTION — this is why the bug hid for so long,
// and it is the property that makes the change safe for all existing content.
TEST(NormalMatrix, UniformScaleAndRotationAreUnchangedInDirection)
{
    const glm::vec3 n = glm::normalize(glm::vec3(0.2f, -0.5f, 0.84f));
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), 1.3f, glm::normalize(glm::vec3(-1, 2, 0.5f)));
    const glm::mat4 m = rot * glm::scale(glm::mat4(1.0f), glm::vec3(0.47f));

    expect_same_direction(transform_normal(m, n), glm::mat3(m) * n,
                          "uniform scale + rotation must match the plain model matrix");
}
