#include "pch.h"

#include "bx/readerwriter.h"
#include "bx/filepath.h"
#include "bx/os.h"
#include "bx/cpu.h"
#include "bx/file.h"
#include "bxx/path.h"
#include "bxx/pool.h"
#include "bxx/lock.h"
#include "bxx/array.h"
#include "bxx/string.h"

#include "gfx_driver.h"
#include "gfx_font.h"
#include "gfx_utils.h"
#include "gfx_texture.h"
#include "gfx_debugdraw2d.h"
#include "gfx_debugdraw.h"
#include "gfx_model.h"
#include "gfx_render.h"
#include "gfx_sprite.h"
#include "assetlib.h"
#include "io_driver.h"
#include "job_dispatcher.h"
#include "memory_pool.h"
#include "plugin_system.h"
#include "ecs.h"
#include "event_dispatcher.h"
#include "physics_2d.h"
#include "command_system.h"
#include "sound_driver.h"
#include "lang.h"
#include "internal.h"
#include "rapidjson.h"

#ifdef termite_SDL2
#  include <SDL.h>
#  include "sdl_utils.h"
#endif

#include "../imgui_impl/imgui_impl.h"

#define STB_LEAKCHECK_IMPLEMENTATION
#define STB_LEAKCHECK_MULTITHREAD
#include "bxx/leakcheck_allocator.h"

#include "bxx/path.h"

#define BX_IMPLEMENT_LOGGER
#ifdef termite_SHARED_LIB
#   define BX_SHARED_LIB
#endif
#include "bxx/logger.h"

#define BX_IMPLEMENT_JSON
#include "bxx/json.h"

#include <dirent.h>
#include <random>
#include <chrono>
#include <thread>

#include "remotery/Remotery.h"
#include "lz4/lz4.h"
#include "tiny-AES128-C/aes.h"

#define MEM_POOL_BUCKET_SIZE 256
#define IMGUI_VIEWID 255
#define NANOVG_VIEWID 254
#define LOG_STRING_SIZE 256
#define RANDOM_NUMBER_POOL 10000

#define T_ENC_SIGN 0x54454e43        // "TENC"
#define T_ENC_VERSION TEE_MAKE_VERSION(1, 0)       

typedef std::chrono::high_resolution_clock TClock;
typedef std::chrono::high_resolution_clock::time_point TClockTimePt;

namespace tee {
// default AES encryption keys
static const uint8_t kAESKey[] = {0x32, 0xBF, 0xE7, 0x76, 0x41, 0x21, 0xF6, 0xA5, 0xEE, 0x70, 0xDC, 0xC8, 0x73, 0xBC, 0x9E, 0x37};
static const uint8_t kAESIv[] = {0x0A, 0x2D, 0x76, 0x63, 0x9F, 0x28, 0x10, 0xCD, 0x24, 0x22, 0x26, 0x68, 0xC1, 0x5A, 0x82, 0x5A};

//
struct FrameData
{
    uint64_t frame;
    double frameTime;
    double fps;
    double elapsedTime;
    double avgFrameTime;
    TClockTimePt lastFrameTimePt;
    double frameTimes[32];
    double fpsTime;
};

struct HeapMemoryImpl
{
    MemoryBlock m;
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

#pragma pack(push, 1)
struct EncodeHeader
{
    uint32_t sign;
    uint32_t version;
    int decodeSize;
    int uncompSize;
};
#pragma pack(pop)

class GfxDriverEvents : public GfxDriverEventsI
{
private:
    bx::Lock m_lock;

public:
    void onFatal(GfxFatalType::Enum type, const char* str) override;
    void onTraceVargs(const char* filepath, int line, const char* format, va_list argList) override;

    uint32_t onCacheReadSize(uint64_t id) override
    {
        return 0;
    }

    bool onCacheRead(uint64_t id, void* data, uint32_t size) override
    {
        return false;
    }

    void onCacheWrite(uint64_t id, const void* data, uint32_t size) override
    {
    }

    void onScreenShot(const char *filePath, uint32_t width, uint32_t height, uint32_t pitch, 
                      const void *data, uint32_t size, bool yflip) override
    {
    }

    void onCaptureBegin(uint32_t width, uint32_t height, uint32_t pitch, TextureFormat::Enum fmt, bool yflip) override
    {
    }

    void onCaptureEnd() override
    {
    }

    void onCaptureFrame(const void* data, uint32_t size) override
    {
    }
};

struct LogCache
{
    bx::LogType::Enum type;
    char text[LOG_STRING_SIZE];
};

struct ConsoleCommand
{
    size_t cmdHash;
    std::function<void(int, const char**)> callback;
    ConsoleCommand() : cmdHash(0) {}
};

struct Tee
{
    UpdateCallback updateFn;
    Config conf;
    RendererApi* renderer;
    FrameData frameData;
    double timeMultiplier;
    bx::Pool<HeapMemoryImpl> memPool;
    bx::Lock memPoolLock;
    GfxDriver* gfxDriver;
    IoDriverDual* ioDriver;
    PhysDriver2D* phys2dDriver;
    SimpleSoundDriver* sndDriver;
    PageAllocator tempAlloc;
    GfxDriverEvents gfxDriverEvents;
    LogCache* gfxLogCache;
    int numGfxLogCache;

    std::random_device randDevice;
    std::mt19937 randEngine;
    int* randomPoolInt;
    float* randomPoolFloat;
    volatile int32_t randomIntOffset;
    volatile int32_t randomFloatOffset;

    Remotery* rmt;
    bx::Array<ConsoleCommand> consoleCmds;

    bool init;
    bool gfxReset;

    Tee() :
        tempAlloc(TEE_MEMID_TEMP),
        randEngine(randDevice())
    {
        gfxDriver = nullptr;
        phys2dDriver = nullptr;
        sndDriver = nullptr;
        updateFn = nullptr;
        renderer = nullptr;
        ioDriver = nullptr;
        timeMultiplier = 1.0;
        gfxLogCache = nullptr;
        numGfxLogCache = 0;
        rmt = nullptr;
        init = false;
        gfxReset = false;
        bx::memSet(&frameData, 0x00, sizeof(frameData));
        randomFloatOffset = randomIntOffset = 0;
        randomPoolFloat = nullptr;
        randomPoolInt = nullptr;
    }
};

#ifdef _DEBUG
static bx::LeakCheckAllocator gAllocStub;
#else
static bx::DefaultAllocator gAllocStub;
#endif
static bx::AllocatorI* gAlloc = &gAllocStub;

static bx::Path gDataDir;
static bx::Path gCacheDir;
static HardwareInfo gHwInfo;
static Tee* gTee = nullptr;

#if BX_PLATFORM_ANDROID
static JavaVM* gJavaVM = nullptr;
static jclass gActivityClass = 0;
static jobject gActivityObj = 0;

extern "C" JNIEXPORT void JNICALL Java_com_termite_util_Platform_termiteInitEngineVars(JNIEnv* env, jclass cls, 
                                                                                       jobject obj, jstring dataDir, 
                                                                                       jstring cacheDir)
{
    BX_UNUSED(cls);

    env->GetJavaVM(&gJavaVM);
    gActivityClass = (jclass)env->NewGlobalRef(env->GetObjectClass(obj));
    gActivityObj = env->NewGlobalRef(obj);
    
    const char* str = env->GetStringUTFChars(dataDir, nullptr);
    bx::strCopy(gDataDir.getBuffer(), sizeof(gDataDir), str);
    env->ReleaseStringUTFChars(dataDir, str);

    str = env->GetStringUTFChars(cacheDir, nullptr);
    bx::strCopy(gCacheDir.getBuffer(), sizeof(gCacheDir), str);
    env->ReleaseStringUTFChars(cacheDir, str);
}


extern "C" JNIEXPORT void JNICALL Java_com_termite_util_Platform_termiteSetGraphicsReset(JNIEnv* env, jclass cls)
{
    if (gTee)
        gTee->gfxReset = true;
}

extern "C" JNIEXPORT void JNICALL Java_com_termite_util_Platform_termiteSetDeviceInfo(JNIEnv* env, jclass cls,
                                                                                      jstring brand, jstring model, jstring uniqueId,
                                                                                      jlong totalMem, jlong availMem, jlong thresholdMem,
                                                                                      jint apiVersion)
{
    BX_UNUSED(cls);
    
    HardwareInfo& hwInfo = gHwInfo;

    const char* strBrand = env->GetStringUTFChars(brand, nullptr);
    bx::strCopy(hwInfo.brand, sizeof(hwInfo.brand), strBrand);
    env->ReleaseStringUTFChars(brand, strBrand);

    const char* strModel = env->GetStringUTFChars(model, nullptr);
    bx::strCopy(hwInfo.model, sizeof(hwInfo.model), strModel);
    env->ReleaseStringUTFChars(model, strModel);

    const char* strUniqueId = env->GetStringUTFChars(uniqueId, nullptr);
    bx::strCopy(hwInfo.uniqueId, sizeof(hwInfo.uniqueId), strUniqueId);
    env->ReleaseStringUTFChars(uniqueId, strUniqueId);

    hwInfo.totalMem = totalMem;
    hwInfo.apiVersion = apiVersion;
}

JavaMethod androidFindMethod(const char* methodName, const char* methodSig, const char* classPath,
                                      JavaMethodType::Enum type)
{
    assert(gJavaVM);

    JavaMethod m;
    bx::memSet(&m, 0x00, sizeof(m));

    if (type == JavaMethodType::Method) {
        gJavaVM->AttachCurrentThread(&m.env, nullptr);
    } else if (type == JavaMethodType::StaticMethod) {
        gJavaVM->GetEnv((void**)&m.env, JNI_VERSION_1_6);
    }

    if (m.env == nullptr) {
        BX_WARN("Calling Java method '%s' failed: Cannot retreive Java Env", methodName);
        return m;
    }

    if (classPath == nullptr) {
        m.cls = gActivityClass;
        m.obj = gActivityObj;
    } else {
        m.cls = m.env->FindClass(classPath);
        if (m.cls == 0) {
            BX_WARN("Calling Java method '%s' failed: Cannot find class '%s'", methodName, classPath);
            return m;
        }
    }


    if (type == JavaMethodType::Method) {
        m.methodId = m.env->GetMethodID(m.cls, methodName, methodSig);
    } else if (type == JavaMethodType::StaticMethod) {
        m.methodId = m.env->GetStaticMethodID(m.cls, methodName, methodSig);
    }
    if (m.methodId == 0) {
        BX_WARN("Finding Java method '%s' failed: Cannot find method in class '%s'", methodName, classPath);
        return m;
    }

    return m;
}
#endif

static void* remoteryMallocCallback(void* mm_context, rmtU32 size)
{
    return BX_ALLOC(gAlloc, size);
}

static void remoteryFreeCallback(void* mm_context, void* ptr)
{
    BX_FREE(gAlloc, ptr);
}

static void* remoteryReallocCallback(void* mm_context, void* ptr, rmtU32 size)
{
    return BX_REALLOC(gAlloc, ptr, size);
}

static void remoteryInputHandlerCallback(const char* text, void* context)
{
    assert(gTee);

    const int maxArgs = 16;
    bx::String64 args[maxArgs];
    char cmdText[2048];
    bx::strCopy(cmdText, sizeof(cmdText), text);
    char* token = strtok(cmdText, " ");
    int numArgs = 0;
    while (token && numArgs < maxArgs) {
        args[numArgs++] = token;
        token = strtok(nullptr, " ");
    }

    if (numArgs > 0) {
        // find and execute command
        size_t cmdHash = tinystl::hash_string(args[0].cstr(), args[0].getLength());
        for (int i = 0; i < gTee->consoleCmds.getCount(); i++) {
            if (gTee->consoleCmds[i].cmdHash == cmdHash) {
                const char* cargs[maxArgs];
                for (int k = 0; k < numArgs; k++)
                    cargs[k] = args[k].cstr();

                gTee->consoleCmds[i].callback(numArgs, cargs);
                break;
            }
        }
    }
}

bool init(const Config& conf, UpdateCallback updateFn, const GfxPlatformData* platform)
{
    if (gTee) {
        assert(false);
        return false;
    }

    bx::enableLogToFileHandle(stdout, stderr);
    json::HeapAllocator::SetAlloc(gAlloc);

    gTee = BX_NEW(gAlloc, Tee);
    if (!gTee)
        return false;

    memcpy(&gTee->conf, &conf, sizeof(gTee->conf));

    // Hardware stats
    // Same on all platforms
    gHwInfo.numCores = std::thread::hardware_concurrency();

    gTee->updateFn = updateFn;

    // Set Data and Cache Dir paths
#if !BX_PLATFORM_ANDROID
    gDataDir = conf.dataUri;
    gDataDir.normalizeSelf();
    gCacheDir = bx::getTempDir();        
#endif

    // Error handler
    if (!err::init(gAlloc)) {
        return false;
    }

    // Memory pool for MemoryBlock objects
    if (!gTee->memPool.create(MEM_POOL_BUCKET_SIZE, gAlloc))
        return false;

    if (!initMemoryPool(gAlloc, conf.pageSize*1024, conf.maxPagesPerPool))
        return false;

    // Random pools
    gTee->randomPoolInt = (int*)BX_ALLOC(gAlloc, sizeof(int)*RANDOM_NUMBER_POOL);
    gTee->randomPoolFloat = (float*)BX_ALLOC(gAlloc, sizeof(float)*RANDOM_NUMBER_POOL);
    if (!gTee->randomPoolInt || !gTee->randomPoolFloat)
        return false;
    restartRandom();    // fill random values

    // Initialize plugins system and enumerate plugins
    if (!initPluginSystem(conf.pluginPath.cstr(), gAlloc)) {
        TEE_ERROR("Engine init failed: PluginSystem failed");
        return false;
    }

    // IO
    PluginHandle ioPlugin = findPlugin(!conf.ioName.isEmpty() ? conf.ioName.cstr() : "DiskIO_Lite", PluginType::IoDriver);
    if (ioPlugin.isValid()) {
        gTee->ioDriver = (IoDriverDual*)initPlugin(ioPlugin, gAlloc);
        if (!gTee->ioDriver) {
            TEE_ERROR("Engine init failed: Could not find IO driver");
            return false;
        }

        // Initialize IO
        // If data path is chosen, set is as root path
        // If not, use the current directory as the root path
        const char* uri;
        if (!conf.dataUri.isEmpty()) {
            uri = conf.dataUri.cstr();
        } else {
            bx::FilePath curPath(bx::Dir::Current);
            uri = curPath.get();
        }

        const PluginDesc& desc = getPluginDesc(ioPlugin);
        BX_BEGINP("Initializing IO Driver: %s v%d.%d", desc.name, TEE_VERSION_MAJOR(desc.version), TEE_VERSION_MINOR(desc.version));
        if (!gTee->ioDriver->blocking->init(gAlloc, uri, nullptr, nullptr, IoFlags::ExtractLZ4) ||
            !gTee->ioDriver->async->init(gAlloc, uri, nullptr, nullptr, IoFlags::ExtractLZ4))
        {
            BX_END_FATAL();
            TEE_ERROR("Engine init failed: Initializing IoDriver failed");
            return false;
        }
        BX_END_OK();
    }

    if (!gTee->ioDriver) {
        TEE_ERROR("Engine init failed: No IoDriver is detected");
        return false;
    }

    BX_BEGINP("Initializing Resource Library");
    if (!asset::init(BX_ENABLED(termite_DEV) ? AssetLibInitFlags::HotLoading : 0, gTee->ioDriver->async, gAlloc)) 
    {
        TEE_ERROR("Core init failed: Creating default ResourceLib failed");
        return false;
    }
    BX_END_OK();

    // Renderer
    if (!conf.rendererName.isEmpty()) {
        PluginHandle rendererPlugin = findPlugin(conf.rendererName.cstr(), PluginType::Renderer);
        if (rendererPlugin.isValid()) {
            gTee->renderer = (RendererApi*)initPlugin(rendererPlugin, gAlloc);
            const PluginDesc& desc = getPluginDesc(rendererPlugin);
            BX_TRACE("Found Renderer: %s v%d.%d", desc.name, TEE_VERSION_MAJOR(desc.version), TEE_VERSION_MINOR(desc.version));

            if (!platform) {
                TEE_ERROR("Core init failed: PlatformData is not provided for Renderer");
                return false;
            }
        } 
    }

    // Graphics Device
    if (!conf.gfxName.isEmpty())    {
        PluginHandle gfxPlugin = findPlugin(conf.gfxName.cstr(), PluginType::GraphicsDriver);
        if (gfxPlugin.isValid()) {
            gTee->gfxDriver = (GfxDriver*)initPlugin(gfxPlugin, gAlloc);
        }

        if (!gTee->gfxDriver) {
            TEE_ERROR("Core init failed: Could not detect Graphics driver: %s", conf.gfxName.cstr());
            return false;
        }

        const PluginDesc& desc = getPluginDesc(gfxPlugin);
        BX_BEGINP("Initializing Graphics Driver: %s v%d.%d", desc.name, TEE_VERSION_MAJOR(desc.version),
                  TEE_VERSION_MINOR(desc.version));
        if (platform)
            gTee->gfxDriver->setPlatformData(*platform);
        if (!gTee->gfxDriver->init(conf.gfxDeviceId, &gTee->gfxDriverEvents, gAlloc)) {
            BX_END_FATAL();
            dumpGfxLog();
            TEE_ERROR("Core init failed: Could not initialize Graphics driver");
            return false;
        }
        BX_END_OK();
        dumpGfxLog();

        // Initialize Renderer with Gfx Driver
        if (gTee->renderer) {
            BX_BEGINP("Initializing Renderer");
            if (!gTee->renderer->init(gAlloc, gTee->gfxDriver)) {
                BX_END_FATAL();
                TEE_ERROR("Core init failed: Could not initialize Renderer");
                return false;
            }
            BX_END_OK();
        }

        // Init and Register graphics resource loaders
        gfx::initTextureLoader(gTee->gfxDriver, gAlloc);
        gfx::registerTextureToAssetLib();

        gfx::initModelLoader(gTee->gfxDriver, gAlloc);
        gfx::registerModelToAssetLib();

        gfx::initFontSystem(gAlloc, vec2(float(conf.refScreenWidth), float(conf.refScreenHeight)));
        gfx::registerFontToAssetLib();

        // VectorGraphics
        if (!gfx::initDebugDraw2D(gAlloc, gTee->gfxDriver)) {
            TEE_ERROR("Initializing Vector Graphics failed");
            return false;
        }

        // Debug graphics
        if (!gfx::initDebugDraw(gAlloc, gTee->gfxDriver)) {
            TEE_ERROR("Initializing Editor Draw failed");
            return false;
        }

        // Graphics Utilities
        if (!gfx::initGfxUtils(gTee->gfxDriver)) {
            TEE_ERROR("Initializing Graphics Utilities failed");
            return false;
        }

        // ImGui initialize
        if (!initImGui(IMGUI_VIEWID, gTee->gfxDriver, gAlloc, conf.keymap,
                       conf.uiIniFilename.cstr(), platform ? platform->nwh : nullptr)) {
            TEE_ERROR("Initializing ImGui failed");
            return false;
        }

        if (!gfx::initSpriteSystem(gTee->gfxDriver, gAlloc)) {
            TEE_ERROR("Initializing Sprite System failed");
            return false;
        }
        gfx::registerSpriteSheetToAssetLib();

        if (!gfx::initMaterialLib(gAlloc, gTee->gfxDriver)) {
            TEE_ERROR("Initializing material lib failed");
            return false;
        }
    }

    // Physics2D Driver
    if (!conf.phys2dName.isEmpty()) {
        PluginHandle phys2dPlugin = findPlugin(conf.phys2dName.cstr(), PluginType::Physics2dDriver);
        if (phys2dPlugin.isValid()) {
            gTee->phys2dDriver = (PhysDriver2D*)initPlugin(phys2dPlugin, gAlloc);
        }

        if (!gTee->phys2dDriver) {
            TEE_ERROR("Core init failed: Could not detect Physics driver: %s", conf.phys2dName.cstr());
            return false;
        }

        const PluginDesc& desc = getPluginDesc(phys2dPlugin);
        BX_BEGINP("Initializing Physics2D Driver: %s v%d.%d", desc.name, TEE_VERSION_MAJOR(desc.version),
                  TEE_VERSION_MINOR(desc.version));
        if (!gTee->phys2dDriver->init(gAlloc, BX_ENABLED(termite_DEV) ? PhysFlags2D::EnableDebug : 0, NANOVG_VIEWID)) {
            BX_END_FATAL();
            TEE_ERROR("Core init failed: Could not initialize Physics2D driver");
            return false;
        }
        BX_END_OK();
    }

    // Sound device
    if (!conf.soundName.isEmpty()) {
        PluginHandle sndPlugin = findPlugin(conf.soundName.cstr(), PluginType::SimpleSoundDriver);
        if (sndPlugin.isValid()) {
            gTee->sndDriver = (SimpleSoundDriver*)initPlugin(sndPlugin, gAlloc);
        }

        if (!gTee->sndDriver) {
            TEE_ERROR("Core init failed: Could not detect Sound driver: %s", conf.soundName.cstr());
            return false;
        }

        const PluginDesc& desc = getPluginDesc(sndPlugin);
        BX_BEGINP("Initializing Sound Driver: %s v%d.%d", desc.name, TEE_VERSION_MAJOR(desc.version), TEE_VERSION_MINOR(desc.version));
        if (!gTee->sndDriver->init(conf.audioFreq, conf.audioChannels, conf.audioBufferSize)) {
            BX_END_FATAL();
            TEE_ERROR("Core init failed: Could not initialize Sound driver");
            return false;
        }
        BX_END_OK();
    }

    // Job Dispatcher
    if ((conf.engineFlags & InitEngineFlags::EnableJobDispatcher) == InitEngineFlags::EnableJobDispatcher) {
        BX_BEGINP("Initializing Job Dispatcher");
        if (!initJobDispatcher(gAlloc, conf.maxSmallFibers, conf.smallFiberSize*1024, conf.maxBigFibers, 
                              conf.bigFiberSize*1024, 
                              (conf.engineFlags & InitEngineFlags::LockThreadsToCores) == InitEngineFlags::LockThreadsToCores)) 
        {
            TEE_ERROR("Core init failed: Job Dispatcher init failed");
            BX_END_FATAL();
            return false;
        }
        BX_END_OK();
        BX_TRACE("%d Worker threads spawned", getNumWorkerThreads());
    }

	// Component System
	BX_BEGINP("Initializing Component System");
	if (!ecs::init(gAlloc)) {
		TEE_ERROR("Core init failed: Could not initialize Component-System");
		BX_END_FATAL();
		return false;
	}
	BX_END_OK();

    BX_BEGINP("Initializing Event Dispatcher");
    if (!initEventDispatcher(gAlloc)) {
        TEE_ERROR("Core init fialed: Could not initialize Event Dispatcher");
        BX_END_FATAL();
        return false;
    }
    BX_END_OK();

#ifdef termite_SDL2
    BX_BEGINP("Initializing SDL2 utils");
    if (!sdl::init(gAlloc)) {
        TEE_ERROR("Core init failed: Could not initialize SDL2 utils");
        BX_END_FATAL();
        return false;
    }
    BX_END_OK();
#endif

#if termite_DEV
    BX_BEGINP("Initializing Command System");
    if (!cmd::init(conf.cmdHistorySize, gAlloc)) {
        TEE_ERROR("Core init failed: Could not initialize Command System");
        BX_END_FATAL();
        return false;
    }
    BX_END_OK();
#endif

#if RMT_ENABLED
    BX_BEGINP("Initializing Remotery");
    rmtSettings* rsettings = rmt_Settings();
    rsettings->malloc = remoteryMallocCallback;
    rsettings->free = remoteryFreeCallback;
    rsettings->realloc = remoteryReallocCallback;
#  if termite_DEV
    gTee->consoleCmds.create(64, 64, gAlloc);
    rsettings->input_handler = remoteryInputHandlerCallback;
#endif

    if (rmt_CreateGlobalInstance(&gTee->rmt) != RMT_ERROR_NONE) {
        BX_END_NONFATAL();
    }
    BX_END_OK();
#endif

#ifdef termite_CURL
    BX_BEGINP("Initializing Http Client");
    if (!http::init(gAlloc)) {
        TEE_ERROR("Core init failed: Could not initialize SDL2 utils");
        BX_END_FATAL();
        return false;
    }
    BX_END_OK();
#endif

    lang::registerToAssetLib();

    gTee->init = true;
    return true;
}

void shutdown(ShutdownCallback callback, void* userData)
{
    if (!gTee) {
        assert(false);
        return;
    }

#if termite_CURL
    BX_BEGINP("Shutting down Http Client");
    http::shutdown();
    BX_END_OK();
#endif

#if RMT_ENABLED
    BX_BEGINP("Shutting down Remotery");
    if (gTee->rmt)
        rmt_DestroyGlobalInstance(gTee->rmt);
    for (int i = 0; i < gTee->consoleCmds.getCount(); ++i) {
        ConsoleCommand* cmd = gTee->consoleCmds.itemPtr(i);
        cmd->~ConsoleCommand();
    }
    gTee->consoleCmds.destroy();
    BX_END_OK();
#endif

#if termite_DEV
    BX_BEGINP("Shutting down Command System");
    cmd::shutdown();
    BX_END_OK();
#endif

#ifdef termite_SDL2
    BX_BEGINP("Shutting down SDL2 utils");
    sdl::shutdown();
    BX_END_OK();
#endif

    BX_BEGINP("Shutting down Event Dispatcher");
    shutdownEventDispatcher();
    BX_END_OK();

	BX_BEGINP("Shutting down Component System");
	ecs::shutdown();
	BX_END_OK();

	BX_BEGINP("Shutting down Job Dispatcher");
    shutdownJobDispatcher();
	BX_END_OK();

    if (gTee->phys2dDriver) {
        BX_BEGINP("Shutting down Physics2D Driver");
        gTee->phys2dDriver->shutdown();
        gTee->phys2dDriver = nullptr;
        BX_END_OK();
    }

    BX_BEGINP("Shutting down Graphics Subsystems");
    gfx::shutdownMaterialLib();
    gfx::shutdownSpriteSystem();
	shutdownImGui();
    gfx::shutdownDebugDraw();
    gfx::shutdownDebugDraw2D();
    gfx::shutdownFontSystem();
    gfx::shutdownModelLoader();
    gfx::shutdownTextureLoader();
    gfx::shutdownGfxUtils();
    BX_END_OK();

    if (gTee->renderer) {
        BX_BEGINP("Shutting down Renderer");
        gTee->renderer->shutdown();
        gTee->renderer = nullptr;
        BX_END_OK();
    }

    if (gTee->gfxDriver) {
        BX_BEGINP("Shutting down Graphics Driver");
        gTee->gfxDriver->shutdown();
        gTee->gfxDriver = nullptr;
        BX_END_OK();
        dumpGfxLog();
    }

    if (gTee->sndDriver) {
        BX_BEGINP("Shutting down Sound Driver");
        gTee->sndDriver->shutdown();
        gTee->sndDriver = nullptr;
        BX_END_OK();
    }

    asset::shutdown();
    
    // User Shutdown happens before IO and memory stuff
    // In order for user to clean-up any memory or save stuff
    if (callback) {
        callback(userData);
    }

    if (gTee->ioDriver) {
        BX_BEGINP("Shutting down IO Driver");
        gTee->ioDriver->blocking->shutdown();
        gTee->ioDriver->async->shutdown();
        gTee->ioDriver = nullptr;
        BX_END_OK();
    }

    BX_BEGINP("Shutting down Plugin system");
    shutdownPluginSystem();
    BX_END_OK();

    if (gTee->gfxLogCache) {
        BX_FREE(gAlloc, gTee->gfxLogCache);
        gTee->gfxLogCache = nullptr;
        gTee->numGfxLogCache = 0;
    }

    BX_BEGINP("Destroying Memory pools");
    gTee->memPool.destroy();
    shutdownMemoryPool();
    BX_END_OK();

    if (gTee->randomPoolFloat)
        BX_FREE(gAlloc, gTee->randomPoolFloat);
    if (gTee->randomPoolInt)
        BX_FREE(gAlloc, gTee->randomPoolInt);

    err::shutdown();
    BX_DELETE(gAlloc, gTee);
    gTee = nullptr;

#ifdef _DEBUG
    stb_leakcheck_dumpmem();
#endif
}

static double calcAvgFrameTime(const FrameData& fd)
{
    double sum = 0;
    for (int i = 0; i < BX_COUNTOF(fd.frameTimes); i++) {
        sum += fd.frameTimes[i];
    }
    sum /= double(BX_COUNTOF(fd.frameTimes));
    return sum;
}

void doFrame()
{
    rmt_BeginCPUSample(DoFrame, 0);
    gTee->tempAlloc.free();

    FrameData& fd = gTee->frameData;
    if (fd.frame == 0)
        fd.lastFrameTimePt = TClock::now();

    TClockTimePt frameTimePt = TClock::now();
    std::chrono::duration<double> dt_fp = frameTimePt - fd.lastFrameTimePt;
    double dt = gTee->timeMultiplier * dt_fp.count();
    float fdt = float(dt);

    if (gTee->gfxDriver) {
        ImGui::GetIO().DeltaTime = float(dt_fp.count());
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    rmt_BeginCPUSample(Game_Update, 0);
    if (gTee->updateFn)
        gTee->updateFn(fdt);
    rmt_EndCPUSample(); // Game_Update
    
    runEventDispatcher(fdt);

    rmt_BeginCPUSample(ImGui_Render, 0);
    if (gTee->gfxDriver) {
        ImGui::Render();
        ImGui::GetIO().MouseWheel = 0;
    }
    rmt_EndCPUSample(); // ImGuiRender

    if (gTee->renderer)
        gTee->renderer->render(nullptr);

    rmt_BeginCPUSample(Async_Loop, 0);
    if (gTee->ioDriver->async)
        gTee->ioDriver->async->runAsyncLoop();
    rmt_EndCPUSample(); // Async_Loop

    rmt_BeginCPUSample(Gfx_DrawFrame, 0);
    if (gTee->gfxDriver)
        gTee->gfxDriver->frame();
    rmt_EndCPUSample(); // Gfx_DrawFrame

#ifdef termite_CURL
    http::update();     // Async Request process
#endif

    fd.frame++;
    fd.elapsedTime += dt;
    fd.frameTime = dt;
    fd.lastFrameTimePt = frameTimePt;
    int frameTimeIter = fd.frame % BX_COUNTOF(fd.frameTimes);
    fd.frameTimes[frameTimeIter] = dt;
    fd.avgFrameTime = calcAvgFrameTime(fd);
    double fpsTime = fd.elapsedTime - fd.fpsTime;
    if (frameTimeIter == 0 && fpsTime != 0) {
        fd.fps = BX_COUNTOF(fd.frameTimes) / fpsTime;
        fd.fpsTime = fd.elapsedTime;
    }
    rmt_EndCPUSample(); // DoFrame
}

void pause()
{
    gTee->timeMultiplier = 0;
}

void resume()
{
    gTee->timeMultiplier = 1.0;
    gTee->frameData.lastFrameTimePt = TClock::now();
}

bool isPaused()
{
    return gTee->timeMultiplier == 0;
}

void resetTempAlloc()
{
    gTee->tempAlloc.free();
}

void resetBackbuffer(uint16_t width, uint16_t height)
{
    if (gTee->gfxDriver)
        gTee->gfxDriver->reset(width, height, gTee->conf.gfxDriverFlags);
    gTee->conf.gfxWidth = width;
    gTee->conf.gfxHeight = height;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
}

double getFrameTime()
{
    return gTee->frameData.frameTime;
}

double getElapsedTime()
{
    return gTee->frameData.elapsedTime;
}

double getFps()
{
    return gTee->frameData.fps;
}

double getSmoothFrameTime()
{
    return gTee->frameData.avgFrameTime;
}

uint64_t getFrameIndex()
{
    return gTee->frameData.frame;
}

MemoryBlock* createMemoryBlock(uint32_t size, bx::AllocatorI* alloc)
{
    gTee->memPoolLock.lock();
    HeapMemoryImpl* mem = gTee->memPool.newInstance();
    gTee->memPoolLock.unlock();
    if (!alloc)
        alloc = gAlloc;
    mem->m.data = (uint8_t*)BX_ALLOC(alloc, size);

    if (!mem->m.data)
        return nullptr;
    mem->m.size = size;
    mem->alloc = alloc;

    return (MemoryBlock*)mem;
}

MemoryBlock* refMemoryBlockPtr(const void* data, uint32_t size)
{
    gTee->memPoolLock.lock();
    HeapMemoryImpl* mem = gTee->memPool.newInstance();
    gTee->memPoolLock.unlock();
    mem->m.data = (uint8_t*)const_cast<void*>(data);
    mem->m.size = size;

    return (MemoryBlock*)mem;
}

MemoryBlock* copyMemoryBlock(const void* data, uint32_t size, bx::AllocatorI* alloc)
{
    gTee->memPoolLock.lock();
    HeapMemoryImpl* mem = gTee->memPool.newInstance();
    gTee->memPoolLock.unlock();
    if (!alloc)
        alloc = gAlloc;
    mem->m.data = (uint8_t*)BX_ALLOC(alloc, size);
    if (!mem->m.data)
        return nullptr;
    memcpy(mem->m.data, data, size);
    mem->m.size = size; 
    mem->alloc = alloc;

    return (MemoryBlock*)mem;
}

MemoryBlock* refMemoryBlock(MemoryBlock* mem)
{
    HeapMemoryImpl* m = (HeapMemoryImpl*)mem;
    bx::atomicFetchAndAdd(&m->refcount, 1);
    return mem;
}

void releaseMemoryBlock(MemoryBlock* mem)
{
    HeapMemoryImpl* m = (HeapMemoryImpl*)mem;
    if (bx::atomicDec(&m->refcount) == 0) {
        if (m->alloc) {
            BX_FREE(m->alloc, m->m.data);
            m->m.data = nullptr;
            m->m.size = 0;
        }

        bx::LockScope(gTee->memPoolLock);
        gTee->memPool.deleteInstance(m);
    }
}

MemoryBlock* readTextFile(const char* absFilepath)
{
    bx::FileReader file;
    bx::Error err;
    if (!file.open(absFilepath, &err))
        return nullptr;
    uint32_t size = (uint32_t)file.seek(0, bx::Whence::End);

    MemoryBlock* mem = createMemoryBlock(size + 1, gAlloc);
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

MemoryBlock* readBinaryFile(const char* absFilepath)
{
    bx::FileReader file;
    bx::Error err;
    if (!file.open(absFilepath, &err))
        return nullptr;
    uint32_t size = (uint32_t)file.seek(0, bx::Whence::End);
    if (size == 0) {
        file.close();
        return nullptr;
    }

    MemoryBlock* mem = createMemoryBlock(size, gAlloc);
    if (!mem) {
        file.close();
        return nullptr;
    }

    file.seek(0, bx::Whence::Begin);
    file.read(mem->data, size, &err);
    file.close();

    return mem;
}

bool saveBinaryFile(const char* absFilepath, const MemoryBlock* mem)
{
    bx::FileWriter file;
    bx::Error err;
    if (mem->size == 0 || !file.open(absFilepath, false, &err)) {
        return false;
    }

    int wb = file.write(mem->data, mem->size, &err);
    file.close();
    return wb == mem->size ? true : false;
}

MemoryBlock* encryptMemoryAES128(const MemoryBlock* mem, bx::AllocatorI* alloc, const uint8_t* key, const uint8_t* iv)
{
    assert(mem);
    if (key == nullptr)
        key = kAESKey;
    if (iv == nullptr)
        iv = kAESIv;
    if (alloc == nullptr)
        alloc = gAlloc;

    EncodeHeader header;
    header.sign = T_ENC_SIGN;
    header.version = T_ENC_VERSION;
    
    // Compress LZ4
    int maxSize = BX_ALIGN_16(LZ4_compressBound(mem->size));
    void* compressedBuff = BX_ALLOC(alloc, maxSize);
    if (!compressedBuff)
        return nullptr;
    int compressSize = LZ4_compress_default((const char*)mem->data, (char*)compressedBuff, mem->size, maxSize);
    int alignedCompressSize = BX_ALIGN_16(compressSize);
    assert(alignedCompressSize <= maxSize);
    bx::memSet((uint8_t*)compressedBuff + compressSize, 0x00, alignedCompressSize - compressSize);

    // AES Encode
    MemoryBlock* encMem = createMemoryBlock(alignedCompressSize + sizeof(header), alloc);
    if (encMem) {
        AES_CBC_encrypt_buffer((uint8_t*)encMem->data + sizeof(header), (uint8_t*)compressedBuff, alignedCompressSize, key, iv);

        // finalize header and slap it at the begining of the output buffer
        header.decodeSize = compressSize;
        header.uncompSize = (int)mem->size;
        *((EncodeHeader*)encMem->data) = header;
    }

    BX_FREE(alloc, compressedBuff);
    return encMem;
}

MemoryBlock* decryptMemoryAES128(const MemoryBlock* mem, bx::AllocatorI* alloc, const uint8_t* key, const uint8_t* iv)
{
    assert(mem);
    if (key == nullptr)
        key = kAESKey;
    if (iv == nullptr)
        iv = kAESIv;
    if (alloc == nullptr)
        alloc = gAlloc;

    // AES Decode
    const EncodeHeader header = *((const EncodeHeader*)mem->data);
    if (header.sign != T_ENC_SIGN ||
        header.version != T_ENC_VERSION)
    {
        return nullptr;
    }

    uint8_t* encBuff = (uint8_t*)mem->data + sizeof(header);
    uint32_t encSize = mem->size - sizeof(header);
    assert(encSize % 16 == 0);

    void* decBuff = BX_ALLOC(alloc, encSize);
    if (!decBuff)
        return nullptr;
    AES_CBC_decrypt_buffer((uint8_t*)decBuff, encBuff, encSize, key, iv);
    
    // Uncompress LZ4
    MemoryBlock* rmem = createMemoryBlock(header.uncompSize, alloc);
    if (rmem)
        LZ4_decompress_safe((const char*)decBuff, (char*)rmem->data, header.decodeSize, header.uncompSize);

    BX_FREE(alloc, decBuff);
    return rmem;
}

void cipherXOR(uint8_t* outputBuff, const uint8_t* inputBuff, size_t buffSize, const uint8_t* key, size_t keySize)
{
    assert(buffSize > 0);
    assert(keySize > 0);
    for (size_t i = 0; i < buffSize; i++) {
        outputBuff[i] = inputBuff[i] ^ key[i % keySize];
    }
}

void restartRandom()
{
    std::uniform_int_distribution<int> idist(0, INT_MAX);
    int* randomInt = gTee->randomPoolInt;
    for (int i = 0; i < RANDOM_NUMBER_POOL; i++) {
        randomInt[i] = idist(gTee->randEngine);
    }

    std::uniform_real_distribution<float> fdist(0, 1.0f);
    float* randomFloat = gTee->randomPoolFloat;
    for (int i = 0; i < RANDOM_NUMBER_POOL; i++) {
        randomFloat[i] = fdist(gTee->randEngine);
    }

    gTee->randomFloatOffset = gTee->randomIntOffset = 0;
}

float getRandomFloatUniform(float a, float b)
{
    assert(a <= b);
    int off = gTee->randomFloatOffset;
    float r = (gTee->randomPoolFloat[off]*(b - a)) + a;
    bx::atomicExchange<int32_t>(&gTee->randomFloatOffset, (off + 1) % RANDOM_NUMBER_POOL);
    return r;
}

int getRandomIntUniform(int a, int b)
{
    assert(a <= b);
    int off = gTee->randomIntOffset;
    int r = (gTee->randomPoolInt[off] % (b - a + 1)) + a;
    bx::atomicExchange<int32_t>(&gTee->randomIntOffset, (off + 1) % RANDOM_NUMBER_POOL);
    return r;
}

float getRandomFloatNormal(float mean, float sigma)
{
    std::normal_distribution<float> dist(mean, sigma);
    return dist(gTee->randEngine);
}

void inputSendChars(const char* chars)
{
	ImGuiIO& io = ImGui::GetIO();
    io.AddInputCharactersUTF8(chars);
}

void inputSendKeys(const bool keysDown[512], bool shift, bool alt, bool ctrl)
{
	ImGuiIO& io = ImGui::GetIO();
	memcpy(io.KeysDown, keysDown, 512*sizeof(bool));
	io.KeyShift = shift;
	io.KeyAlt = alt;
	io.KeyCtrl = ctrl;
}

void inputSendMouse(float mousePos[2], int mouseButtons[3], float mouseWheel)
{
	ImGuiIO& io = ImGui::GetIO();
	io.MousePos = ImVec2(mousePos[0], mousePos[1]);

	io.MouseDown[0] = mouseButtons[0] ? true : false;
	io.MouseDown[1] = mouseButtons[1] ? true : false;
	io.MouseDown[2] = mouseButtons[2] ? true : false;

	io.MouseWheel += mouseWheel;
}

GfxDriver* getGfxDriver()
{
    return gTee->gfxDriver;
}

IoDriver* getBlockingIoDriver() TEE_THREAD_SAFE
{
    return gTee->ioDriver->blocking;
}

IoDriver* getAsyncIoDriver() TEE_THREAD_SAFE
{
    return gTee->ioDriver->async;
}

RendererApi* getRenderer() TEE_THREAD_SAFE
{
    return gTee->renderer;
}

SimpleSoundDriver* getSoundDriver() TEE_THREAD_SAFE
{
    return gTee->sndDriver;
}

PhysDriver2D* getPhys2dDriver() TEE_THREAD_SAFE
{
    return gTee->phys2dDriver;
}

uint32_t getEngineVersion() TEE_THREAD_SAFE
{
    return TEE_MAKE_VERSION(0, 1);
}

bx::AllocatorI* getHeapAlloc() TEE_THREAD_SAFE
{
    return gAlloc;
}

bx::AllocatorI* getTempAlloc() TEE_THREAD_SAFE
{
    return &gTee->tempAlloc;
}

const Config& getConfig() TEE_THREAD_SAFE
{
    return gTee->conf;
}

Config* getMutableConfig() TEE_THREAD_SAFE
{
    return &gTee->conf;
}

const char* getCacheDir() TEE_THREAD_SAFE
{
    return gCacheDir.cstr();
}

const char* getDataDir() TEE_THREAD_SAFE
{
    return gDataDir.cstr();
}

void dumpGfxLog() TEE_THREAD_SAFE
{
    if (gTee->gfxLogCache) {
        for (int i = 0, c = gTee->numGfxLogCache; i < c; i++) {
            const LogCache& l = gTee->gfxLogCache[i];
            bx::logPrint(__FILE__, __LINE__, l.type, l.text);
        }

        BX_FREE(gAlloc, gTee->gfxLogCache);
        gTee->gfxLogCache = nullptr;
        gTee->numGfxLogCache = 0;
    }
}

bool needGfxReset() TEE_THREAD_SAFE
{
    return gTee->gfxReset;
}

void shutdownGraphics()
{
    // Unload all graphics resources
    asset::unloadAssets("texture");

    // Unload subsystems
    gfx::shutdownSpriteSystemGraphics();
    shutdownImGui();
    gfx::shutdownDebugDraw();
    gfx::shutdownDebugDraw2D();
    gfx::shutdownFontSystemGraphics();
    gfx::shutdownModelLoader();
    gfx::shutdownTextureLoader();
    gfx::shutdownGfxUtils();
    gfx::destroyMaterialUniforms();

    if (gTee->phys2dDriver) {
        gTee->phys2dDriver->shutdownGraphicsObjects();
    }

    // Shutdown driver
    if (gTee->gfxDriver) {
        gTee->gfxDriver->shutdown();
        gTee->gfxDriver = nullptr;
        dumpGfxLog();
    }
}

bool resetGraphics(const GfxPlatformData* platform)
{
    const Config& conf = gTee->conf;

    PluginHandle gfxPlugin = findPlugin(conf.gfxName.cstr(), PluginType::GraphicsDriver);
    if (gfxPlugin.isValid()) {
        gTee->gfxDriver = (GfxDriver*)initPlugin(gfxPlugin, gAlloc);
    }

    if (!gTee->gfxDriver) {
        TEE_ERROR("Core init failed: Could not detect Graphics driver: %s", conf.gfxName.cstr());
        return false;
    }

    const PluginDesc& desc = getPluginDesc(gfxPlugin);
    BX_BEGINP("Initializing Graphics Driver: %s v%d.%d", desc.name, TEE_VERSION_MAJOR(desc.version),
              TEE_VERSION_MINOR(desc.version));
    if (platform)
        gTee->gfxDriver->setPlatformData(*platform);
    if (!gTee->gfxDriver->init(conf.gfxDeviceId, &gTee->gfxDriverEvents, gAlloc)) {
        BX_END_FATAL();
        dumpGfxLog();
        TEE_ERROR("Core init failed: Could not initialize Graphics driver");
        return false;
    }
    BX_END_OK();
    dumpGfxLog();

    // Initialize Renderer with Gfx Driver
    if (gTee->renderer) {
        BX_BEGINP("Initializing Renderer");
        if (!gTee->renderer->init(gAlloc, gTee->gfxDriver)) {
            BX_END_FATAL();
            TEE_ERROR("Core init failed: Could not initialize Renderer");
            return false;
        }
        BX_END_OK();
    }

    // Init and Register graphics resource loaders
    gfx::initTextureLoader(gTee->gfxDriver, gAlloc);
    gfx::registerTextureToAssetLib();

    gfx::initModelLoader(gTee->gfxDriver, gAlloc);
    gfx::registerModelToAssetLib();

    gfx::initFontSystemGraphics();

    // VectorGraphics
    if (!gfx::initDebugDraw2D(gAlloc, gTee->gfxDriver)) {
        TEE_ERROR("Initializing Vector Graphics failed");
        return false;
    }

    // Debug graphics
    if (!gfx::initDebugDraw(gAlloc, gTee->gfxDriver)) {
        TEE_ERROR("Initializing Editor Draw failed");
        return false;
    }

    // Graphics Utilities
    if (!gfx::initGfxUtils(gTee->gfxDriver)) {
        TEE_ERROR("Initializing Graphics Utilities failed");
        return false;
    }

    // ImGui initialize
    if (!initImGui(IMGUI_VIEWID, gTee->gfxDriver, gAlloc, conf.keymap, conf.uiIniFilename.cstr(), 
                   platform ? platform->nwh : nullptr)) 
    {
        TEE_ERROR("Initializing ImGui failed");
        return false;
    }

    if (!gfx::initSpriteSystemGraphics(gTee->gfxDriver)) {
        TEE_ERROR("Initializing Sprite System failed");
        return false;
    }

    if (gTee->phys2dDriver) {
        gTee->phys2dDriver->initGraphicsObjects();
    }

    if (!gfx::createMaterialUniforms(gTee->gfxDriver)) {
        TEE_ERROR("Initializing material uniforms failed");
        return false;
    }

    // Reload resources
    asset::reloadAssets("texture");

    gTee->gfxReset = false;
    return true;
}

void registerConsoleCommand(const char* name, std::function<void(int, const char**)> callback)
{
#if termite_DEV && RMT_ENABLED
    assert(gTee);
    ConsoleCommand* cmd = new(gTee->consoleCmds.push()) ConsoleCommand;
    cmd->cmdHash = tinystl::hash_string(name, strlen(name));
    cmd->callback = callback;
#endif
}

const HardwareInfo& getHardwareInfo()
{
    return gHwInfo;
}

void GfxDriverEvents::onFatal(GfxFatalType::Enum type, const char* str)
{
    char strTrimed[LOG_STRING_SIZE];
    bx::strCopy(strTrimed, sizeof(strTrimed), str);
    strTrimed[strlen(strTrimed) - 1] = 0;

    if (gTee->numGfxLogCache < 1000) {
        m_lock.lock();
        gTee->gfxLogCache = (LogCache*)BX_REALLOC(gAlloc, gTee->gfxLogCache, sizeof(LogCache) * (++gTee->numGfxLogCache));
        gTee->gfxLogCache[gTee->numGfxLogCache-1].type = bx::LogType::Fatal;
        strcpy(gTee->gfxLogCache[gTee->numGfxLogCache-1].text, strTrimed);
        m_lock.unlock();
    }
}

void GfxDriverEvents::onTraceVargs(const char* filepath, int line, const char* format, va_list argList)
{
    char text[LOG_STRING_SIZE];
    vsnprintf(text, sizeof(text), format, argList);
    text[strlen(text) - 1] = 0;
    if (gTee->numGfxLogCache < 1000) {
        m_lock.lock();
        gTee->gfxLogCache = (LogCache*)BX_REALLOC(gAlloc, gTee->gfxLogCache, sizeof(LogCache) * (++gTee->numGfxLogCache));
        gTee->gfxLogCache[gTee->numGfxLogCache-1].type = bx::LogType::Verbose;
        strcpy(gTee->gfxLogCache[gTee->numGfxLogCache-1].text, text);
        m_lock.unlock();
    }

}

} // namespace tee