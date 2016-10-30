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
#include "bxx/json.h"

#include "../include_common/sprite_format.h"
#include "shaders_h/sprite.vso"
#include "shaders_h/sprite.fso"

#include <algorithm>

using namespace termite;

struct SpriteVertex
{
    float x;
    float y;
    float rot;
    float scale;
    float offsetx;
    float offsety;
    float pivotx;
    float pivoty;
    float tx;
    float ty;
    uint32_t color;

    static void init()
    {
        vdeclBegin(&Decl);
        vdeclAdd(&Decl, VertexAttrib::Position, 4, VertexAttribType::Float);        // pos
        vdeclAdd(&Decl, VertexAttrib::TexCoord0, 4, VertexAttribType::Float);       // offset/pivot
        vdeclAdd(&Decl, VertexAttrib::TexCoord1, 2, VertexAttribType::Float);       // texture coords
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
    bool destroyResource;

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
        frameCallback(nullptr),
        frameCallbackUserData(nullptr),
        nameHash(0),
        tagHash(0)
    {
    }
};

namespace termite
{
    struct Sprite
    {
        typedef bx::List<Sprite*>::Node LNode;

        bx::AllocatorI* alloc;
        vec2_t halfSize;
        bx::Array<SpriteFrame> frames;
        int curFrameIdx;

        float animTm;       // Animation timer
        bool playReverse;
        float playSpeed;
        float resumeSpeed;
        color_t tint;
        LNode lnode;

        Sprite(bx::AllocatorI* _alloc) :
            alloc(_alloc),
            animTm(0),
            playReverse(false),
            playSpeed(30.0f),
            resumeSpeed(30.0f),
            curFrameIdx(0),
            tint(0xffffffff),
            lnode(this)
        {
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
    UniformHandle u_texture;
    SpriteSheetLoader loader;
    SpriteSheet* failSheet;
    SpriteSheet* asyncSheet;
    bx::List<Sprite*> spriteList;       // keep a list of sprites for proper shutdown and sheet reloading

    SpriteSystem(bx::AllocatorI* _alloc) : 
        alloc(_alloc),
        driver(nullptr),
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

    bx::JsonError err;
    bx::JsonNodeAllocator jalloc(tmpAlloc);
    bx::JsonNode* jroot = bx::parseJson(jsonStr, &jalloc, &err);
    if (!jroot) {
        T_ERROR("Parse Json Error: %s (Pos: %s, Line: %d)", err.desc, err.pos, err.line);
        return false;
    }
    BX_FREE(tmpAlloc, jsonStr);

    const bx::JsonNode* jframes = jroot->findChild("frames");
    const bx::JsonNode* jmeta = jroot->findChild("meta");
    if (jframes->isNull() || jmeta->isNull()) {
        T_ERROR("SpriteSheet Json is Invalid");
        jroot->destroy();
        return false;
    }

    int numFrames = jframes->getArrayCount();
    if (numFrames == 0) {
        T_ERROR("Parse Json Error: %s (Pos: %s, Line: %d)", err.desc, err.pos, err.line);
        jroot->destroy();
        return false;
    }

    // Create sprite sheet
    size_t totalSz = sizeof(SpriteSheet) + numFrames*sizeof(SpriteSheetFrame);
    uint8_t* buff = (uint8_t*)BX_ALLOC(alloc ? alloc : g_spriteSys->alloc, totalSz);
    if (!buff) {
        T_ERROR("Out of Memory");
        jroot->destroy();
        return false;
    }
    SpriteSheet* ss = new(buff) SpriteSheet();  buff += sizeof(SpriteSheet);
    ss->frames = (SpriteSheetFrame*)buff;
    ss->numFrames = numFrames;

    // image width/height
    const bx::JsonNode* jsize = jmeta->findChild("size");
    float imgWidth = float(jsize->findChild("w")->valueInt());
    float imgHeight = float(jsize->findChild("h")->valueInt());

    // Make texture path and load it
    const char* imageFile = jmeta->findChild("image")->valueString();
    bx::Path texFilepath = bx::Path(params.uri).getDirectory();
    texFilepath.joinUnix(imageFile);

    LoadTextureParams texParams;
    texParams.flags = ssParams->flags;
    texParams.generateMips = ssParams->generateMips;
    texParams.skipMips = ssParams->skipMips;
    ss->texHandle = loadResource("texture", texFilepath.cstr(), &texParams);

    for (int i = 0; i < numFrames; i++) {
        SpriteSheetFrame& frame = ss->frames[i];
        const bx::JsonNode* jframe = jframes->getArrayItem(i);
        const char* filename = jframe->findChild("filename")->valueString();
        frame.filenameHash = tinystl::hash_string(filename, strlen(filename));
        bool rotated = jframe->findChild("rotated")->valueBool();

        const bx::JsonNode* jframeFrame = jframe->findChild("frame");
        float frameWidth = float(jframeFrame->findChild("w")->valueInt());
        float frameHeight = float(jframeFrame->findChild("h")->valueInt());
        if (rotated)
            std::swap<float>(frameWidth, frameHeight);

        frame.frame = rectfwh(float(jframeFrame->findChild("x")->valueInt()) / imgWidth,
                              float(jframeFrame->findChild("y")->valueInt()) / imgHeight,
                              frameWidth / imgWidth,
                              frameHeight / imgHeight);

        const bx::JsonNode* jsourceSize = jframe->findChild("sourceSize");
        frame.sourceSize = vec2f(float(jsourceSize->findChild("w")->valueInt()),
                                 float(jsourceSize->findChild("h")->valueInt()));

        // Normalize pos/size offsets (0~1)
        // Rotate offset can only be 90 degrees
        const bx::JsonNode* jssFrame = jframe->findChild("spriteSourceSize");
        float srcx = float(jssFrame->findChild("x")->valueInt());
        float srcy = float(jssFrame->findChild("y")->valueInt());
        float srcw = float(jssFrame->findChild("w")->valueInt());
        float srch = float(jssFrame->findChild("h")->valueInt());

        frame.sizeOffset = vec2f(srcw / frame.sourceSize.x, srch / frame.sourceSize.y);

        if (!rotated) {
            frame.rotOffset = 0;
        } else {
            std::swap<float>(srcw, srch);
            frame.rotOffset = -90.0f;
        }
        frame.pixelRatio = frame.sourceSize.x / frame.sourceSize.y;

        const bx::JsonNode* jpivot = jframe->findChild("pivot");
        const bx::JsonNode* jpivotX = jpivot->findChild("x");
        const bx::JsonNode* jpivotY = jpivot->findChild("y");
        float pivotx = jpivotX->getType() == bx::JsonType::Float ?  jpivotX->valueFloat() : float(jpivotX->valueInt());
        float pivoty = jpivotY->getType() == bx::JsonType::Float ?  jpivotX->valueFloat() : float(jpivotY->valueInt());
        pivotx = pivoty = 0.5f;
        frame.pivot = vec2f(pivotx - 0.5f, -pivoty + 0.5f);     // convert to our coordinates

        vec2_t srcPivot = vec2f( (srcx + 0.5f*srcw)/frame.sourceSize.x - 0.5f,
                                -(srcy + 0.5f*srch)/frame.sourceSize.y + 0.5f);
        frame.posOffset = srcPivot;
    }

    jroot->destroy();
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
    Sprite* sprite = BX_NEW(alloc, Sprite)(alloc);
    if (!sprite)
        return nullptr;
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
        if (frame.destroyResource) {
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

void termite::addSpriteFrameTexture(Sprite* sprite, 
                                    ResourceHandle texHandle, bool destroy /*= false*/, const vec2_t pivot /*= vec2f(0, 0)*/, 
                                    const vec2_t topLeftCoords /*= vec2f(0, 0)*/, const vec2_t bottomRightCoords /*= vec2f(1.0f, 1.0f)*/, 
                                    const char* frameTag /*= nullptr*/)
{
    if (texHandle.isValid()) {
        assert(getResourceLoadState(texHandle) != ResourceLoadState::LoadInProgress);
 
        SpriteFrame* frame = new(sprite->frames.push()) SpriteFrame();
        if (frame) {
            frame->texHandle = texHandle;
            frame->destroyResource = destroy;
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
                                        ResourceHandle ssHandle, const char* name, bool destroy /*= false*/, 
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
            frame->destroyResource = destroy;
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

void termite::addSpriteFrameAll(Sprite* sprite, ResourceHandle ssHandle, bool destroy)
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
            frame->destroyResource = destroy;
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
        float t = sprite->animTm;
        t += dt;
        float progress = t * sprite->playSpeed;
        float frames = bx::ffloor(progress);
        float reminder = frames > 0 ? bx::fmod(progress, frames) : progress;
        t = reminder / sprite->playSpeed;

        // Progress sprite frame
        int frameIdx = sprite->curFrameIdx;
        int iframes = int(frames);
        frameIdx = iwrap(!sprite->playReverse ? (frameIdx + iframes) : (frameIdx - iframes), 
                         0, sprite->frames.getCount() - 1);

        // Check if we hit any callbacks
        const SpriteFrame& frame = sprite->frames[frameIdx];
        if (frame.frameCallback)
            frame.frameCallback(sprite, frame.frameCallbackUserData);

        sprite->curFrameIdx = frameIdx;
        sprite->animTm = t;
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
    sprite->curFrameIdx = 0;
    sprite->playSpeed = 0;
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

void termite::setSpriteTintColor(Sprite* sprite, color_t color)
{
    sprite->tint = color;
}

color_t termite::getSpriteTintColor(Sprite* sprite)
{
    return sprite->tint;
}

void termite::drawSprites(uint8_t viewId, Sprite** sprites, uint16_t numSprites, const SpriteTransform* transforms,
                          ProgramHandle progOverride /*= ProgramHandle()*/, SetSpriteStateCallback stateCallback /*= nullptr*/)
{
    assert(sprites);
    assert(transforms);

    if (numSprites <= 0)
        return;

    GfxDriverApi* driver = g_spriteSys->driver;
    bx::AllocatorI* tmpAlloc = getTempAlloc();
    TransientVertexBuffer tvb;
    TransientIndexBuffer tib;
    const int numVerts = numSprites * 4;
    const int numIndices = numSprites * 6;
    GfxState::Bits baseState = gfxStateBlendAlpha() | GfxState::RGBWrite | GfxState::AlphaWrite | GfxState::CullCCW;

    if (!driver->checkAvailTransientVertexBuffer(numVerts, SpriteVertex::Decl))
        return;
    driver->allocTransientVertexBuffer(&tvb, numVerts, SpriteVertex::Decl);

    if (!driver->checkAvailTransientIndexBuffer(numIndices))
        return;
    driver->allocTransientIndexBuffer(&tib, numIndices);

    // Sort sprites by texture and batch them
    struct SortedSprite
    {
        int index;
        Sprite* sprite;
    };

    SortedSprite* sortedSprites = (SortedSprite*)BX_ALLOC(tmpAlloc, sizeof(SortedSprite)*numSprites);
    for (int i = 0; i < numSprites; i++) {
        sortedSprites[i].index = i;
        sortedSprites[i].sprite = sprites[i];
    }
    std::sort(sortedSprites, sortedSprites + numSprites, [](const SortedSprite& a, const SortedSprite&b)->bool {
        const SpriteFrame& fa = a.sprite->getCurFrame();
        const SpriteFrame& fb = a.sprite->getCurFrame();
        return fa.texHandle.value < fb.texHandle.value;
    });

    // Fill sprite quads
    SpriteVertex* verts = (SpriteVertex*)tvb.data;
    uint16_t* indices = (uint16_t*)tib.data;
    int indexIdx = 0;
    int vertexIdx = 0;
    for (int i = 0; i < numSprites; i++) {
        const SortedSprite& ss = sortedSprites[i];
        const SpriteTransform transform = transforms[ss.index];
        const SpriteFrame& frame = ss.sprite->getCurFrame();
        vec2_t halfSize = ss.sprite->halfSize;
        rect_t texRect = frame.frame;
        float pixelRatio = frame.pixelRatio;

        if (halfSize.y <= 0)
            halfSize.y = halfSize.x / pixelRatio;
        else if (halfSize.x <= 0)
            halfSize.x = halfSize.y * pixelRatio;

        vec2_t fullSize = vec2f(halfSize.x*2.0f, halfSize.y*2.0f);
        float scale = transform.scale;
        float rot = frame.rotOffset + transform.rot;
        vec2_t offset = frame.posOffset * fullSize;
        vec2_t pos = vec2f(transform.x, transform.y) + offset;
        vec2_t pivot = frame.pivot;

        pivot = pivot * halfSize * 2.0f;
        halfSize = halfSize * frame.sizeOffset;

        SpriteVertex& v0 = verts[vertexIdx];
        SpriteVertex& v1 = verts[vertexIdx + 1];
        SpriteVertex& v2 = verts[vertexIdx + 2];
        SpriteVertex& v3 = verts[vertexIdx + 3];

        // Top-Left
        v0.x = -halfSize.x;       v0.y = halfSize.y;
        v0.rot = rot;             v0.scale = scale;
        v0.offsetx = pos.x;       v0.offsety = pos.y;
        v0.pivotx = pivot.x;      v0.pivoty = pivot.y;
        v0.tx = texRect.xmin;     v0.ty = texRect.ymin;

        // Top-Right
        v1.x = halfSize.x;        v1.y = halfSize.y;
        v1.rot = rot;             v1.scale = scale;
        v1.offsetx = pos.x;       v1.offsety = pos.y;
        v1.pivotx = pivot.x;      v1.pivoty = pivot.y;
        v1.tx = texRect.xmax;     v1.ty = texRect.ymin;

        // Bottom-Left
        v2.x = -halfSize.x;       v2.y = -halfSize.y;
        v2.rot = rot;             v2.scale = scale;
        v2.offsetx = pos.x;       v2.offsety = pos.y;
        v2.pivotx = pivot.x;      v2.pivoty = pivot.y;
        v2.tx = texRect.xmin;     v2.ty = texRect.ymax;

        // Bottom-Right
        v3.x = halfSize.x;        v3.y = -halfSize.y;
        v3.rot = rot;             v3.scale = scale;
        v3.offsetx = pos.x;       v3.offsety = pos.y;
        v3.pivotx = pivot.x;      v3.pivoty = pivot.y;
        v3.tx = texRect.xmax;     v3.ty = texRect.ymax;

        v0.color = v1.color = v2.color = v3.color = ss.sprite->tint;

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

    ResourceHandle prevHandle;
    Batch* curBatch = nullptr;
    for (int i = 0; i < numSprites; i++) {
        Sprite* sprite = sortedSprites[i].sprite;
        ResourceHandle curHandle = sprite->getCurFrame().texHandle;
        if (curHandle.value != prevHandle.value) {
            curBatch = batches.push();
            curBatch->index = i;
            curBatch->count = 0;
            prevHandle = curHandle;
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

        if (stateCallback)
            stateCallback(driver);
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
