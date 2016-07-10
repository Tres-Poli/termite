#include "pch.h"

#include "bx/readerwriter.h"
#include "bx/os.h"
#include "bx/cpu.h"
#include "bx/timer.h"
#include "bxx/inifile.h"
#include "bxx/pool.h"
#include "bxx/lock.h"
#include "bx/crtimpl.h"

#include "gfx_defines.h"
#include "gfx_font.h"
#include "gfx_utils.h"
#include "gfx_texture.h"
#include "gfx_vg.h"
#include "gfx_debugdraw.h"
#include "gfx_model.h"
#include "gfx_render.h"
#include "resource_lib.h"
#include "io_driver.h"
#include "job_dispatcher.h"
#include "memory_pool.h"
#include "plugin_system.h"

#define STB_LEAKCHECK_IMPLEMENTATION
#include "bxx/leakcheck_allocator.h"

#include "bxx/path.h"

#define BX_IMPLEMENT_LOGGER
#ifdef termite_SHARED_LIB
#   define BX_SHARED_LIB
#endif
#include "bxx/logger.h"

#include <dirent.h>
#include <random>

#define MEM_POOL_BUCKET_SIZE 256

using namespace termite;

struct FrameData
{
    int64_t frame;
    double frameTime;
    double fps;
    double elapsedTime;
    int64_t lastFrameTick;
};

struct HeapMemoryImpl
{
    termite::MemoryBlock m;
    volatile int32_t refcount;
    bx::AllocatorI* alloc;

    HeapMemoryImpl()
    {
        m.data = nullptr;
        m.size = 0;
        refcount = 1;
        alloc = nullptr;
    }
};

struct Core
{
    UpdateCallback updateFn;
    Config conf;
    RendererApi* renderer;
    FrameData frameData;
    double hpTimerFreq;
    bx::Pool<HeapMemoryImpl> memPool;
    bx::Lock memPoolLock;
    ResourceLib* resLib;
    GfxDriverApi* gfxDriver;
    IoDriverDual* ioDriver;
    PageAllocator tempAlloc;

    std::random_device randDevice;
    std::mt19937 randEngine;

    Core() :
        tempAlloc(T_MID_TEMP),
        randEngine(randDevice())
    {
        gfxDriver = nullptr;
        updateFn = nullptr;
        renderer = nullptr;
        hpTimerFreq = 0;
        resLib = nullptr;
        ioDriver = nullptr;
        memset(&frameData, 0x00, sizeof(frameData));
    }
};

#ifdef _DEBUG
static bx::LeakCheckAllocator g_allocStub;
#else
static bx::CrtAllocator g_allocStub;
#endif
static bx::AllocatorI* g_alloc = &g_allocStub;

static Core* g_core = nullptr;

static void callbackConf(const char* key, const char* value, void* userParam)
{
    Config* conf = (Config*)userParam;

    if (bx::stricmp(key, "Plugin_Path") == 0)
        bx::strlcpy(conf->pluginPath, value, sizeof(conf->pluginPath));
    else if (bx::stricmp(key, "gfx_DeviceId") == 0)
        sscanf(value, "%hu", &conf->gfxDeviceId);
    else if (bx::stricmp(key, "gfx_Width") == 0)
        sscanf(value, "%hu", &conf->gfxWidth);
    else if (bx::stricmp(key, "gfx_Height") == 0)
        sscanf(value, "%hu", &conf->gfxHeight);
    else if (bx::stricmp(key, "gfx_VSync") == 0)
        conf->gfxDriverFlags |= bx::toBool(value) ? uint32_t(GfxResetFlag::VSync) : 0;
}

Config* termite::loadConfig(const char* confFilepath)
{
    assert(g_core);

    Config* conf = BX_NEW(g_alloc, Config);
    if (!parseIniFile(confFilepath, callbackConf, conf, g_alloc)) {
        BX_WARN("Loading config file '%s' failed: Loading default config");
    }

    return conf;
}

void termite::freeConfig(Config* conf)
{
    assert(conf);

    BX_DELETE(g_alloc, conf);
}

static void loadFontsInDirectory(const char* baseDir, const char* rootDir)
{
    BX_VERBOSE("Scanning for fonts in directory '%s' ...", rootDir);
    bx::Path fullDir(baseDir);
    fullDir.join(rootDir);

    DIR* dir = opendir(fullDir.cstr());
    if (!dir) {
        BX_WARN("Could not open fonts directory '%s'", rootDir);
        return;
    }

    dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type == DT_REG) {
            bx::Path fntFilepath(rootDir);
            fntFilepath.join(ent->d_name);
            if (fntFilepath.getFileExt().isEqualNoCase("fnt"))
                registerFont(fntFilepath.cstr(), fntFilepath.getFilename().cstr());
        }
    }
    closedir(dir);

    return;
}

result_t termite::initialize(const Config& conf, UpdateCallback updateFn, const GfxPlatformData* platform)
{
    if (g_core) {
        assert(false);
        return T_ERR_ALREADY_INITIALIZED;
    }

    g_core = BX_NEW(g_alloc, Core);
    if (!g_core)
        return T_ERR_OUTOFMEM;

    memcpy(&g_core->conf, &conf, sizeof(g_core->conf));

    g_core->updateFn = updateFn;

    // Error handler
    if (initErrorReport(g_alloc)) {
        return T_ERR_FAILED;
    }

    // Memory pool for MemoryBlock objects
    if (!g_core->memPool.create(MEM_POOL_BUCKET_SIZE, g_alloc))
        return T_ERR_OUTOFMEM;

    if (initMemoryPool(g_alloc))
        return T_ERR_OUTOFMEM;

    // Initialize plugins system and enumerate plugins
    if (initPluginSystem(conf.pluginPath, g_alloc)) {
        T_ERROR("Engine init failed: PluginSystem failed");
        return T_ERR_FAILED;
    }

    // Initialize and blocking data driver for permanent resources
    int r;
    PluginHandle pluginHandle;

    // IO
    r = findPluginByName(conf.ioName[0] ? conf.ioName : "DiskIO" , 0, &pluginHandle, 1, PluginType::IoDriver);
    if (r > 0) {
        g_core->ioDriver = (IoDriverDual*)initPlugin(pluginHandle, g_alloc);
        if (!g_core->ioDriver) {
            T_ERROR("Engine init failed: Could not find IO driver");
            return T_ERR_FAILED;
        }

        // Initialize IO
        // If data path is chosen, set is as root path
        // If not, use the current directory as the root path
        char curPath[256];
        const char* uri;
        if (conf.dataUri[0]) {
            uri = conf.dataUri;
        } else {
            bx::pwd(curPath, sizeof(curPath));
            uri = curPath;
        }

        const PluginDesc& desc = getPluginDesc(pluginHandle);
        BX_BEGINP("Initializing IO Driver: %s v%d.%d", desc.name, T_VERSION_MAJOR(desc.version), T_VERSION_MINOR(desc.version));
        if (T_FAILED(g_core->ioDriver->blocking->init(g_alloc, uri, nullptr, nullptr)) ||
            T_FAILED(g_core->ioDriver->async->init(g_alloc, uri, nullptr, nullptr))) 
        {
            BX_END_FATAL();
            T_ERROR("Engine init failed: Initializing Async IoDriver failed");
            return T_ERR_FAILED;
        }
        BX_END_OK();
    }


    if (!g_core->ioDriver) {
        T_ERROR("Engine init failed: No IoDriver is detected");
        return T_ERR_FAILED;
    }

    BX_BEGINP("Initializing Resource Library");
    g_core->resLib = createResourceLib(ResourceLibInitFlag::HotLoading, g_core->ioDriver->async, g_alloc);
    if (!g_core->resLib) {
        T_ERROR("Core init failed: Creating default ResourceLib failed");
        return T_ERR_FAILED;
    }
    BX_END_OK();

    // Graphics
    r = findPluginByName(conf.rendererName[0] ? conf.rendererName : "StockRenderer", 0, &pluginHandle, 1, PluginType::Renderer);
    if (r > 0) {
        g_core->renderer = (RendererApi*)initPlugin(pluginHandle, g_alloc);
        const PluginDesc& desc = getPluginDesc(pluginHandle);
        BX_TRACE("Found Renderer: %s v%d.%d", desc.name, T_VERSION_MAJOR(desc.version), T_VERSION_MINOR(desc.version));

        if (!platform) {
            T_ERROR("Core init failed: PlatformData is not provided for Renderer");
            return T_ERR_FAILED;
        }
    } else {
        BX_WARN("No Renderer found - Graphics output will be unavailable");
    }

    // Graphics Device
    if (g_core->renderer) {
        r = findPluginByName(conf.gfxName[0] ? conf.gfxName : "Bgfx", 0, &pluginHandle, 1, PluginType::GraphicsDriver);
        if (r > 0) {
            g_core->gfxDriver = (GfxDriverApi*)initPlugin(pluginHandle, g_alloc);
        }

        if (!g_core->gfxDriver) {
            T_ERROR("Core init failed: Could not detect Graphics driver");
            return T_ERR_FAILED;
        }

        const PluginDesc& desc = getPluginDesc(pluginHandle);
        BX_BEGINP("Initializing Graphics Driver: %s v%d.%d", desc.name, T_VERSION_MAJOR(desc.version), 
                                                                        T_VERSION_MINOR(desc.version));
        g_core->gfxDriver->setPlatformData(*platform);
        if (T_FAILED(g_core->gfxDriver->init(conf.gfxDeviceId, nullptr, g_alloc))) {
            BX_END_FATAL();
            T_ERROR("Core init failed: Could not initialize Graphics driver");
            return T_ERR_FAILED;
        }
        BX_END_OK();

        // Initialize Renderer
        BX_BEGINP("Initializing Renderer");
        if (T_FAILED(g_core->renderer->init(g_alloc, g_core->gfxDriver))) {
            BX_END_FATAL();
            T_ERROR("Core init failed: Could not initialize Renderer");
            return T_ERR_FAILED;
        }
        BX_END_OK();

        // Init and Register graphics resource loaders
        initTextureLoader(g_core->gfxDriver, g_alloc);
        registerTextureToResourceLib(g_core->resLib);

        initModelLoader(g_core->gfxDriver, g_alloc);
        registerModelToResourceLib(g_core->resLib);

        // Font library
        if (initFontLib(g_alloc, g_core->ioDriver->blocking)) {
            T_ERROR("Initializing font library failed");
            return T_ERR_FAILED;
        }
        loadFontsInDirectory(g_core->ioDriver->blocking->getUri(), "fonts");

        // VectorGraphics
        if (initVectorGfx(g_alloc, g_core->gfxDriver)) {
            T_ERROR("Initializing Vector Graphics failed");
            return T_ERR_FAILED;
        }

        // Debug graphics
        if (initDebugDraw(g_alloc, g_core->gfxDriver)) {
            T_ERROR("Initializing Editor Draw failed");
            return T_ERR_FAILED;
        }

        // Graphics Utilities
        if (initGfxUtils(g_core->gfxDriver)) {
            T_ERROR("Initializing Graphics Utilities failed");
            return T_ERR_FAILED;
        }
    }

    // Job Dispatcher
    if (initJobDispatcher(g_alloc, 0, 0, 0, 0, false)) {
        T_ERROR("Core init failed: Job Dispatcher init failed");
        return T_ERR_FAILED;
    }

    g_core->hpTimerFreq = bx::getHPFrequency();

    return 0;
}

void termite::shutdown()
{
    if (!g_core) {
        assert(false);
        return;
    }

    shutdownJobDispatcher();

    shutdownDebugDraw();
    shutdownVectorGfx();
    shutdownFontLib();
    shutdownModelLoader();
    shutdownTextureLoader();
    shutdownGfxUtils();

    if (g_core->renderer) {
        BX_BEGINP("Shutting down Renderer");
        g_core->renderer->shutdown();
        g_core->renderer = nullptr;
        BX_END_OK();
    }

    if (g_core->gfxDriver) {
        BX_BEGINP("Shutting down Graphics Driver");
        g_core->gfxDriver->shutdown();
        g_core->gfxDriver = nullptr;
        BX_END_OK();
    }

    if (g_core->resLib) {
        destroyResourceLib(g_core->resLib);
        g_core->resLib = nullptr;
    }

    if (g_core->ioDriver) {
        BX_BEGINP("Shutting down IO Driver");
        g_core->ioDriver->blocking->shutdown();
        g_core->ioDriver->async->shutdown();
        g_core->ioDriver = nullptr;
        BX_END_OK();
    }

    shutdownPluginSystem();

    g_core->memPool.destroy();
    shutdownMemoryPool();

    shutdownErrorReport();
    BX_DELETE(g_alloc, g_core);
    g_core = nullptr;

#ifdef _DEBUG
    stb_leakcheck_dumpmem();
#endif
}

void termite::doFrame()
{
    FrameData& fd = g_core->frameData;

    freeTag(T_MID_TEMP);

    if (fd.lastFrameTick == 0)
        fd.lastFrameTick = bx::getHPCounter();

    int64_t frameTick = bx::getHPCounter();
    double dt = double(frameTick - fd.lastFrameTick)/g_core->hpTimerFreq;

    if (g_core->renderer)
        g_core->renderer->render(nullptr);

    if (g_core->updateFn)
        g_core->updateFn(float(dt));

    if (g_core->ioDriver->async)
        g_core->ioDriver->async->runAsyncLoop();

    if (g_core->gfxDriver)
        g_core->gfxDriver->frame();

    fd.frame++;
    fd.elapsedTime += dt;
    fd.frameTime = dt;
    fd.fps = (double)fd.frame / fd.elapsedTime;
    fd.lastFrameTick = frameTick;
}

double termite::getFrameTime()
{
    return g_core->frameData.frameTime;
}

double termite::getElapsedTime()
{
    return g_core->frameData.elapsedTime;
}

double termite::getFps()
{
    return g_core->frameData.fps;
}

termite::MemoryBlock* termite::createMemoryBlock(uint32_t size, bx::AllocatorI* alloc)
{
    g_core->memPoolLock.lock();
    HeapMemoryImpl* mem = g_core->memPool.newInstance();
    g_core->memPoolLock.unlock();
    if (!alloc)
        alloc = g_alloc;
    mem->m.data = (uint8_t*)BX_ALLOC(alloc, size);
    if (!mem->m.data)
        return nullptr;
    mem->m.size = size;
    mem->alloc = alloc;
    return (termite::MemoryBlock*)mem;
}

termite::MemoryBlock* termite::refMemoryBlockPtr(void* data, uint32_t size)
{
    g_core->memPoolLock.lock();
    HeapMemoryImpl* mem = g_core->memPool.newInstance();
    g_core->memPoolLock.unlock();
    mem->m.data = (uint8_t*)data;
    mem->m.size = size;
    return (MemoryBlock*)mem;
}

termite::MemoryBlock* termite::copyMemoryBlock(const void* data, uint32_t size, bx::AllocatorI* alloc)
{
    g_core->memPoolLock.lock();
    HeapMemoryImpl* mem = g_core->memPool.newInstance();
    g_core->memPoolLock.unlock();
    if (!alloc)
        alloc = g_alloc;
    mem->m.data = (uint8_t*)BX_ALLOC(alloc, size);
    if (!mem->m.data)
        return nullptr;
    memcpy(mem->m.data, data, size);
    mem->m.size = size; 
    mem->alloc = alloc;
    return (MemoryBlock*)mem;
}

termite::MemoryBlock* termite::refMemoryBlock(termite::MemoryBlock* mem)
{
    HeapMemoryImpl* m = (HeapMemoryImpl*)mem;
    bx::atomicFetchAndAdd(&m->refcount, 1);
    return mem;
}

void termite::releaseMemoryBlock(termite::MemoryBlock* mem)
{
    HeapMemoryImpl* m = (HeapMemoryImpl*)mem;
    if (bx::atomicDec(&m->refcount) == 0) {
        if (m->alloc) {
            BX_FREE(m->alloc, m->m.data);
            m->m.data = nullptr;
            m->m.size = 0;
        }

        bx::LockScope(g_core->memPoolLock);
        g_core->memPool.deleteInstance(m);
    }
}

MemoryBlock* termite::readTextFile(const char* filepath)
{
    const char* rootPath = g_core->ioDriver->blocking ? g_core->ioDriver->blocking->getUri() : "";
    bx::Path fullpath(rootPath);
    fullpath.join(filepath);

    bx::CrtFileReader file;
    bx::Error err;
    if (!file.open(fullpath.cstr(), &err))
        return nullptr;
    uint32_t size = (uint32_t)file.seek(0, bx::Whence::End);

    MemoryBlock* mem = createMemoryBlock(size + 1, g_alloc);
    if (!mem) {
        file.close();
        return nullptr;
    }

    file.seek(0, bx::Whence::Begin);
    file.read(mem->data, size, &err);
    ((char*)mem->data)[size] = 0;
    file.close();

    return mem;
}

float termite::getRandomFloatUniform(float a, float b)
{
    std::uniform_real_distribution<float> dist(a, b);
    return dist(g_core->randEngine);
}

int termite::getRandomIntUniform(int a, int b)
{
    std::uniform_int_distribution<int> dist(a, b);
    return dist(g_core->randEngine);
}

void termite::inputSendChars(const char* chars)
{
}

void termite::inputSendKeys(const bool keysDown[512], bool shift, bool alt, bool ctrl)
{
}

void termite::inputSendMouse(float mousePos[2], int mouseButtons[3], float mouseWheel)
{
}

GfxDriverApi* termite::getGfxDriver()
{
    return g_core->gfxDriver;
}

IoDriverDual* termite::getIoDriver()
{
    return g_core->ioDriver;
}

RendererApi* termite::getRenderer()
{
    return g_core->renderer;
}

uint32_t termite::getEngineVersion()
{
    return T_MAKE_VERSION(0, 1);
}

bx::AllocatorI* termite::getHeapAlloc() T_THREAD_SAFE
{
    return g_alloc;
}

bx::AllocatorI* termite::getTempAlloc() T_THREAD_SAFE
{
    return &g_core->tempAlloc;
}

const Config& termite::getConfig()
{
    return g_core->conf;
}

ResourceLib* termite::getDefaultResourceLib()
{
    return g_core->resLib;
}
