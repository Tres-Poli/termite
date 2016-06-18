#include "pch.h"

#include "gfx_driver.h"
#include "gfx_font.h"
#include "gfx_texture.h"
#include "gfx_debug.h"
#include "gfx_vg.h"
#include "camera.h"

#include "bx/uint32_t.h"
#include "bxx/pool.h"
#include "bxx/stack.h"

#include <cstdarg>

#include "shaders_h/dbg.vso"
#include "shaders_h/dbg.fso"

#define STATE_POOL_SIZE 8
#define MAX_TEXT_SIZE 256

using namespace termite;

struct dbgVertexPosCoordColor
{
    float x, y, z;
    float tx, ty;

    void setPos(float _x, float _y, float _z)
    {
        x = _x;     y = _y;     z = _z;
    }

    void setPos(const vec3_t& p)
    {
        x = p.x;    y = p.y;    z = p.z;
    }

    static void init()
    {
        Decl.begin()
            .add(gfxAttrib::Position, 3, gfxAttribType::Float)
            .add(gfxAttrib::TexCoord0, 2, gfxAttribType::Float)
            .end();
    }

    static gfxVertexDecl Decl;
};
gfxVertexDecl dbgVertexPosCoordColor::Decl;

class BX_NO_VTABLE DrawHandler
{
public:
    virtual result_t init(bx::AllocatorI* alloc, gfxDriverI* driver) = 0;
    virtual void shutdown() = 0;

    virtual uint32_t getHash(const void* params) = 0;
    virtual gfxState setStates(vgContext* ctx, gfxDriverI* driver, const void* params) = 0;
};

struct State
{
    mtx4x4_t mtx;
    vec4_t color;
    float alpha;
    rect_t scissor;
    const fntFont* font;

    typedef bx::StackNode<State*> SNode;
    SNode snode;
};

namespace termite
{
    struct dbgContext
    {
        bx::AllocatorI* alloc;
        gfxDriverI* driver;
        uint8_t viewId;
        bx::FixedPool<State> statePool;
        State::SNode* stateStack;
        rect_t viewport;
        const fntFont* defaultFont;
        bool readyToDraw;
        vgContext* vgCtx;
        Camera* cam;
        mtx4x4_t billboardMtx;
        mtx4x4_t viewProjMtx;

        dbgContext(bx::AllocatorI* _alloc) : alloc(_alloc)
        {
            driver = nullptr;
            viewId = 0;
            stateStack = nullptr;
            viewport = rectf(0, 0, 0, 0);
            defaultFont = nullptr;
            readyToDraw = false;
            vgCtx = nullptr;
            cam = nullptr;
            viewProjMtx = mtx4x4Ident();
            billboardMtx = mtx4x4Ident();
        }
    };
} // namespace termite

struct Shape
{
    gfxVertexBufferHandle vb;
    uint32_t numVerts;

    Shape()
    {
        vb = T_INVALID_HANDLE;
        numVerts = 0;
    }

    Shape(gfxVertexBufferHandle _vb, uint32_t _numVerts)
    {
        vb = _vb;
        numVerts = _numVerts;
    }
};

struct dbgMgr
{
    gfxDriverI* driver;
    bx::AllocatorI* alloc;
    gfxProgramHandle program;
    gfxTextureHandle whiteTexture;

    gfxUniformHandle uTexture;
    gfxUniformHandle uColor;

    Shape bbShape;
    Shape bsphereShape;
    Shape sphereShape;

    dbgMgr(bx::AllocatorI* _alloc) : alloc(_alloc)
    {
        driver = nullptr;
        program = T_INVALID_HANDLE;
        whiteTexture = T_INVALID_HANDLE;
        uTexture = T_INVALID_HANDLE;
        uColor = T_INVALID_HANDLE;
    }
};

static dbgMgr* g_dbg = nullptr;

static bool projectToScreen(vec2_t* result, const vec3_t point, const rect_t& rect, const mtx4x4_t& viewProjMtx)
{
    float w = rect.xmax - rect.xmin;
    float h = rect.ymax - rect.ymin;
    float wh = w*0.5f;
    float hh = h*0.5f;

    vec4_t proj;
    bx::vec4MulMtx(proj.f, vec4f(point.x, point.y, point.z, 1.0f).f, viewProjMtx.f);
    bx::vec3Mul(proj.f, proj.f, 1.0f / proj.w);     proj.w = 1.0f;
    
    float x = bx::ffloor(proj.x*wh + wh + 0.5f);
    float y = bx::ffloor(-proj.y*hh + hh + 0.5f);

    // ZCull
    if (proj.z < 0.0f || proj.z > 1.0f)
        return false;

    *result = vec2f(x, y);
    return true;
}

static Shape createSolidAABB()
{
    aabb_t box = aabb();
    vec3_t pts[8];

    const int numVerts = 36;
    aabbPushPoint(&box, vec3f(-0.5f, -0.5f, -0.5f));
    aabbPushPoint(&box, vec3f(0.5f, 0.5f, 0.5f));
    for (int i = 0; i < 8; i++)
        pts[i] = aabbGetCorner(box, i);

    dbgVertexPosCoordColor* verts = (dbgVertexPosCoordColor*)alloca(sizeof(dbgVertexPosCoordColor) * numVerts);
    memset(verts, 0x00, sizeof(dbgVertexPosCoordColor)*numVerts);

    // Z-
    verts[0].setPos(pts[0]); verts[1].setPos(pts[2]); verts[2].setPos(pts[3]);
    verts[3].setPos(pts[3]); verts[4].setPos(pts[1]); verts[5].setPos(pts[0]);
    // Z+
    verts[6].setPos(pts[5]); verts[7].setPos(pts[7]); verts[8].setPos(pts[6]);
    verts[9].setPos(pts[6]); verts[10].setPos(pts[4]); verts[11].setPos(pts[5]);
    // X+
    verts[12].setPos(pts[1]); verts[13].setPos(pts[3]); verts[14].setPos(pts[7]);
    verts[15].setPos(pts[7]); verts[16].setPos(pts[5]); verts[17].setPos(pts[1]);
    // X-
    verts[18].setPos(pts[6]); verts[19].setPos(pts[2]); verts[20].setPos(pts[0]);
    verts[21].setPos(pts[0]); verts[22].setPos(pts[4]); verts[23].setPos(pts[6]);
    // Y-
    verts[24].setPos(pts[1]); verts[25].setPos(pts[5]); verts[26].setPos(pts[4]);
    verts[27].setPos(pts[4]); verts[28].setPos(pts[0]); verts[29].setPos(pts[1]);
    // Y+
    verts[30].setPos(pts[3]); verts[31].setPos(pts[2]); verts[32].setPos(pts[6]);
    verts[33].setPos(pts[6]); verts[34].setPos(pts[7]); verts[35].setPos(pts[3]);

    return Shape(g_dbg->driver->createVertexBuffer(g_dbg->driver->copy(verts, sizeof(dbgVertexPosCoordColor) * numVerts),
                                                   dbgVertexPosCoordColor::Decl), numVerts);
}

static Shape createAABB()
{
    aabb_t box = aabb();
    vec3_t pts[8];

    const int numVerts = 24;
    aabbPushPoint(&box, vec3f(-0.5f, -0.5f, -0.5f));
    aabbPushPoint(&box, vec3f(0.5f, 0.5f, 0.5f));
    for (int i = 0; i < 8; i++)
        pts[i] = aabbGetCorner(box, i);

    dbgVertexPosCoordColor* verts = (dbgVertexPosCoordColor*)alloca(sizeof(dbgVertexPosCoordColor) * numVerts);
    memset(verts, 0x00, sizeof(dbgVertexPosCoordColor)*numVerts);

    // Bottom edges
    verts[0].setPos(pts[0]);    verts[1].setPos(pts[1]);
    verts[2].setPos(pts[1]);    verts[3].setPos(pts[5]);
    verts[4].setPos(pts[5]);    verts[5].setPos(pts[4]);
    verts[6].setPos(pts[4]);    verts[7].setPos(pts[0]);

    // Middle edges
    verts[8].setPos(pts[0]);    verts[9].setPos(pts[2]);
    verts[10].setPos(pts[1]);   verts[11].setPos(pts[3]);
    verts[12].setPos(pts[5]);   verts[13].setPos(pts[7]);
    verts[14].setPos(pts[4]);   verts[15].setPos(pts[6]);

    // Top edges
    verts[16].setPos(pts[2]);   verts[17].setPos(pts[3]);
    verts[18].setPos(pts[3]);   verts[19].setPos(pts[7]);
    verts[20].setPos(pts[7]);   verts[21].setPos(pts[6]);
    verts[22].setPos(pts[6]);   verts[23].setPos(pts[2]);

    return Shape(g_dbg->driver->createVertexBuffer(g_dbg->driver->copy(verts, sizeof(dbgVertexPosCoordColor) * numVerts),
        dbgVertexPosCoordColor::Decl), numVerts);
}


static Shape createBoundingSphere(int numSegs)
{
    numSegs = bx::uint32_iclamp(numSegs, 4, 35);
    const int numVerts = numSegs * 2;

    // Create buffer and fill it with data
    int size = numVerts * sizeof(dbgVertexPosCoordColor);
    dbgVertexPosCoordColor* verts = (dbgVertexPosCoordColor*)alloca(size);
    assert(verts);
    memset(verts, 0x00, size);

    const float dt = bx::pi * 2.0f / float(numSegs);
    float theta = 0.0f;
    int idx = 0;

    // Circle on the XY plane (center = (0, 0, 0), radius = 1)
    for (int i = 0; i < numSegs; i++) {
        verts[idx].setPos(bx::fcos(theta), bx::fsin(theta), 0);
        verts[idx + 1].setPos(bx::fcos(theta + dt), bx::fsin(theta + dt), 0);
        idx += 2;
        theta += dt;
    }

    return Shape(g_dbg->driver->createVertexBuffer(g_dbg->driver->copy(verts, size), dbgVertexPosCoordColor::Decl),
                 numVerts);
}

static Shape createSphere(int numSegsX, int numSegsY)
{
    // in solid sphere we have horozontal segments and vertical segments horizontal
    numSegsX = bx::uint32_iclamp(numSegsX, 4, 30);
    numSegsY = bx::uint32_iclamp(numSegsY, 3, 30);
    if (numSegsX % 2 != 0)
        numSegsX++;        // horozontal must be even number 
    if (numSegsY % 2 == 0)
        numSegsY++;        // vertical must be odd number
    const int numVerts = numSegsX * 3 * 2 + (numSegsY - 3) * 3 * 2 * numSegsX;

    /* set extreme points (radius = 1.0f) */
    vec3_t y_max = vec3f(0.0f, 1.0f, 0.0f);
    vec3_t y_min = vec3f(0.0f, -1.0f, 0.0f);

    // start from lower extreme point and draw slice of circles
    // connect them to the lower level
    // if we are on the last level (i == numIter-1) connect to upper extreme
    // else just make triangles connected to lower level
    int numIter = numSegsY - 1;
    int idx = 0;
    int lower_idx = idx;
    int delta_idx;
    float r;

    // Phi: vertical angle 
    float delta_phi = bx::pi / (float)numIter;
    float phi = -bx::piHalf + delta_phi;

    // Theta: horizontal angle 
    float delta_theta = (bx::pi*2.0f) / (float)numSegsX;
    float theta = 0.0f;
    float y;

    /* create buffer and fill it with data */
    int size = numVerts * sizeof(dbgVertexPosCoordColor);
    dbgVertexPosCoordColor *verts = (dbgVertexPosCoordColor*)alloca(size);
    if (verts == nullptr)
        return Shape();
    memset(verts, 0x00, size);

    for (int i = 0; i < numIter; i++) {
        /* calculate z and slice radius */
        r = bx::fcos(phi);
        y = bx::fsin(phi);
        phi += delta_phi;

        /* normal drawing (quad between upper level and lower level) */
        if (i != 0 && i != numIter - 1) {
            theta = 0.0f;
            for (int k = 0; k < numSegsX; k++) {
                /* current level verts */
                verts[idx].setPos(r*bx::fcos(theta), y, r*bx::fsin(theta));
                verts[idx + 1].setPos(r*bx::fcos(theta + delta_theta), y, r*bx::fsin(theta + delta_theta));
                verts[idx + 2].setPos(verts[lower_idx].x, verts[lower_idx].y, verts[lower_idx].z);
                verts[idx + 3].setPos(verts[idx + 1].x, verts[idx + 1].y, verts[idx + 1].z);
                verts[idx + 4].setPos(verts[lower_idx + 1].x, verts[lower_idx + 1].y, verts[lower_idx + 1].z);
                verts[idx + 5].setPos(verts[lower_idx].x, verts[lower_idx].y, verts[lower_idx].z);

                idx += 6;
                theta += delta_theta;
                lower_idx += delta_idx;
            }
            delta_idx = 6;
            continue;
        }

        /* lower cap */
        if (i == 0) {
            theta = 0.0f;
            lower_idx = idx;
            delta_idx = 3;
            for (int k = 0; k < numSegsX; k++) {
                verts[idx].setPos(r*bx::fcos(theta), y, r*bx::fsin(theta));
                verts[idx + 1].setPos(r*bx::fcos(theta + delta_theta), y, r*bx::fsin(theta + delta_theta));
                verts[idx + 2].setPos(y_min);
                idx += delta_idx;
                theta += delta_theta;
            }
        }

        /* higher cap */
        if (i == numIter - 1) {
            for (int k = 0; k < numSegsX; k++) {
                verts[idx].setPos(y_max);
                verts[idx + 1].setPos(verts[lower_idx + 1].x, verts[lower_idx + 1].y, verts[lower_idx + 1].z);
                verts[idx + 2].setPos(verts[lower_idx].x, verts[lower_idx].y, verts[lower_idx].z);
                idx += 3;
                theta += delta_theta;
                lower_idx += delta_idx;
            }
        }
    }

    return Shape(g_dbg->driver->createVertexBuffer(g_dbg->driver->copy(verts, size), dbgVertexPosCoordColor::Decl),
                 numVerts);
}

result_t termite::dbgInit(bx::AllocatorI* alloc, gfxDriverI* driver)
{
    if (g_dbg) {
        assert(false);
        return T_ERR_ALREADY_INITIALIZED;
    }

    g_dbg = BX_NEW(alloc, dbgMgr)(alloc);
    if (!g_dbg)
        return T_ERR_OUTOFMEM;
    g_dbg->alloc = alloc;
    g_dbg->driver = driver;
    g_dbg->whiteTexture = textureGetWhite1x1();
    if (!T_ISVALID(g_dbg->whiteTexture))
        return T_ERR_FAILED;
    
    // Load program
    {
        gfxShaderHandle vertexShader = driver->createShader(driver->makeRef(dbg_vso, sizeof(dbg_vso)));
        gfxShaderHandle fragmentShader = driver->createShader(driver->makeRef(dbg_fso, sizeof(dbg_fso)));
        if (!T_ISVALID(vertexShader) || !T_ISVALID(fragmentShader)) {
            T_ERROR("Creating shaders failed");
            return T_ERR_FAILED;
        }
        g_dbg->program = driver->createProgram(vertexShader, fragmentShader, true);
        if (!T_ISVALID(g_dbg->program)) {
            T_ERROR("Creating GPU program failed");
            return T_ERR_FAILED;
        }
    }

    dbgVertexPosCoordColor::init();

    g_dbg->uTexture = driver->createUniform("u_texture", gfxUniformType::Int1);
    assert(T_ISVALID(g_dbg->uTexture));
    g_dbg->uColor = driver->createUniform("u_color", gfxUniformType::Vec4);
    assert(T_ISVALID(g_dbg->uColor));

    // Create a 1x1 white texture 
    g_dbg->whiteTexture = textureGetWhite1x1();
    assert(T_ISVALID(g_dbg->whiteTexture));

    g_dbg->bbShape = createAABB();
    g_dbg->bsphereShape = createBoundingSphere(30);
    g_dbg->sphereShape = createSphere(12, 9);

    return T_OK;
}

void termite::dbgShutdown()
{
    if (!g_dbg)
        return;
    gfxDriverI* driver = g_dbg->driver;

    if (T_ISVALID(g_dbg->bbShape.vb))
        driver->destroyVertexBuffer(g_dbg->bbShape.vb);
    if (T_ISVALID(g_dbg->sphereShape.vb))
        driver->destroyVertexBuffer(g_dbg->sphereShape.vb);
    if (T_ISVALID(g_dbg->bsphereShape.vb))
        driver->destroyVertexBuffer(g_dbg->bsphereShape.vb);

    if (T_ISVALID(g_dbg->uColor))
        g_dbg->driver->destroyUniform(g_dbg->uColor);
    if (T_ISVALID(g_dbg->program))
        g_dbg->driver->destroyProgram(g_dbg->program);
    if (T_ISVALID(g_dbg->uTexture))
        g_dbg->driver->destroyUniform(g_dbg->uTexture);
    BX_DELETE(g_dbg->alloc, g_dbg);
    g_dbg = nullptr;
}

dbgContext* termite::dbgCreateContext(uint8_t viewId)
{
    assert(g_dbg);

    bx::AllocatorI* alloc = g_dbg->alloc;
    dbgContext* ctx = BX_NEW(alloc, dbgContext)(alloc);
    ctx->driver = g_dbg->driver;
    ctx->viewId = viewId;

    ctx->defaultFont = fntGet("fixedsys");
    if (!ctx->defaultFont) {
        dbgDestroyContext(ctx);
        return nullptr;
    }

    if (!ctx->statePool.create(STATE_POOL_SIZE, alloc)) {
        dbgDestroyContext(ctx);
        return nullptr;
    }

    // Push one state into state-stack
    State* state = ctx->statePool.newInstance();
    bx::pushStackNode<State*>(&ctx->stateStack, &state->snode, state);

    return ctx;
}

void termite::dbgDestroyContext(dbgContext* ctx)
{
    assert(g_dbg);
    assert(ctx);

    if (!ctx->alloc)
        return;

    ctx->statePool.destroy();
    BX_DELETE(ctx->alloc, ctx);
}

void termite::dbgBegin(dbgContext* ctx, float viewWidth, float viewHeight, Camera* cam, vgContext* vg)
{
    assert(ctx);
    ctx->viewport = rectf(0, 0, viewWidth, viewHeight);
    dbgReset(ctx);
    ctx->vgCtx = vg;
    ctx->viewport = rectf(0, 0, viewWidth, viewHeight);
    ctx->readyToDraw = true;

    mtx4x4_t projMtx = camProjMtx(cam, viewWidth / viewHeight);
    mtx4x4_t viewMtx = camViewMtx(cam);
    bx::mtxMul(ctx->viewProjMtx.f, viewMtx.f, projMtx.f);
    ctx->cam = cam;    
    ctx->billboardMtx = mtx4x4f3(viewMtx.m11, viewMtx.m21, viewMtx.m31,
                                 viewMtx.m12, viewMtx.m22, viewMtx.m32,
                                 viewMtx.m13, viewMtx.m23, viewMtx.m33,
                                 0.0f, 0.0f, 0.0f);

    if (vg)
        vgBegin(ctx->vgCtx, viewWidth, viewHeight);

    gfxDriverI* driver = ctx->driver;
    uint8_t viewId = ctx->viewId;
    driver->touch(viewId);
    driver->setViewRect(viewId, 0, 0, uint16_t(viewWidth), uint16_t(viewHeight));
    driver->setViewSeq(viewId, false);
    driver->setViewTransform(viewId, viewMtx.f, projMtx.f);
}

void termite::dbgEnd(dbgContext* ctx)
{
    if (ctx->vgCtx)
        vgEnd(ctx->vgCtx);
    ctx->readyToDraw = false;
}

void termite::dbgText(dbgContext* ctx, const vec3_t pos, const char* text)
{
    if (ctx->vgCtx) {
        vec2_t screenPt;
        if (projectToScreen(&screenPt, pos, ctx->viewport, ctx->viewProjMtx)) {
            State* state = ctx->stateStack->data;
            vgSetFont(ctx->vgCtx, state->font);
            vec4_t c = state->color;
            vgTextColor(ctx->vgCtx, rgbaf(c.x, c.y, c.z, c.w));
            vgText(ctx->vgCtx, screenPt.x, screenPt.y, text);
        }
    }
}

void termite::dbgTextf(dbgContext* ctx, const vec3_t pos, const char* fmt, ...)
{
    if (ctx->vgCtx) {
        char text[MAX_TEXT_SIZE];   text[0] = 0;

        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);

        dbgText(ctx, pos, text);
    }
}

void termite::dbgImage(dbgContext* ctx, const vec3_t pos, gfxTexture* image)
{
    if (ctx->vgCtx) {
        vec2_t screenPt;
        if (projectToScreen(&screenPt, pos, ctx->viewport, ctx->viewProjMtx)) {
            State* state = ctx->stateStack->data;
            vec4_t c = state->color;
            vgFillColor(ctx->vgCtx, rgbaf(c.x, c.y, c.z, c.w));
            vgImage(ctx->vgCtx, screenPt.x, screenPt.y, image);
        }
    }
}

void termite::dbgRect(dbgContext* ctx, const vec3_t& vmin, const vec3_t& vmax)
{
    if (ctx->vgCtx) {
        vec2_t minPt, maxPt;
        if (projectToScreen(&minPt, vmin, ctx->viewport, ctx->viewProjMtx) &&
            projectToScreen(&maxPt, vmax, ctx->viewport, ctx->viewProjMtx)) 
        {
            State* state = ctx->stateStack->data;
            vec4_t c = state->color;
            vgFillColor(ctx->vgCtx, rgbaf(c.x, c.y, c.z, c.w));
            vgRect(ctx->vgCtx, rectv(minPt, maxPt));
        }
    }
}

void termite::dbgSnapGridXZ(dbgContext* ctx, float spacing, float maxDepth)
{
    spacing = bx::fceil(bx::fclamp(spacing, 1.0f, 20.0f));

    vec3_t corners[8];
    float ratio = (ctx->viewport.xmax - ctx->viewport.xmin) / (ctx->viewport.ymax - ctx->viewport.ymin);
    camCalcFrustumCorners(ctx->cam, corners, ratio, -2.0f, bx::fmin(maxDepth, ctx->cam->ffar));

    mtx4x4_t projToXz;
    mtxProjPlane(&projToXz, vec3f(0, 1.0f, 0));

    // project frustum corners to XZ plane add them to bounding box
    aabb_t bb = aabb();
    for (int i = 0; i < 8; i++) {
        vec3_t tmp;
        bx::vec3MulMtx(tmp.f, corners[i].f, projToXz.f);
        aabbPushPoint(&bb, tmp);
    }

    // Snap grid bounds to 'spacing'
    // Example: spacing = 5, snap bounds to -5, 0, 5, ...
    int nspace = (int)spacing;
    vec3_t& minpt = bb.vmin;
    vec3_t& maxpt = bb.vmax;
    aabb_t snapbox = aabbf(float(int(minpt.x) - int(minpt.x) % nspace),
                           0,
                           float(int(minpt.z) - int(minpt.z) % nspace),
                           float(int(maxpt.x) - int(maxpt.x) % nspace),
                           0,
                           float(int(maxpt.z) - int(maxpt.z) % nspace));
    float w = snapbox.xmax - snapbox.xmin;
    float d = snapbox.zmax - snapbox.zmin;
    if (bx::fequal(w, 0, 0.00001f) || bx::fequal(d, 0, 0.00001f))
        return;

    int xlines = int(w) / nspace + 1;
    int ylines = int(d) / nspace + 1;
    int numVerts = (xlines + ylines) * 2;

    // Draw
    gfxDriverI* driver = ctx->driver;
    if (!driver->checkAvailTransientVertexBuffer(numVerts, dbgVertexPosCoordColor::Decl))
        return;
    gfxTransientVertexBuffer tvb;
    driver->allocTransientVertexBuffer(&tvb, numVerts, dbgVertexPosCoordColor::Decl);
    dbgVertexPosCoordColor* verts = (dbgVertexPosCoordColor*)tvb.data;
    
    int i = 0;
    for (float zoffset = snapbox.zmin; zoffset <= snapbox.zmax; zoffset += spacing, i += 2) {
        verts[i].x = snapbox.xmin;
        verts[i].y = 0;
        verts[i].z = zoffset;

        int ni = i + 1;
        verts[ni].x = snapbox.xmax;
        verts[ni].y = 0;
        verts[ni].z = zoffset;
    }

    for (float xoffset = snapbox.xmin; xoffset <= snapbox.xmax; xoffset += spacing, i += 2) {
        verts[i].x = xoffset;
        verts[i].y = 0;
        verts[i].z = snapbox.zmin;

        int ni = i + 1;
        verts[ni].x = xoffset;
        verts[ni].y = 0;
        verts[ni].z = snapbox.zmax;
    }

    State* state = ctx->stateStack->data;
    mtx4x4_t ident = mtx4x4Ident();

    driver->setVertexBuffer(&tvb);
    driver->setTransform(ident.f, 1);
    driver->setState(gfxState::RGBWrite | gfxState::DepthTestLess | gfxState::PrimitiveLines);
    driver->setUniform(g_dbg->uColor, state->color.f);
    driver->setTexture(0, g_dbg->uTexture, g_dbg->whiteTexture);
    driver->submit(ctx->viewId, g_dbg->program);
}

void termite::dbgBoundingBox(dbgContext* ctx, const aabb_t bb, bool showInfo /*= false*/)
{
    vec3_t center = (bb.vmin + bb.vmax)*0.5f;
    float w = bb.vmax.x - bb.vmin.x;
    float h = bb.vmax.y - bb.vmin.y;
    float d = bb.vmax.z - bb.vmin.z;

    mtx4x4_t mtx;
    bx::mtxSRT(mtx.f,
               w, h, d,
               0, 0, 0,
               center.x, center.y, center.z);

    Shape shape = g_dbg->bbShape;
    State* state = ctx->stateStack->data;
    gfxDriverI* driver = g_dbg->driver;
    driver->setVertexBuffer(shape.vb, 0, shape.numVerts);
    driver->setTransform(mtx.f, 1);
    driver->setUniform(g_dbg->uColor, state->color.f);
    driver->setState(gfxState::RGBWrite | gfxState::DepthTestLess | gfxState::PrimitiveLines);
    driver->setTexture(0, g_dbg->uTexture, g_dbg->whiteTexture);
    driver->submit(ctx->viewId, g_dbg->program);

    if (showInfo) {
        vec2_t center2d;
        if (projectToScreen(&center2d, center, ctx->viewport, ctx->viewProjMtx)) {
            State* state = ctx->stateStack->data;
            vgSetFont(ctx->vgCtx, state->font);
            vec4_t c = state->color;
            color_t color = rgbaf(c.x, c.y, c.z, c.w);
            vgTextColor(ctx->vgCtx, color);
            vgFillColor(ctx->vgCtx, color);
            vgRect(ctx->vgCtx, rectfwh(center2d.x - 5, center2d.y - 5, 10, 10));
            vgTextf(ctx->vgCtx, center2d.x, center2d.y, "aabb(%.1f, %.1f, %.1f)", w, h, d);
        }
    }
}

void termite::dbgBoundingSphere(dbgContext* ctx, const sphere_t sphere, bool showInfo /*= false*/)
{
    // translate by sphere center, and resize by sphere radius
    // combine with billboard mtx to face the camera
    mtx4x4_t mtx;
    bx::mtxSRT(mtx.f,
        sphere.r, sphere.r, sphere.r,
        0, 0, 0,
        sphere.x, sphere.y, sphere.z);
    mtx = ctx->billboardMtx * mtx;

    State* state = ctx->stateStack->data;
    gfxDriverI* driver = g_dbg->driver;
    Shape shape = g_dbg->bsphereShape;
    driver->setVertexBuffer(shape.vb, 0, shape.numVerts);
    driver->setTransform(mtx.f, 1);
    driver->setUniform(g_dbg->uColor, state->color.f);
    driver->setState(gfxState::RGBWrite | gfxState::DepthTestLess | gfxState::PrimitiveLines);
    driver->setTexture(0, g_dbg->uTexture, g_dbg->whiteTexture);
    driver->submit(ctx->viewId, g_dbg->program);

    if (showInfo) {
        vec2_t center2d;
        if (projectToScreen(&center2d, sphere.cp, ctx->viewport, ctx->viewProjMtx)) {
            State* state = ctx->stateStack->data;
            vgSetFont(ctx->vgCtx, state->font);
            vec4_t c = state->color;
            color_t color = rgbaf(c.x, c.y, c.z, c.w);
            vgTextColor(ctx->vgCtx, color);
            vgFillColor(ctx->vgCtx, color);
            vgRect(ctx->vgCtx, rectfwh(center2d.x - 5, center2d.y - 5, 10, 10));
            vgTextf(ctx->vgCtx, center2d.x, center2d.y, "sphere(%.1f, %.1f, %.1f, %.1f)", sphere.x, sphere.y, sphere.z, sphere.r);
        }
    }
}

void termite::dbgBox(dbgContext* ctx, const aabb_t aabb, const mtx4x4_t* modelMtx /*= nullptr*/)
{

}

void termite::dbgSphere(dbgContext* ctx, const sphere_t sphere, const mtx4x4_t* modelMtx /*= nullptr*/)
{

}

void termite::dbgAxis(dbgContext* ctx, const vec3_t axis, const mtx4x4_t* modelMtx /*= nullptr*/)
{

}

void termite::dbgSetFont(dbgContext* ctx, fntFont* font)
{
    State* state = ctx->stateStack->data;
    state->font = font ? font : ctx->defaultFont;
}

void termite::dbgAlpha(dbgContext* ctx, float alpha)
{
    State* state = ctx->stateStack->data;
    state->alpha = alpha;
}

void termite::dbgColor(dbgContext* ctx, const vec4_t& color)
{
    State* state = ctx->stateStack->data;
    state->color = color;
}

void termite::dbgTransform(dbgContext* ctx, const mtx4x4_t& mtx)
{
    State* state = ctx->stateStack->data;
    state->mtx = mtx;
}

void termite::dbgPushState(dbgContext* ctx)
{
    State* curState = bx::peekStack<State*>(ctx->stateStack);;
    State* s = ctx->statePool.newInstance();
    if (s) {
        bx::pushStackNode<State*>(&ctx->stateStack, &s->snode, s);
        memcpy(s, curState, sizeof(State));
    }
}

void termite::dbgPopState(dbgContext* ctx)
{
    if (ctx->stateStack->down) {
        State* s = bx::popStack<State*>(&ctx->stateStack);
        ctx->statePool.deleteInstance(s);
    }
}

static void setDefaultState(dbgContext* ctx, State* state)
{
    state->mtx = mtx4x4Ident();
    state->color = vec4f(1.0f, 1.0f, 1.0f, 1.0f);
    state->alpha = 1.0f;
    state->scissor = ctx->viewport;
    state->font = ctx->defaultFont;
}

void termite::dbgReset(dbgContext* ctx)
{
    State::SNode* node = ctx->stateStack;
    while (node->down) {
        State* s = bx::popStack<State*>(&ctx->stateStack);
        ctx->statePool.deleteInstance(s);
    }
    setDefaultState(ctx, node->data);
}
