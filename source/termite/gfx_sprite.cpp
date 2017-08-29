#include "pch.h"
#include "gfx_sprite.h"
#include "gfx_texture.h"
#include "math_util.h"

#include "bx/readerwriter.h"
#include "bxx/path.h"
#include "bxx/array.h"
#include "bxx/linked_list.h"
#include "bxx/pool.h"
#include "bxx/logger.h"

#include "rapidjson/error/en.h"
#include "rapidjson/document.h"
#include "bxx/rapidjson_allocator.h"

#include T_MAKE_SHADER_PATH(shaders_h, sprite.vso)
#include T_MAKE_SHADER_PATH(shaders_h, sprite.fso)
#include T_MAKE_SHADER_PATH(shaders_h, sprite_add.vso)
#include T_MAKE_SHADER_PATH(shaders_h, sprite_add.fso)

#include <algorithm>

using namespace rapidjson;
using namespace termite;

static const uint64_t kSpriteKeyOrderBits = 8;
static const uint64_t kSpriteKeyOrderMask = (1 << kSpriteKeyOrderBits) - 1;
static const uint64_t kSpriteKeyTextureBits = 16;
static const uint64_t kSpriteKeyTextureMask = (1 << kSpriteKeyTextureBits) - 1;
static const uint64_t kSpriteKeyIdBits = 32;
static const uint64_t kSpriteKeyIdMask = (1llu << kSpriteKeyIdBits) - 1;

static const uint8_t kSpriteKeyOrderShift = kSpriteKeyTextureBits + kSpriteKeyIdBits;
static const uint8_t kSpriteKeyTextureShift = kSpriteKeyIdBits;

#define MAKE_SPRITE_KEY(_Order, _Texture, _Id) \
    (uint64_t(_Order & kSpriteKeyOrderMask) << kSpriteKeyOrderShift) | (uint64_t(_Texture & kSpriteKeyTextureMask) << kSpriteKeyTextureShift) | uint64_t(_Id & kSpriteKeyIdMask)
#define SPRITE_KEY_GET_BATCH(_Key) uint32_t((_Key >> kSpriteKeyIdBits) & kSpriteKeyIdMask)

struct SpriteVertex
{
    vec2_t pos;
    vec3_t transform1;
    vec3_t transform2;
    vec2_t coords;
    uint32_t color;

    static void init()
    {
        vdeclBegin(&Decl);
        vdeclAdd(&Decl, VertexAttrib::Position, 2, VertexAttribType::Float);        // pos
        vdeclAdd(&Decl, VertexAttrib::TexCoord0, 3, VertexAttribType::Float);       // transform mat (part 1)
        vdeclAdd(&Decl, VertexAttrib::TexCoord1, 3, VertexAttribType::Float);       // transform mat (part 2)
        vdeclAdd(&Decl, VertexAttrib::TexCoord2, 2, VertexAttribType::Float);       // texture coords
        vdeclAdd(&Decl, VertexAttrib::Color0, 4, VertexAttribType::Uint8, true);    // color
        vdeclEnd(&Decl);
    }

    static VertexDecl Decl;
};
VertexDecl SpriteVertex::Decl;

class SpriteSheetLoader : public ResourceCallbacksI
{
public:
    bool loadObj(const MemoryBlock* mem, const ResourceTypeParams& params, uintptr_t* obj, bx::AllocatorI* alloc) override;
    void unloadObj(uintptr_t obj, bx::AllocatorI* alloc) override;
    void onReload(ResourceHandle handle, bx::AllocatorI* alloc) override;
};

struct SpriteFrame
{
    ResourceHandle texHandle;   // Handle to spritesheet/texture resource
    ResourceHandle ssHandle;    // For spritesheets we have spritesheet handle
    uint8_t flags;

    size_t nameHash;
    size_t tagHash;

    rect_t frame;       // top-left, right-bottom
    vec2_t pivot;       // In textures it should be provided, in spritesheets, this value comes from the file
    vec2_t sourceSize;  // Retreive from texture or spritesheet
    vec2_t posOffset;   // Offset to move the image in source bounds (=0 in texture sprites)
    vec2_t sizeOffset;  // Offset to resize the image in source bounds (=1 in texture sprites)
    float rotOffset;    // Rotation offset to fit the original (=0 in texture sprites)
    float pixelRatio;

    SpriteFrameCallback frameCallback;
    void* frameCallbackUserData;

    SpriteFrame() :
        nameHash(0),
        tagHash(0),
        frameCallback(nullptr),
        frameCallbackUserData(nullptr)
    {
    }
};

namespace termite
{
    struct Sprite
    {
        typedef bx::List<Sprite*>::Node LNode;

        uint32_t id;
        bx::AllocatorI* alloc;
        vec2_t halfSize;
        vec2_t sizeMultiplier;
        vec2_t posOffset;
        bx::Array<SpriteFrame> frames;
        int curFrameIdx;

        float animTm;       // Animation timer
        bool playReverse;
        float playSpeed;
        float resumeSpeed;
        color_t tint;
        uint8_t order;

        SpriteFlag::Bits flip;
        LNode lnode;

        SpriteFrameCallback endCallback;
        void* endUserData;
        void* userData;
        bool triggerEndCallback;

        Sprite(bx::AllocatorI* _alloc) :
            alloc(_alloc),
            curFrameIdx(0),
            animTm(0),
            playReverse(false),
            playSpeed(30.0f),
            resumeSpeed(30.0f),
            order(0),
            flip(SpriteFlag::None),
            lnode(this),
            endCallback(nullptr),
            endUserData(nullptr),
            triggerEndCallback(false)
        {
            tint = color1n(0xffffffff);
            posOffset = vec2f(0, 0);
            sizeMultiplier = vec2f(1.0f, 1.0f);
            userData = nullptr;
        }

        inline const SpriteFrame& getCurFrame() const
        {
            return frames[curFrameIdx];
        }
    };
}

struct SpriteSheetFrame
{
    size_t filenameHash;
    rect_t frame;
    vec2_t pivot;
    vec2_t sourceSize;
    vec2_t posOffset;
    vec2_t sizeOffset;
    float rotOffset;
    float pixelRatio;
};

struct SpriteSheet
{
    ResourceHandle texHandle;
    int numFrames;
    SpriteSheetFrame* frames;

    SpriteSheet() :
        numFrames(0),
        frames(nullptr)
    {
    }
};

struct SpriteSystem
{
    GfxDriverApi* driver;
    bx::AllocatorI* alloc;
    ProgramHandle spriteProg;
    ProgramHandle spriteAddProg;
    UniformHandle u_texture;
    SpriteSheetLoader loader;
    SpriteSheet* failSheet;
    SpriteSheet* asyncSheet;
    bx::List<Sprite*> spriteList;       // keep a list of sprites for proper shutdown and sheet reloading

    SpriteSystem(bx::AllocatorI* _alloc) : 
        driver(nullptr),
        alloc(_alloc),
        failSheet(nullptr),
        asyncSheet(nullptr)
    {
    }
};

static SpriteSystem* g_spriteSys = nullptr;

static const SpriteSheetFrame* findSpritesheetFrame(const SpriteSheet* sheet, size_t nameHash)
{
    SpriteSheetFrame* sheetFrame = nullptr;
    for (int i = 0, c = sheet->numFrames; i < c; i++) {
        if (sheet->frames[i].filenameHash != nameHash)
            continue;
        sheetFrame = &sheet->frames[i];
        break;
    }

    return sheetFrame;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SpriteSheetLoader
bool SpriteSheetLoader::loadObj(const MemoryBlock* mem, const ResourceTypeParams& params, uintptr_t* obj, 
                                bx::AllocatorI* alloc)
{
    const LoadSpriteSheetParams* ssParams = (const LoadSpriteSheetParams*)params.userParams;

    bx::AllocatorI* tmpAlloc = getTempAlloc();
    char* jsonStr = (char*)BX_ALLOC(tmpAlloc, mem->size + 1);
    if (!jsonStr) {
        T_ERROR("Out of Memory");
        return false;
    }
    memcpy(jsonStr, mem->data, mem->size);
    jsonStr[mem->size] = 0;

    BxAllocatorNoFree jalloc(tmpAlloc);
    BxAllocator jpool(4096, &jalloc);
    BxDocument jdoc(&jpool, 1024, &jalloc);

    if (jdoc.ParseInsitu(jsonStr).HasParseError()) {
        T_ERROR("Parse Json Error: %s (Pos: %s)", GetParseError_En(jdoc.GetParseError()), jdoc.GetErrorOffset());                 
        return false;
    }
    BX_FREE(tmpAlloc, jsonStr);

    if (!jdoc.HasMember("frames") || !jdoc.HasMember("meta")) {
        T_ERROR("SpriteSheet Json is Invalid");
        return false;
    }
    const BxValue& jframes = jdoc["frames"];
    const BxValue& jmeta = jdoc["meta"];

    assert(jframes.IsArray());
    int numFrames = jframes.Size();
    if (numFrames == 0)
        return false;

    // Create sprite sheet
    size_t totalSz = sizeof(SpriteSheet) + numFrames*sizeof(SpriteSheetFrame);
    uint8_t* buff = (uint8_t*)BX_ALLOC(alloc ? alloc : g_spriteSys->alloc, totalSz);
    if (!buff) {
        T_ERROR("Out of Memory");
        return false;
    }
    SpriteSheet* ss = new(buff) SpriteSheet();  buff += sizeof(SpriteSheet);
    ss->frames = (SpriteSheetFrame*)buff;
    ss->numFrames = numFrames;

    // image width/height
    const BxValue& jsize = jmeta["size"];
    float imgWidth = float(jsize["w"].GetInt());
    float imgHeight = float(jsize["h"].GetInt());

    // Make texture path and load it
    const char* imageFile = jmeta["image"].GetString();
    bx::Path texFilepath = bx::Path(params.uri).getDirectory();
    texFilepath.joinUnix(imageFile);

    LoadTextureParams texParams;
    texParams.flags = ssParams->flags;
    texParams.generateMips = ssParams->generateMips;
    texParams.skipMips = ssParams->skipMips;
    texParams.fmt = ssParams->fmt;
    ss->texHandle = loadResource("texture", texFilepath.cstr(), &texParams, params.flags, alloc ? alloc : nullptr);

    for (int i = 0; i < numFrames; i++) {
        SpriteSheetFrame& frame = ss->frames[i];
        const BxValue& jframe = jframes[i];
        const char* filename = jframe["filename"].GetString();
        frame.filenameHash = tinystl::hash_string(filename, strlen(filename));
        bool rotated = jframe["rotated"].GetBool();

        const BxValue& jframeFrame = jframe["frame"];
        float frameWidth = float(jframeFrame["w"].GetInt());
        float frameHeight = float(jframeFrame["h"].GetInt());
        if (rotated)
            std::swap<float>(frameWidth, frameHeight);

        frame.frame = rectwh(float(jframeFrame["x"].GetInt()) / imgWidth,
                              float(jframeFrame["y"].GetInt()) / imgHeight,
                              frameWidth / imgWidth,
                              frameHeight / imgHeight);

        const BxValue& jsourceSize = jframe["sourceSize"];
        frame.sourceSize = vec2f(float(jsourceSize["w"].GetInt()),
                                 float(jsourceSize["h"].GetInt()));

        // Normalize pos/size offsets (0~1)
        // Rotate offset can only be 90 degrees
        const BxValue& jssFrame = jframe["spriteSourceSize"];
        float srcx = float(jssFrame["x"].GetInt());
        float srcy = float(jssFrame["y"].GetInt());
        float srcw = float(jssFrame["w"].GetInt());
        float srch = float(jssFrame["h"].GetInt());

        frame.sizeOffset = vec2f(srcw / frame.sourceSize.x, srch / frame.sourceSize.y);

        if (!rotated) {
            frame.rotOffset = 0;
        } else {
            std::swap<float>(srcw, srch);
            frame.rotOffset = -90.0f;
        }
        frame.pixelRatio = frame.sourceSize.x / frame.sourceSize.y;

        const BxValue& jpivot = jframe["pivot"];
        const BxValue& jpivotX = jpivot["x"];
        const BxValue& jpivotY = jpivot["y"];
        float pivotx = jpivotX.IsFloat() ?  jpivotX.GetFloat() : float(jpivotX.GetInt());
        float pivoty = jpivotY.IsFloat() ?  jpivotY.GetFloat() : float(jpivotY.GetInt());
        frame.pivot = vec2f(pivotx - 0.5f, -pivoty + 0.5f);     // convert to our coordinates

        vec2_t srcOffset = vec2f((srcx + srcw*0.5f)/frame.sourceSize.x - 0.5f, -(srcy + srch*0.5f)/frame.sourceSize.y + 0.5f);
        frame.posOffset = srcOffset;
    }

    *obj = uintptr_t(ss);

    return true;
}

void SpriteSheetLoader::unloadObj(uintptr_t obj, bx::AllocatorI* alloc)
{
    assert(g_spriteSys);
    assert(obj);

    SpriteSheet* sheet = (SpriteSheet*)obj;
    if (sheet->texHandle.isValid()) {
        unloadResource(sheet->texHandle);
        sheet->texHandle.reset();
    }
    
    BX_FREE(alloc ? alloc : g_spriteSys->alloc, sheet);
}

void SpriteSheetLoader::onReload(ResourceHandle handle, bx::AllocatorI* alloc)
{
    // Search in sprites, check if spritesheet is the same and reload the data
    Sprite::LNode* node = g_spriteSys->spriteList.getFirst();
    while (node) {
        Sprite* sprite = node->data;
        for (int i = 0, c = sprite->frames.getCount(); i < c; i++) {
            if (sprite->frames[i].ssHandle != handle)
                continue;

            // find the frame in spritesheet and update data
            SpriteFrame& frame = sprite->frames[i];
            SpriteSheet* sheet = getResourcePtr<SpriteSheet>(handle);
            const SpriteSheetFrame* sheetFrame = findSpritesheetFrame(sheet, frame.nameHash);
            if (sheetFrame) {
                frame.texHandle = sheet->texHandle;
                frame.pivot = sheetFrame->pivot;
                frame.frame = sheetFrame->frame;
                frame.sourceSize = sheetFrame->sourceSize;
                frame.posOffset = sheetFrame->posOffset;
                frame.sizeOffset = sheetFrame->sizeOffset;
                frame.rotOffset = sheetFrame->rotOffset;
                frame.pixelRatio = sheetFrame->pixelRatio;
            } else {
                frame.texHandle = getResourceFailHandle("texture");
                Texture* tex = getResourcePtr<Texture>(frame.texHandle);
                frame.pivot = vec2f(0, 0);
                frame.frame = rectf(0, 0, 1.0f, 1.0f);
                frame.sourceSize = vec2f(float(tex->info.width), float(tex->info.height));
                frame.posOffset = vec2f(0, 0);
                frame.sizeOffset = vec2f(1.0f, 1.0f);
                frame.rotOffset = 0;
                frame.pixelRatio = 1.0f;
            }
        }
        node = node->next;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static SpriteSheet* createDummySpriteSheet(ResourceHandle texHandle, bx::AllocatorI* alloc)
{
    // Create sprite sheet
    size_t totalSz = sizeof(SpriteSheet) + sizeof(SpriteSheetFrame);
    uint8_t* buff = (uint8_t*)BX_ALLOC(alloc, totalSz);
    if (!buff) {
        T_ERROR("Out of Memory");
        return nullptr;
    }
    SpriteSheet* ss = new(buff) SpriteSheet();  buff += sizeof(SpriteSheet);
    ss->frames = (SpriteSheetFrame*)buff;
    ss->numFrames = 1;

    ss->texHandle = getResourceFailHandle("texture");
    assert(ss->texHandle.isValid());

    Texture* tex = getResourcePtr<Texture>(ss->texHandle);
    float imgWidth = float(tex->info.width);
    float imgHeight = float(tex->info.height);

    ss->frames[0].filenameHash = 0;
    ss->frames[0].frame = rectf(0, 0, 1.0f, 1.0f);
    ss->frames[0].pivot = vec2f(0, 0);
    ss->frames[0].posOffset = vec2f(0, 0);
    ss->frames[0].sizeOffset = vec2f(1.0f, 1.0f);
    ss->frames[0].sourceSize = vec2f(imgWidth, imgHeight);
    ss->frames[0].rotOffset = 0;
    
    return ss;
}

result_t termite::initSpriteSystem(GfxDriverApi* driver, bx::AllocatorI* alloc)
{
    if (g_spriteSys) {
        assert(false);
        return T_ERR_ALREADY_INITIALIZED;
    }

    g_spriteSys = BX_NEW(alloc, SpriteSystem)(alloc);
    if (!g_spriteSys) {
        return T_ERR_OUTOFMEM;
    }

    g_spriteSys->driver = driver;

    SpriteVertex::init();

    g_spriteSys->spriteProg = 
        driver->createProgram(driver->createShader(driver->makeRef(sprite_vso, sizeof(sprite_vso), nullptr, nullptr)),
                              driver->createShader(driver->makeRef(sprite_fso, sizeof(sprite_fso), nullptr, nullptr)),
                              true);
    if (!g_spriteSys->spriteProg.isValid())
        return T_ERR_FAILED;

    g_spriteSys->spriteAddProg = 
        driver->createProgram(driver->createShader(driver->makeRef(sprite_add_vso, sizeof(sprite_add_vso), nullptr, nullptr)),
                              driver->createShader(driver->makeRef(sprite_add_fso, sizeof(sprite_add_fso), nullptr, nullptr)),
                              true);
    if (!g_spriteSys->spriteAddProg.isValid())
        return T_ERR_FAILED;

    g_spriteSys->u_texture = driver->createUniform("u_texture", UniformType::Int1, 1);

    // Create fail spritesheet
    g_spriteSys->failSheet = createDummySpriteSheet(getResourceFailHandle("texture"), g_spriteSys->alloc);    
    g_spriteSys->asyncSheet = createDummySpriteSheet(getResourceAsyncHandle("texture"), g_spriteSys->alloc);
    if (!g_spriteSys->failSheet || !g_spriteSys->asyncSheet) {
        T_ERROR("Creating async/fail spritesheets failed");
        return T_ERR_FAILED;
    }

    return 0;
}

Sprite* termite::createSprite(bx::AllocatorI* alloc, const vec2_t halfSize)
{
    static uint32_t id = 0;

    Sprite* sprite = BX_NEW(alloc, Sprite)(alloc);
    if (!sprite)
        return nullptr;
    sprite->id = ++id;
    if (!sprite->frames.create(4, 16, alloc))
        return nullptr;
    sprite->halfSize = halfSize;
    // Add to sprite list
    g_spriteSys->spriteList.addToEnd(&sprite->lnode);
    return sprite;
}

void termite::destroySprite(Sprite* sprite)
{
    // Destroy frames
    for (int i = 0, c = sprite->frames.getCount(); i < c; i++) {
        SpriteFrame& frame = sprite->frames[i];
        if (frame.flags & SpriteFlag::DestroyResource) {
            if (frame.ssHandle.isValid())
                unloadResource(frame.ssHandle);
            else
                unloadResource(frame.texHandle);
        }
    }
    sprite->frames.destroy();

    // Remove from list
    g_spriteSys->spriteList.remove(&sprite->lnode);

    BX_DELETE(sprite->alloc, sprite);
}

void termite::shutdownSpriteSystem()
{
    if (!g_spriteSys)
        return;

    bx::AllocatorI* alloc = g_spriteSys->alloc;
    GfxDriverApi* driver = g_spriteSys->driver;

    if (g_spriteSys->spriteProg.isValid())
        driver->destroyProgram(g_spriteSys->spriteProg);
    if (g_spriteSys->spriteAddProg.isValid())
        driver->destroyProgram(g_spriteSys->spriteAddProg);
    if (g_spriteSys->u_texture.isValid())
        driver->destroyUniform(g_spriteSys->u_texture);

    // Move through the remaining sprites and unload their resources
    Sprite::LNode* node = g_spriteSys->spriteList.getFirst();
    while (node) {
        Sprite::LNode* next = node->next;
        destroySprite(node->data);
        node = next;
    }

    if (g_spriteSys->failSheet)
        g_spriteSys->loader.unloadObj(uintptr_t(g_spriteSys->failSheet), g_spriteSys->alloc);
    if (g_spriteSys->asyncSheet)
        g_spriteSys->loader.unloadObj(uintptr_t(g_spriteSys->asyncSheet), g_spriteSys->alloc);

    BX_DELETE(alloc, g_spriteSys);
    g_spriteSys = nullptr;
}

bool termite::initSpriteSystemGraphics(GfxDriverApi* driver)
{
    g_spriteSys->driver = driver;

    g_spriteSys->spriteProg =
        driver->createProgram(driver->createShader(driver->makeRef(sprite_vso, sizeof(sprite_vso), nullptr, nullptr)),
                              driver->createShader(driver->makeRef(sprite_fso, sizeof(sprite_fso), nullptr, nullptr)),
                              true);
    if (!g_spriteSys->spriteProg.isValid())
        return false;

    g_spriteSys->spriteAddProg =
        driver->createProgram(driver->createShader(driver->makeRef(sprite_add_vso, sizeof(sprite_add_vso), nullptr, nullptr)),
                              driver->createShader(driver->makeRef(sprite_add_fso, sizeof(sprite_add_fso), nullptr, nullptr)),
                              true);
    if (!g_spriteSys->spriteAddProg.isValid())
        return false;

    g_spriteSys->u_texture = driver->createUniform("u_texture", UniformType::Int1, 1);

    return true;
}

void termite::shutdownSpriteSystemGraphics()
{
    GfxDriverApi* driver = g_spriteSys->driver;

    if (g_spriteSys->spriteProg.isValid())
        driver->destroyProgram(g_spriteSys->spriteProg);
    if (g_spriteSys->spriteAddProg.isValid())
        driver->destroyProgram(g_spriteSys->spriteAddProg);
    if (g_spriteSys->u_texture.isValid())
        driver->destroyUniform(g_spriteSys->u_texture);
}

void termite::addSpriteFrameTexture(Sprite* sprite, 
                                    ResourceHandle texHandle, SpriteFlag::Bits flags, const vec2_t pivot /*= vec2_t(0, 0)*/, 
                                    const vec2_t topLeftCoords /*= vec2_t(0, 0)*/, const vec2_t bottomRightCoords /*= vec2_t(1.0f, 1.0f)*/, 
                                    const char* frameTag /*= nullptr*/)
{
    if (texHandle.isValid()) {
        assert(getResourceLoadState(texHandle) != ResourceLoadState::LoadInProgress);
 
        SpriteFrame* frame = new(sprite->frames.push()) SpriteFrame();
        if (frame) {
            frame->texHandle = texHandle;
            frame->flags = flags;
            frame->pivot = pivot;
            frame->frame = rectv(topLeftCoords, bottomRightCoords);
            frame->tagHash = !frameTag ? 0 : tinystl::hash_string(frameTag, strlen(frameTag)) ;

            Texture* tex = getResourcePtr<Texture>(texHandle);
            frame->sourceSize = vec2f(float(tex->info.width), float(tex->info.height));
            frame->posOffset = vec2f(0, 0);
            frame->sizeOffset = vec2f(1.0f, 1.0f);
            frame->rotOffset = 0;
            frame->pixelRatio = ((bottomRightCoords.x - topLeftCoords.x)*frame->sourceSize.x) /
                ((bottomRightCoords.y - topLeftCoords.y)*frame->sourceSize.y);
        }
    }
}

void termite::addSpriteFrameSpritesheet(Sprite* sprite,
                                        ResourceHandle ssHandle, const char* name, SpriteFlag::Bits flags, 
                                        const char* frameTag /*= nullptr*/)
{
    assert(name);
    if (ssHandle.isValid()) {
        assert(getResourceLoadState(ssHandle) != ResourceLoadState::LoadInProgress);
        
        SpriteFrame* frame = new(sprite->frames.push()) SpriteFrame();
        if (frame) {
            frame->ssHandle = ssHandle;
            SpriteSheet* sheet = getResourcePtr<SpriteSheet>(ssHandle);

            // find frame name in spritesheet, if found fill the frame data
            size_t nameHash = tinystl::hash_string(name, strlen(name));
            const SpriteSheetFrame* sheetFrame = findSpritesheetFrame(sheet, nameHash);

            frame->nameHash = nameHash;
            frame->flags = flags;
            frame->tagHash = !frameTag ? 0 : tinystl::hash_string(frameTag, strlen(frameTag));

            // If not found fill dummy data
            if (sheetFrame) {
                frame->texHandle = sheet->texHandle;
                frame->pivot = sheetFrame->pivot;
                frame->frame = sheetFrame->frame;
                frame->sourceSize = sheetFrame->sourceSize;
                frame->posOffset = sheetFrame->posOffset;
                frame->sizeOffset = sheetFrame->sizeOffset;
                frame->rotOffset = sheetFrame->rotOffset;
                frame->pixelRatio = sheetFrame->pixelRatio;
            } else {
                frame->texHandle = getResourceFailHandle("texture");
                Texture* tex = getResourcePtr<Texture>(frame->texHandle);
                frame->pivot = vec2f(0, 0);
                frame->frame = rectf(0, 0, 1.0f, 1.0f);
                frame->sourceSize = vec2f(float(tex->info.width), float(tex->info.height));
                frame->posOffset = vec2f(0, 0);
                frame->sizeOffset = vec2f(1.0f, 1.0f);
                frame->rotOffset = 0;
                frame->pixelRatio = 1.0f;
            }
        }
    }
}

void termite::addSpriteFrameAll(Sprite* sprite, ResourceHandle ssHandle, SpriteFlag::Bits flags)
{
    if (ssHandle.isValid()) {
        assert(getResourceLoadState(ssHandle) != ResourceLoadState::LoadInProgress);

        SpriteSheet* sheet = getResourcePtr<SpriteSheet>(ssHandle);
        SpriteFrame* frames = sprite->frames.pushMany(sheet->numFrames);
        for (int i = 0, c = sheet->numFrames; i < c; i++) {
            const SpriteSheetFrame& sheetFrame = sheet->frames[i];
            SpriteFrame* frame = new(&frames[i]) SpriteFrame();

            frame->texHandle = sheet->texHandle;
            frame->ssHandle = ssHandle;
            frame->nameHash = sheetFrame.filenameHash;
            frame->tagHash = 0;
            frame->flags = flags;
            frame->pivot = sheetFrame.pivot;
            frame->frame = sheetFrame.frame;
            frame->sourceSize = sheetFrame.sourceSize;
            frame->posOffset = sheetFrame.posOffset;
            frame->sizeOffset = sheetFrame.sizeOffset;
            frame->rotOffset = sheetFrame.rotOffset;
            frame->pixelRatio = sheetFrame.pixelRatio;
        }
    }
}

void termite::animateSprites(Sprite** sprites, uint16_t numSprites, float dt)
{
    for (int i = 0; i < numSprites; i++) {
        Sprite* sprite = sprites[i];
        bool playReverse = sprite->playReverse;

        if (!bx::fequal(sprite->playSpeed, 0, 0.00001f)) {
            float t = sprite->animTm;
            t += dt;
            float progress = t * sprite->playSpeed;
            float frames = bx::ffloor(progress);
            float reminder = frames > 0 ? bx::fmod(progress, frames) : progress;
            t = reminder / sprite->playSpeed;

            // Progress sprite frame
            int curFrameIdx = sprite->curFrameIdx;
            int frameIdx = curFrameIdx;
            int iframes = int(frames);
            if (sprite->endCallback == nullptr) {
                frameIdx = iwrap(!playReverse ? (frameIdx + iframes) : (frameIdx - iframes),
                                 0, sprite->frames.getCount() - 1);
            } else {
                if (sprite->triggerEndCallback && iframes > 0) {
                    sprite->triggerEndCallback = false;
                    sprite->endCallback(sprite, frameIdx, sprite->endUserData);
                }

                int nextFrame = !playReverse ? (frameIdx + iframes) : (frameIdx - iframes);
                frameIdx = iclamp(nextFrame, 0, sprite->frames.getCount() - 1);

                if (frameIdx != nextFrame && sprite->playSpeed != 0) {
                    sprite->triggerEndCallback = true;  // Tigger callback on the next update
                }
            }

            // Check if we hit any callbacks
            const SpriteFrame& frame = sprite->frames[frameIdx];

            if (frame.frameCallback)
                frame.frameCallback(sprite, frameIdx, frame.frameCallbackUserData);

            // Update the frame index only if it's not modified inside any callbacks
            if (curFrameIdx == sprite->curFrameIdx)
                sprite->curFrameIdx = frameIdx;
            sprite->animTm = t;
        }
    }
}

void termite::invertSpriteAnim(Sprite* sprite)
{
    sprite->playReverse = !sprite->playReverse;
}

void termite::setSpriteAnimSpeed(Sprite* sprite, float speed)
{
    sprite->playSpeed = sprite->resumeSpeed = speed;
}

float termite::getSpriteAnimSpeed(Sprite* sprite)
{
    return sprite->resumeSpeed;
}

void termite::pauseSpriteAnim(Sprite* sprite)
{
    sprite->playSpeed = 0;
}

void termite::resumeSpriteAnim(Sprite* sprite)
{
    sprite->playSpeed = sprite->resumeSpeed;
}

void termite::stopSpriteAnim(Sprite* sprite)
{
    sprite->triggerEndCallback = false;
    sprite->curFrameIdx = 0;
    sprite->playSpeed = 0;
}

void termite::replaySpriteAnim(Sprite* sprite)
{
    sprite->triggerEndCallback = false;
    sprite->curFrameIdx = 0;
    sprite->playSpeed = sprite->resumeSpeed;
}

void termite::setSpriteFrameCallbackByTag(Sprite* sprite, const char* frameTag, SpriteFrameCallback callback, void* userData)
{
    assert(frameTag);
    size_t frameTagHash = tinystl::hash_string(frameTag, strlen(frameTag));
    for (int i = 0, c = sprite->frames.getCount(); i < c; i++) {
        if (sprite->frames[i].tagHash != frameTagHash)
            continue;
        SpriteFrame* frame = sprite->frames.itemPtr(i);
        frame->frameCallback = callback;
        frame->frameCallbackUserData = userData;
    }
}

void termite::setSpriteFrameCallbackByName(Sprite* sprite, const char* name, SpriteFrameCallback callback, void* userData)
{
    size_t nameHash = tinystl::hash_string(name, strlen(name));
    for (int i = 0, c = sprite->frames.getCount(); i < c; i++) {
        if (sprite->frames[i].nameHash != nameHash)
            continue;
        SpriteFrame* frame = sprite->frames.itemPtr(i);
        frame->frameCallback = callback;
        frame->frameCallbackUserData = userData;
    }
}

void termite::setSpriteFrameCallbackByIndex(Sprite* sprite, int frameIdx, SpriteFrameCallback callback, void* userData)
{
    assert(frameIdx < sprite->frames.getCount());
    SpriteFrame* frame = sprite->frames.itemPtr(frameIdx);
    frame->frameCallback = callback;
    frame->frameCallbackUserData = userData;
}

void termite::setSpriteFrameEndCallback(Sprite* sprite, SpriteFrameCallback callback, void* userData)
{
    sprite->endCallback = callback;
    sprite->endUserData = userData;
    sprite->triggerEndCallback = false;
}

void termite::setSpriteHalfSize(Sprite* sprite, const vec2_t& halfSize)
{
    sprite->halfSize = halfSize;
}

vec2_t termite::getSpriteHalfSize(Sprite* sprite)
{
    return sprite->halfSize;
}

void termite::setSpriteSizeMultiplier(Sprite* sprite, const vec2_t& sizeMultiplier)
{
    sprite->sizeMultiplier = sizeMultiplier;
}

void termite::gotoSpriteFrameIndex(Sprite* sprite, int frameIdx)
{
    assert(frameIdx < sprite->frames.getCount());
    sprite->curFrameIdx = frameIdx;
}

void termite::gotoSpriteFrameName(Sprite* sprite, const char* name)
{
    size_t nameHash = tinystl::hash_string(name, strlen(name));
    int curIdx = sprite->curFrameIdx;
    for (int i = 0, c = sprite->frames.getCount(); i < c;  i++) {
        int idx = (i + curIdx) % c;
        if (sprite->frames[idx].nameHash == nameHash) {
            sprite->curFrameIdx = idx;
            return;
        }
    }
}

void termite::gotoSpriteFrameTag(Sprite* sprite, const char* frameTag)
{
    assert(frameTag);
    size_t tagHash = tinystl::hash_string(frameTag, strlen(frameTag));
    int curIdx = sprite->curFrameIdx;
    for (int i = 0, c = sprite->frames.getCount(); i < c; i++) {
        int idx = (i + curIdx) % c;
        if (sprite->frames[idx].tagHash == tagHash) {
            sprite->curFrameIdx = idx;
            return;
        }
    }
}

int termite::getSpriteFrameIndex(Sprite* sprite)
{
    return sprite->curFrameIdx;
}

int termite::getSpriteFrameCount(Sprite* sprite)
{
    return sprite->frames.getCount();
}

void termite::setSpriteFrameIndex(Sprite* sprite, int index)
{
    assert(index < sprite->frames.getCount());
    sprite->curFrameIdx = index;
}

void termite::setSpriteFlip(Sprite* sprite, SpriteFlag::Bits flip)
{
    sprite->flip = flip;
}

SpriteFlip::Bits termite::getSpriteFlip(Sprite* sprite)
{
    return sprite->flip;
}

void termite::setSpritePosOffset(Sprite* sprite, const vec2_t posOffset)
{
    sprite->posOffset = posOffset;
}

vec2_t termite::getSpritePosOffset(Sprite* sprite)
{
    return sprite->posOffset;
}

void termite::setSpriteCurFrameTag(Sprite* sprite, const char* frameTag)
{
    SpriteFrame& frame = sprite->frames[sprite->curFrameIdx];
    frame.tagHash = tinystl::hash_string(frameTag, strlen(frameTag));
}

void termite::setSpriteOrder(Sprite* sprite, uint8_t order)
{
    sprite->order = order;
}

int termite::getSpriteOrder(Sprite* sprite)
{
    return sprite->order;
}

void termite::setSpritePivot(Sprite* sprite, const vec2_t pivot)
{
    for (int i = 0, c = sprite->frames.getCount(); i < c; i++)
        sprite->frames[i].pivot = pivot;
}

void termite::setSpriteTintColor(Sprite* sprite, color_t color)
{
    sprite->tint = color;
}

color_t termite::getSpriteTintColor(Sprite* sprite)
{
    return sprite->tint;
}

static rect_t getSpriteDrawRectFrame(Sprite* sprite, int index)
{
    const SpriteFrame& frame = sprite->frames[index];
    vec2_t halfSize = sprite->halfSize;
    float pixelRatio = frame.pixelRatio;
    if (halfSize.y <= 0)
        halfSize.y = halfSize.x / pixelRatio;
    else if (halfSize.x <= 0)
        halfSize.x = halfSize.y * pixelRatio;
    halfSize = halfSize * sprite->sizeMultiplier;
    vec2_t fullSize = halfSize * 2.0f;

    // calculate final pivot offset to make geometry
    SpriteFlag::Bits flipX = sprite->flip | frame.flags;
    SpriteFlag::Bits flipY = sprite->flip | frame.flags;

    vec2_t offset = frame.posOffset + sprite->posOffset - frame.pivot;
    if (flipX & SpriteFlip::FlipX)
        offset.x = -offset.x;
    if (flipY & SpriteFlip::FlipY)
        offset.y = -offset.y;

    halfSize = halfSize * frame.sizeOffset;
    offset = offset * fullSize;

    return rectv(offset - halfSize, halfSize + offset);
}

rect_t termite::getSpriteDrawRect(Sprite* sprite)
{
    return getSpriteDrawRectFrame(sprite, sprite->curFrameIdx);
}

void termite::getSpriteRealRect(Sprite* sprite, vec2_t* pHalfSize, vec2_t* pCenter)
{
    const SpriteFrame& frame = sprite->getCurFrame();
    SpriteFlip::Bits flip = sprite->flip & frame.flags;
    vec2_t halfSize = sprite->halfSize;
    float pixelRatio = frame.pixelRatio;
    if (halfSize.y <= 0)
        halfSize.y = halfSize.x / pixelRatio;
    else if (halfSize.x <= 0)
        halfSize.x = halfSize.y * pixelRatio;
    vec2_t pivot = frame.pivot;
    if (flip & SpriteFlip::FlipX)
        pivot.x = -pivot.x;
    if (flip & SpriteFlip::FlipY)
        pivot.y = -pivot.y;

    *pHalfSize = halfSize;
    *pCenter = pivot * halfSize * 2.0f;
}

vec2_t termite::getSpriteImageSize(Sprite* sprite)
{
    return sprite->getCurFrame().sourceSize;
}

rect_t termite::getSpriteTexelRect(Sprite* sprite)
{
    return sprite->getCurFrame().frame;
}

ProgramHandle termite::getSpriteColorAddProgram()
{
    return g_spriteSys->spriteAddProg;
}

void termite::setSpriteUserData(Sprite* sprite, void* userData)
{
    sprite->userData = userData;
}

void* termite::getSpriteUserData(Sprite* sprite)
{
    return sprite->userData;
}

void termite::getSpriteFrameDrawData(Sprite* sprite, int frameIdx, rect_t* drawRect, rect_t* textureRect, 
                                     ResourceHandle* textureHandle)
{
    *drawRect = getSpriteDrawRectFrame(sprite, frameIdx);
    *textureRect = sprite->frames[frameIdx].frame;
    *textureHandle = sprite->frames[frameIdx].texHandle;
}

void termite::convertSpritePhysicsVerts(vec2_t* ptsOut, const vec2_t* ptsIn, int numPts, Sprite* sprite)
{
    vec2_t halfSize;
    vec2_t center;
    vec2_t imgSize = getSpriteImageSize(sprite);
    getSpriteRealRect(sprite, &halfSize, &center);
    SpriteFlip::Bits flip = sprite->flip;

    for (int i = 0; i < numPts; i++) {
        vec2_t pt = ptsIn[i];
        if (flip & SpriteFlip::FlipX)
            pt.x = -pt.x;
        if (flip & SpriteFlip::FlipY)
            pt.y = -pt.y;

        pt = vec2f(pt.x/imgSize.x, pt.y/imgSize.y);
        ptsOut[i] = pt * halfSize * 2.0f - center;
    }
}

void termite::drawSprites(uint8_t viewId, Sprite** sprites, uint16_t numSprites, const mtx3x3_t* mats,
                          ProgramHandle progOverride /*= ProgramHandle()*/, SetSpriteStateCallback stateCallback /*= nullptr*/,
                          void* stateUserData, const color_t* colors)
{
    assert(sprites);
    assert(mats);

    if (numSprites <= 0)
        return;

    GfxDriverApi* driver = g_spriteSys->driver;
    bx::AllocatorI* tmpAlloc = getTempAlloc();

    TransientVertexBuffer tvb;
    TransientIndexBuffer tib;
    const int numVerts = numSprites * 4;
    const int numIndices = numSprites * 6;
    GfxState::Bits baseState = gfxStateBlendAlpha() | GfxState::RGBWrite | GfxState::AlphaWrite | GfxState::CullCCW;

    if (driver->getAvailTransientVertexBuffer(numVerts, SpriteVertex::Decl) != numVerts)
        return;
    driver->allocTransientVertexBuffer(&tvb, numVerts, SpriteVertex::Decl);

    if (driver->getAvailTransientIndexBuffer(numIndices) != numIndices)
        return;
    driver->allocTransientIndexBuffer(&tib, numIndices);

    // Sort sprites by texture and batch them
    struct SortedSprite
    {
        int index;
        Sprite* sprite;
        uint64_t key;
    };

    SortedSprite* sortedSprites = (SortedSprite*)BX_ALLOC(tmpAlloc, sizeof(SortedSprite)*numSprites);
    for (int i = 0; i < numSprites; i++) {
        const SpriteFrame& frame = sprites[i]->getCurFrame();
        sortedSprites[i].index = i;
        sortedSprites[i].sprite = sprites[i];
        sortedSprites[i].key = MAKE_SPRITE_KEY(sprites[i]->order, frame.texHandle.value, sprites[i]->id);
    }
    std::sort(sortedSprites, sortedSprites + numSprites, [](const SortedSprite& a, const SortedSprite&b)->bool {
        return a.key < b.key;
    });

    // Fill sprite quads
    SpriteVertex* verts = (SpriteVertex*)tvb.data;
    uint16_t* indices = (uint16_t*)tib.data;
    int indexIdx = 0;
    int vertexIdx = 0;
    for (int i = 0; i < numSprites; i++) {
        const SortedSprite& ss = sortedSprites[i];
        const mtx3x3_t& mat = mats[ss.index];
        const SpriteFrame& frame = ss.sprite->getCurFrame();
        vec2_t halfSize = ss.sprite->halfSize;
        rect_t texRect = frame.frame;
        float pixelRatio = frame.pixelRatio;
        SpriteFlag::Bits flipX = ss.sprite->flip | frame.flags;
        SpriteFlag::Bits flipY = ss.sprite->flip | frame.flags;

        if (halfSize.y <= 0)
            halfSize.y = halfSize.x / pixelRatio;
        else if (halfSize.x <= 0)
            halfSize.x = halfSize.y * pixelRatio;
        halfSize = halfSize * ss.sprite->sizeMultiplier;

        // calculate final pivot offset to make geometry
        vec2_t fullSize = halfSize * 2.0f;
        vec2_t offset = frame.posOffset + ss.sprite->posOffset - frame.pivot;
        if (flipX & SpriteFlip::FlipX)
            offset.x = -offset.x;
        if (flipY & SpriteFlip::FlipY)
            offset.y = -offset.y;

        // shrink and offset to match the image inside sprite
        halfSize = halfSize * frame.sizeOffset;
        offset = offset * fullSize;

        // Encode transform matrix into vertices
        vec3_t transform1 = vec3f(mat.m11, mat.m12, mat.m21);
        vec3_t transform2 = vec3f(mat.m22, mat.m31, mat.m32);

        SpriteVertex& v0 = verts[vertexIdx];
        SpriteVertex& v1 = verts[vertexIdx + 1];
        SpriteVertex& v2 = verts[vertexIdx + 2];
        SpriteVertex& v3 = verts[vertexIdx + 3];

        // Top-Left
        v0.pos = vec2f(-halfSize.x, halfSize.y) + offset;
        v0.coords = vec2f(texRect.xmin, texRect.ymin);

        // Top-Right
        v1.pos = vec2f(halfSize.x, halfSize.y) + offset;
        v1.coords = vec2f(texRect.xmax, texRect.ymin);

        // Bottom-Left
        v2.pos = vec2f(-halfSize.x, -halfSize.y) + offset;
        v2.coords = vec2f(texRect.xmin, texRect.ymax);

        // Bottom-Right
        v3.pos = vec2f(halfSize.x, -halfSize.y) + offset;
        v3.coords = vec2f(texRect.xmax, texRect.ymax);

        v0.transform1 = v1.transform1 = v2.transform1 = v3.transform1 = transform1;
        v0.transform2 = v1.transform2 = v2.transform2 = v3.transform2 = transform2;

        if (!colors)
            v0.color = v1.color = v2.color = v3.color = ss.sprite->tint.n;
        else 
            v0.color = v1.color = v2.color = v3.color = colors[ss.index].n;

        if (flipX & SpriteFlip::FlipX) {
            std::swap<float>(v0.coords.x, v1.coords.x);
            std::swap<float>(v2.coords.x, v3.coords.x);
        }

        if (flipY & SpriteFlip::FlipY) {
            std::swap<float>(v0.coords.y, v2.coords.y);
            std::swap<float>(v1.coords.y, v3.coords.y);
        }

        // Make a quad from 4 verts
        indices[indexIdx] = vertexIdx;
        indices[indexIdx + 1] = vertexIdx + 1;
        indices[indexIdx + 2] = vertexIdx + 2;
        indices[indexIdx + 3] = vertexIdx + 2;
        indices[indexIdx + 4] = vertexIdx + 1;
        indices[indexIdx + 5] = vertexIdx + 3;

        indexIdx += 6;
        vertexIdx += 4;
    }

    // Batch by texture
    struct Batch
    {
        int index;
        int count;
    };
    bx::Array<Batch> batches;
    batches.create(32, 64, tmpAlloc);

    uint32_t prevKey = UINT32_MAX;
    Batch* curBatch = nullptr;
    for (int i = 0; i < numSprites; i++) {
        uint32_t batchKey = SPRITE_KEY_GET_BATCH(sortedSprites[i].key);
        if (batchKey != prevKey) {
            curBatch = batches.push();
            curBatch->index = i;
            curBatch->count = 0;
            prevKey = batchKey;
        }
        curBatch->count++;
    }

    // Draw
    ProgramHandle prog = !progOverride.isValid() ? g_spriteSys->spriteProg : progOverride;
    for (int i = 0, c = batches.getCount(); i < c; i++) {
        const Batch batch = batches[i];
        driver->setState(baseState, 0);
        driver->setTransientVertexBufferI(&tvb, 0, batch.count*4);
        driver->setTransientIndexBufferI(&tib, batch.index*6, batch.count*6);
        Sprite* sprite = sortedSprites[batch.index].sprite;
        ResourceHandle texHandle = sprite->getCurFrame().texHandle;
        if (texHandle.isValid()) {
            driver->setTexture(0, g_spriteSys->u_texture, getResourcePtr<Texture>(texHandle)->handle, 
                               TextureFlag::FromTexture);
        }

        if (stateCallback) {
            stateCallback(driver, stateUserData);
        }
        driver->submit(viewId, prog, 0, false);
    }    

    batches.destroy();
}


void termite::registerSpriteSheetToResourceLib()
{
    ResourceTypeHandle handle;
    handle = registerResourceType("spritesheet", &g_spriteSys->loader, sizeof(LoadSpriteSheetParams), 
                                  uintptr_t(g_spriteSys->failSheet), uintptr_t(g_spriteSys->asyncSheet));
    assert(handle.isValid());
}

rect_t termite::getSpriteSheetTextureFrame(ResourceHandle spritesheet, const char* name)
{
    SpriteSheet* ss = getResourcePtr<SpriteSheet>(spritesheet);
    size_t filenameHash = tinystl::hash_string(name, strlen(name));
    for (int i = 0, c = ss->numFrames; i < c; i++) {
        if (ss->frames[i].filenameHash != filenameHash)
            continue;
        
        return ss->frames[i].frame;
    }
    return rectf(0, 0, 1.0f, 1.0f);
}

ResourceHandle termite::getSpriteSheetTexture(ResourceHandle spritesheet)
{
    SpriteSheet* ss = getResourcePtr<SpriteSheet>(spritesheet);
    return ss->texHandle;
}
