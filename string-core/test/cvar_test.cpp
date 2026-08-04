#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include <string/core/cvar.hpp>

using namespace string::core;

namespace
{

// Each test declares its CVars with a unique name prefix so the process-global registry doesn't
// collide across tests (CVars self-register on construction and de-register on destruction; the
// test-local objects are destroyed at end of scope).

TEST(CVar, RegistrationAndLookup)
{
    CVar<int32_t> v{"test.reg.count", 7, "a count"};
    CVarBase* found = CVarRegistry::instance().find("test.reg.count");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, &v);
    EXPECT_EQ(found->type(), CVarType::Int);
    EXPECT_EQ(found->help(), "a count");
    EXPECT_EQ(v.get(), 7);
}

TEST(CVar, UnregisterOnDestruction)
{
    {
        CVar<bool> v{"test.reg.temp", true, "temp"};
        EXPECT_NE(CVarRegistry::instance().find("test.reg.temp"), nullptr);
    }
    EXPECT_EQ(CVarRegistry::instance().find("test.reg.temp"), nullptr);
}

TEST(CVar, Types)
{
    CVar<bool> b{"test.type.b", false, "b"};
    CVar<int32_t> i{"test.type.i", 0, "i"};
    CVar<float> f{"test.type.f", 0.0f, "f"};
    CVar<std::string> s{"test.type.s", "hello", "s"};

    EXPECT_EQ(b.type(), CVarType::Bool);
    EXPECT_EQ(i.type(), CVarType::Int);
    EXPECT_EQ(f.type(), CVarType::Float);
    EXPECT_EQ(s.type(), CVarType::String);

    EXPECT_FALSE(b.get());
    EXPECT_EQ(i.get(), 0);
    EXPECT_FLOAT_EQ(f.get(), 0.0f);
    EXPECT_EQ(s.get(), "hello");
}

TEST(CVar, SetFromString)
{
    CVar<bool> b{"test.set.b", false, "b"};
    CVar<int32_t> i{"test.set.i", 0, "i"};
    CVar<float> f{"test.set.f", 0.0f, "f"};
    CVar<std::string> s{"test.set.s", "", "s"};

    EXPECT_TRUE(b.set_from_string("true"));
    EXPECT_TRUE(b.get());
    EXPECT_TRUE(b.set_from_string("off"));
    EXPECT_FALSE(b.get());
    EXPECT_FALSE(b.set_from_string("maybe"));  // unchanged
    EXPECT_FALSE(b.get());

    EXPECT_TRUE(i.set_from_string("42"));
    EXPECT_EQ(i.get(), 42);
    EXPECT_TRUE(i.set_from_string("-3"));
    EXPECT_EQ(i.get(), -3);
    EXPECT_FALSE(i.set_from_string("3.5"));   // trailing junk -> reject
    EXPECT_EQ(i.get(), -3);

    EXPECT_TRUE(f.set_from_string("0.25"));
    EXPECT_FLOAT_EQ(f.get(), 0.25f);
    EXPECT_FALSE(f.set_from_string("abc"));
    EXPECT_FLOAT_EQ(f.get(), 0.25f);

    EXPECT_TRUE(s.set_from_string("/tmp/x.png"));
    EXPECT_EQ(s.get(), "/tmp/x.png");
}

TEST(CVar, RegistrySetFromString)
{
    CVar<float> f{"test.regset.f", 1.0f, "f"};
    EXPECT_TRUE(CVarRegistry::instance().set_from_string("test.regset.f", "2.5"));
    EXPECT_FLOAT_EQ(f.get(), 2.5f);
    EXPECT_FALSE(CVarRegistry::instance().set_from_string("test.regset.nope", "1"));
}

TEST(CVar, EnvName)
{
    EXPECT_EQ(env_name_for("r.lod.error_px"), "STRING_R_LOD_ERROR_PX");
    EXPECT_EQ(env_name_for("capture_frame"), "STRING_CAPTURE_FRAME");
}

TEST(CVar, EnvOverride)
{
    setenv("STRING_TEST_ENV_VAL", "13", 1);
    CVar<int32_t> v{"test.env.val", 1, "v"};
    CVarRegistry::instance().apply_env();
    EXPECT_EQ(v.get(), 13);
    unsetenv("STRING_TEST_ENV_VAL");
}

TEST(CVar, AliasLookupAndEnv)
{
    setenv("STRING_LEGACY_LEVER", "99", 1);
    CVar<int32_t> v{"test.alias.new", 1, "v"};
    v.add_alias("legacy_lever");

    // Alias resolves in lookup...
    EXPECT_EQ(CVarRegistry::instance().find_including_aliases("legacy_lever"), &v);
    EXPECT_EQ(CVarRegistry::instance().find("legacy_lever"), nullptr);  // not a primary name

    // ...and via the env bridge (STRING_LEGACY_LEVER honoured verbatim).
    CVarRegistry::instance().apply_env();
    EXPECT_EQ(v.get(), 99);
    unsetenv("STRING_LEGACY_LEVER");
}

TEST(CVar, DuplicateNameLastWins)
{
    CVar<int32_t> a{"test.dup.name", 1, "a"};
    {
        CVar<int32_t> b{"test.dup.name", 2, "b"};
        // Duplicate primary name: registry resolves to the most-recently registered.
        EXPECT_EQ(CVarRegistry::instance().find("test.dup.name"), &b);
    }
    // b destroyed: its de-register removed the shared name, so lookup is now null (a is orphaned
    // from the registry but still valid as an object). This documents the last-writer-wins policy.
    EXPECT_EQ(CVarRegistry::instance().find("test.dup.name"), nullptr);
    (void)a;
}

TEST(CVar, Enumeration)
{
    const std::size_t before = CVarRegistry::instance().size();
    CVar<bool> v{"test.enum.flag", true, "flag"};
    EXPECT_EQ(CVarRegistry::instance().size(), before + 1);

    bool seen = false;
    for (CVarBase* c : CVarRegistry::instance().all())
        if (c->name() == "test.enum.flag") seen = true;
    EXPECT_TRUE(seen);
}

}  // namespace
