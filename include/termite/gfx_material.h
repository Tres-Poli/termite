#pragma once

#include "types.h"
#include "gfx_defines.h"
#include "assetlib.h"

#define MAX_MATERIAL_VARS 16

namespace tee
{
    struct MaterialLib;
    struct MaterialT {};
    typedef PhantomType<uint16_t, MaterialT, UINT16_MAX> MaterialHandle;

    struct MaterialDecl
    {
        const char* names[MAX_MATERIAL_VARS];
        UniformType::Enum types[MAX_MATERIAL_VARS];
        uint16_t arrayCounts[MAX_MATERIAL_VARS];
        bool initValues[MAX_MATERIAL_VARS];
        int count;

        // Initial Data, initial data is likely a Vec4 or a Texture
        union InitData
        {
            vec4_t v;
            AssetHandle t;
        };
        InitData initData[MAX_MATERIAL_VARS];
    };

    namespace gfx {
        TEE_API MaterialHandle createMaterial(ProgramHandle prog, const MaterialDecl& decl, bx::AllocatorI* dataAlloc = nullptr);
        TEE_API void destroyMaterial(MaterialHandle handle);
        TEE_API void applyMaterial(MaterialHandle handle);

        TEE_API void setMtlValue(MaterialHandle handle, const char* name, const vec4_t& v);
        TEE_API void setMtlValue(MaterialHandle handle, const char* name, const vec4_t* vs, uint16_t num);
        TEE_API void setMtlValue(MaterialHandle handle, const char* name, const mat4_t& mat);
        TEE_API void setMtlValue(MaterialHandle handle, const char* name, const mat4_t* mats, uint16_t num);
        TEE_API void setMtlValue(MaterialHandle handle, const char* name, const mat3_t& mat);
        TEE_API void setMtlValue(MaterialHandle handle, const char* name, const mat3_t* mats, uint16_t num);
        TEE_API void setMtlTexture(MaterialHandle handle, const char* name, uint8_t stage, AssetHandle texHandle, 
                                   TextureFlag::Bits flags = TextureFlag::FromTexture);

        void beginMtlDecl(MaterialDecl* decl);
        void addMtlDeclAttrib(MaterialDecl* decl, const char* name, UniformType::Enum type, uint16_t num = 1);
        void setMtlDeclInitData(MaterialDecl* decl, const vec4_t& v);
        void setMtlDeclInitData(MaterialDecl* decl, AssetHandle aHandle);
        void endMtlDecl(MaterialDecl* decl);
    }
} // namespace tee

#include "inline/gfx_material.inl"
