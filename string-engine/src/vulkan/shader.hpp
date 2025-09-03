#pragma once

#include <string/logger.hpp>
#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-com-helper.h>

namespace string
{

inline bool compile_shader()
{
    // 1. Create Global Session
    Slang::ComPtr<slang::IGlobalSession> global_session;
    if (SLANG_FAILED(createGlobalSession(global_session.writeRef())))
    {
        STRING_LOG_ERROR("Failed to create Slang global session.");
        return false;
    }

    // 2. Create Session
    slang::SessionDesc session_desc = {};
    slang::TargetDesc target_desc = {};
    target_desc.format = SLANG_SPIRV;
    target_desc.profile = global_session->findProfile("spirv_1_5");

    session_desc.targets = &target_desc;
    session_desc.targetCount = 1;

    std::array<slang::CompilerOptionEntry, 1> options = {
        {
            slang::CompilerOptionName::EmitSpirvDirectly,
            {slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr}
        }
    };
    session_desc.compilerOptionEntries = options.data();
    session_desc.compilerOptionEntryCount = options.size();

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global_session->createSession(session_desc, session.writeRef())))
    {
        STRING_LOG_ERROR("Failed to create Slang session.");
        return false;
    }

    const char* src = R"(
    struct SceneParams { float4x4 viewProj; float4 lightDir; };
    [binding(0,0)] uniform SceneParamsBlock { SceneParams params; };


    float4 VSMain(float2 pos : POSITION) : SV_Position
    {
    return float4(pos, 0.0, 1.0);
    }
    )";

    // 3. Load module
    Slang::ComPtr<slang::IModule> slang_module;
    {
        Slang::ComPtr<slang::IBlob> diagnostics_blob;
        slang_module = session->loadModuleFromSourceString(
            "shortest",                  // Module name
            "shortest.slang",            // Module path
            src,              // Shader source code
            diagnostics_blob.writeRef()); // Optional diagnostic container

        if (diagnostics_blob != nullptr)
        {
            STRING_LOG_ERROR("{}", diagnostics_blob->getBufferPointer());
        }

        if (!slang_module)
        {
            return false;
        }
    }

    // 4. Query Entry Points
    Slang::ComPtr<slang::IEntryPoint> entry_point;
    slang_module->findEntryPointByName("computeMain", entry_point.writeRef());
    if (!entry_point)
    {
        STRING_LOG_ERROR("Error finding entrypoint of shader.");
        return false;
    }

    // 5. Compose Modules + Entry Points
    std::array<slang::IComponentType*, 2> component_types = {
        slang_module,
        entry_point
    };

    Slang::ComPtr<slang::IComponentType> composed_program;
    {
        Slang::ComPtr<slang::IBlob> diagnostics_blob;
        SlangResult result = session->createCompositeComponentType(
            component_types.data(),
            component_types.size(),
            composed_program.writeRef(),
            diagnostics_blob.writeRef());
        if (diagnostics_blob != nullptr)
        {
            STRING_LOG_ERROR("{}", diagnostics_blob->getBufferPointer());
        }
        SLANG_RETURN_ON_FAIL(result);
    }

    // 6. Link
    Slang::ComPtr<slang::IComponentType> linked_program;
    {
        Slang::ComPtr<slang::IBlob> diagnostics_blob;
        SlangResult result = composed_program->link(
            linked_program.writeRef(),
            diagnostics_blob.writeRef());
        if (diagnostics_blob != nullptr)
        {
            STRING_LOG_ERROR("{}", diagnostics_blob->getBufferPointer());
        }
        SLANG_RETURN_ON_FAIL(result);
    }

    // 7. Get Target Kernel Code
    Slang::ComPtr<slang::IBlob> spirv_code;
    {
        Slang::ComPtr<slang::IBlob> diagnostics_blob;
        SlangResult result = linked_program->getEntryPointCode(
            0,
            0,
            spirv_code.writeRef(),
            diagnostics_blob.writeRef());
        if (diagnostics_blob != nullptr)
        {
            STRING_LOG_ERROR("{}", diagnostics_blob->getBufferPointer());
        }
        SLANG_RETURN_ON_FAIL(result);
    }

    STRING_LOG_INFO("Compiled {} bytes of SPIR-V!", spirv_code->getBufferSize());

    return true;
}

}