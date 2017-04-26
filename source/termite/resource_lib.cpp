#include "pch.h"

#include "resource_lib.h"
#include "io_driver.h"

#include "bxx/logger.h"
#include "bxx/hash_table.h"
#include "bxx/path.h"
#include "bxx/handle_pool.h"

#include "../include_common/folder_png.h"

using namespace termite;

struct ResourceTypeData
{
    char name[32];
    ResourceCallbacksI* callbacks;
    int userParamsSize;
    uintptr_t failObj;
    uintptr_t asyncProgressObj;
};

struct Resource
{
    bx::AllocatorI* objAlloc;
    ResourceHandle handle;
    ResourceCallbacksI* callbacks;
    uint8_t userParams[T_RESOURCE_MAX_USERPARAM_SIZE];
    bx::Path uri;
    uint32_t refcount;
    uintptr_t obj;
    size_t typeNameHash;
    uint32_t paramsHash;
    ResourceLoadState::Enum loadState;
};

struct AsyncLoadRequest
{
    ResourceHandle handle;
    ResourceFlag::Bits flags;
};

namespace termite
{
    class ResourceLib : public IoDriverEventsI
    {
    public:
        ResourceLibInitFlag::Bits flags;
        IoDriverApi* driver;
        IoOperationMode::Enum opMode;
        bx::HandlePool resourceTypes;
        bx::HashTableUint16 resourceTypesTable;    // hash(name) -> handle in resourceTypes
        bx::HandlePool resources;
        bx::HashTableUint16 resourcesTable;        // hash(uri+params) -> handle in resources
        bx::HandlePool asyncLoads;
        bx::HashTableUint16 asyncLoadsTable;       // hash(uri) -> handle in asyncLoads
        bx::MultiHashTable<uint16_t> hotLoadsTable;    // hash(uri) -> list of handles in resources
		bx::Pool<bx::MultiHashTable<uint16_t>::Node> hotLoadsNodePool;
        FileModifiedCallback modifiedCallback;
        void* fileModifiedUserParam;
        bx::AllocatorI* alloc;

    public:
        ResourceLib(bx::AllocatorI* _alloc) : 
            resourceTypesTable(bx::HashTableType::Mutable),
            resourcesTable(bx::HashTableType::Mutable),
            asyncLoadsTable(bx::HashTableType::Mutable),
            hotLoadsTable(bx::HashTableType::Mutable),
            alloc(_alloc)
        {
            driver = nullptr;
            opMode = IoOperationMode::Async;
            flags = ResourceLibInitFlag::None;
            modifiedCallback = nullptr;
            fileModifiedUserParam = nullptr;
        }
        
        virtual ~ResourceLib()
        {
        }

        void onOpenError(const char* uri) override;
        void onReadError(const char* uri) override;
        void onReadComplete(const char* uri, MemoryBlock* mem) override;
        void onModified(const char* uri) override;

        void onWriteError(const char* uri) override  {}
        void onWriteComplete(const char* uri, size_t size) override  {}
        void onOpenStream(IoStream* stream) override  {}
        void onReadStream(IoStream* stream, MemoryBlock* mem) override {}
        void onCloseStream(IoStream* stream) override {}
        void onWriteStream(IoStream* stream, size_t size) override {}
    };
}   // namespace termite

static ResourceLib* g_resLib = nullptr;

result_t termite::initResourceLib(ResourceLibInitFlag::Bits flags, IoDriverApi* driver, bx::AllocatorI* alloc)
{
	assert(driver);
    if (g_resLib) {
        assert(0);
        return T_ERR_ALREADY_INITIALIZED;
    }

	ResourceLib* resLib = BX_NEW(alloc, ResourceLib)(alloc);
	if (!resLib)
		return T_ERR_OUTOFMEM;
    g_resLib = resLib;

	resLib->driver = driver;
	resLib->opMode = driver->getOpMode();
	resLib->flags = flags;

	if (resLib->opMode == IoOperationMode::Async)
		driver->setCallbacks(resLib);

    uint32_t resourceTypeSz = sizeof(ResourceTypeData);
	if (!resLib->resourceTypes.create(&resourceTypeSz, 1, 20, 20, alloc) || !resLib->resourceTypesTable.create(20, alloc)) {
		return T_ERR_OUTOFMEM;
	}

    uint32_t resourceSz = sizeof(Resource);
	if (!resLib->resources.create(&resourceSz, 1, 256, 1024, alloc) || !resLib->resourcesTable.create(256, alloc)) {
		return T_ERR_OUTOFMEM;
	}

    uint32_t asyncLoadReqSz = sizeof(AsyncLoadRequest);
	if (!resLib->asyncLoads.create(&asyncLoadReqSz, 1, 32, 64, alloc) || !resLib->asyncLoadsTable.create(64, alloc)) {
		return T_ERR_OUTOFMEM;
	}

	if (!resLib->hotLoadsNodePool.create(128, alloc) ||
		!resLib->hotLoadsTable.create(128, alloc, &resLib->hotLoadsNodePool)) 
    {
        return T_ERR_OUTOFMEM;
    }

    return 0;
}

void termite::shutdownResourceLib()
{
    ResourceLib* resLib = g_resLib;
    if (!resLib)
        return;

    // Got to clear the callbacks of the driver if DataStore is overloading it
    if (resLib->driver && resLib->driver->getCallbacks() == resLib) {
        resLib->driver->setCallbacks(nullptr);
    }

    resLib->hotLoadsTable.destroy();
	resLib->hotLoadsNodePool.destroy();

    resLib->asyncLoads.destroy();
    resLib->asyncLoadsTable.destroy();

    resLib->resourceTypesTable.destroy();
    resLib->resourceTypes.destroy();

    resLib->resources.destroy();
    resLib->resourcesTable.destroy();

    BX_DELETE(resLib->alloc, resLib);
    g_resLib = nullptr;
}

void termite::setFileModifiedCallback(FileModifiedCallback callback, void* userParam)
{
    ResourceLib* resLib = g_resLib;
    resLib->modifiedCallback = callback;
    resLib->fileModifiedUserParam = userParam;
}

IoDriverApi* termite::getResourceLibIoDriver()
{
    ResourceLib* resLib = g_resLib;
    return resLib->driver;
}

ResourceTypeHandle termite::registerResourceType(const char* name, ResourceCallbacksI* callbacks,
                                                int userParamsSize /*= 0*/, uintptr_t failObj /*=0*/, 
                                                 uintptr_t asyncProgressObj/* = 0*/)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);

    if (userParamsSize > T_RESOURCE_MAX_USERPARAM_SIZE)
        return ResourceTypeHandle();

    uint16_t tHandle = resLib->resourceTypes.newHandle();
    assert(tHandle != UINT16_MAX);
    ResourceTypeData* tdata = resLib->resourceTypes.getHandleData<ResourceTypeData>(0, tHandle);
    bx::strlcpy(tdata->name, name, sizeof(tdata->name));
    tdata->callbacks = callbacks;
    tdata->userParamsSize = userParamsSize;
    tdata->failObj = failObj;
    tdata->asyncProgressObj = asyncProgressObj;
    
    ResourceTypeHandle handle(tHandle);
    resLib->resourceTypesTable.add(tinystl::hash_string(tdata->name, strlen(tdata->name)), tHandle);

    return handle;
}

void termite::unregisterResourceType(ResourceTypeHandle handle)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);

    if (handle.isValid()) {
        ResourceTypeData* tdata = resLib->resourceTypes.getHandleData<ResourceTypeData>(0, handle);
        
        int index = resLib->resourceTypesTable.find(tinystl::hash_string(tdata->name, strlen(tdata->name)));
        if (index != -1)
            resLib->resourceTypesTable.remove(index);
        resLib->resourceTypes.freeHandle(handle);
    }
}

inline uint32_t hashResource(const char* uri, const void* userParams, int userParamsSize, bx::AllocatorI* objAlloc)
{
    uintptr_t objAllocu = uintptr_t(objAlloc);

    // Hash uri + params
    bx::HashMurmur2A hash;
    hash.begin();
    hash.add(uri, (int)strlen(uri));
    hash.add(userParams, userParamsSize);
    hash.add(&objAllocu, sizeof(objAllocu));
    return hash.end();
}

static ResourceHandle newResource(ResourceCallbacksI* callbacks, const char* uri, const void* userParams, 
                                  int userParamsSize, uintptr_t obj, size_t typeNameHash, bx::AllocatorI* objAlloc)
{
    ResourceLib* resLib = g_resLib;

    uint16_t rHandle = resLib->resources.newHandle();
    if (rHandle == UINT16_MAX) {
        BX_WARN("Out of Memory");
        return ResourceHandle();
    }
    Resource* rs = resLib->resources.getHandleData<Resource>(0, rHandle);
    memset(rs, 0x00, sizeof(Resource));

    rs->handle = ResourceHandle(rHandle);
    rs->uri = uri;
    rs->refcount = 1;
    rs->callbacks = callbacks;
    rs->obj = obj;
    rs->typeNameHash = typeNameHash;
    rs->objAlloc = objAlloc;

    if (userParams) {
        memcpy(rs->userParams, userParams, userParamsSize);
        rs->paramsHash = bx::hashMurmur2A(userParams, userParamsSize);
    }

    resLib->resourcesTable.add(hashResource(uri, userParams, userParamsSize, objAlloc), rHandle);

    // Register for hot-loading
    // A URI may contain several resources (different load params), so we have to reload them all
    if (resLib->flags & ResourceLibInitFlag::HotLoading) {
        resLib->hotLoadsTable.add(tinystl::hash_string(uri, strlen(uri)), rHandle);
    }

    return rs->handle;
}

static void deleteResource(ResourceHandle handle, const ResourceTypeData* tdata)
{
    ResourceLib* resLib = g_resLib;
    Resource* rs = resLib->resources.getHandleData<Resource>(0, handle);

    // Unregister from hot-loading
    if (resLib->flags & ResourceLibInitFlag::HotLoading) {
        int index = resLib->hotLoadsTable.find(tinystl::hash_string(rs->uri.cstr(), rs->uri.getLength()));
        if (index != -1) {
            bx::MultiHashTable<uint16_t>::Node* node = resLib->hotLoadsTable.getNode(index);
            bx::MultiHashTable<uint16_t>::Node* foundNode = nullptr;
            while (node) {
                if (resLib->resources.getHandleData<Resource>(0, node->value)->paramsHash == rs->paramsHash) {
                    foundNode = node;
                    break;
                }
                node = node->next;
            }

            if (foundNode)
                resLib->hotLoadsTable.remove(index, foundNode);
        }
    }
    resLib->resources.freeHandle(handle);

    // Unload resource and invalidate the handle (just to set a flag that it's unloaded)
    // Ignore async and fail objects
    if (rs->obj != tdata->asyncProgressObj && rs->obj != tdata->failObj)
        rs->callbacks->unloadObj(rs->obj, rs->objAlloc);

    int tIdx = resLib->resourcesTable.find(hashResource(rs->uri.cstr(), rs->userParams, tdata->userParamsSize, rs->objAlloc));
    if (tIdx != -1)
        resLib->resourcesTable.remove(tIdx);
}

static ResourceHandle addResource(ResourceCallbacksI* callbacks, const char* uri, const void* userParams,
                                  int userParamsSize, uintptr_t obj, ResourceHandle overrideHandle, size_t typeNameHash,
                                  bx::AllocatorI* objAlloc)
{
    ResourceLib* resLib = g_resLib;
    ResourceHandle handle = overrideHandle;
    if (handle.isValid()) {
        Resource* rs = resLib->resources.getHandleData<Resource>(0, handle);
        if (rs->handle.isValid())
            rs->callbacks->unloadObj(rs->obj, rs->objAlloc);
        
        rs->handle = handle;
        rs->uri = uri;
        rs->obj = obj;
        rs->callbacks = callbacks;
        memcpy(rs->userParams, userParams, userParamsSize);
    } else {
        handle = newResource(callbacks, uri, userParams, userParamsSize, obj, typeNameHash, objAlloc);
    }

    return handle;
}

static void setResourceLoadFlag(ResourceHandle resHandle, ResourceLoadState::Enum flag) 
{
    ResourceLib* resLib = g_resLib;
    Resource* res = resLib->resources.getHandleData<Resource>(0, resHandle.value);
    res->loadState = flag;
}

static ResourceHandle loadResourceHashed(size_t nameHash, const char* uri, const void* userParams, ResourceFlag::Bits flags,
                                         bx::AllocatorI* objAlloc)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    ResourceHandle handle;
    ResourceHandle overrideHandle;

    if (uri[0] == 0) {
        BX_WARN("Cannot load resource with empty Uri");
        return ResourceHandle();
    }

    // Find resource Type
    int resTypeIdx = resLib->resourceTypesTable.find(nameHash);
    if (resTypeIdx == -1) {
        BX_WARN("ResourceType for '%s' not found in DataStore", uri);
        return ResourceHandle();
    }
    const ResourceTypeData* tdata = resLib->resourceTypes.getHandleData<ResourceTypeData>(0, 
                                                                    resLib->resourceTypesTable[resTypeIdx]);

    // Find the possible already loaded handle by uri+params hash value
    int rresult = resLib->resourcesTable.find(hashResource(uri, userParams, tdata->userParamsSize, objAlloc));
    if (rresult != -1) {
        handle.value = (uint16_t)resLib->resourcesTable[rresult];
    }

    // Resource already exists ?
    if (handle.isValid()) {
        if (flags & ResourceFlag::Reload) {
            // Override the handle and set handle to INVALID in order to reload it later below
            overrideHandle = handle;
            handle = ResourceHandle();
        } else {
            // No Reload flag is set, just add the reference count and return
            resLib->resources.getHandleData<Resource>(0, handle)->refcount++;
        }
    }

    // Load the resource for the first time
    if (!handle.isValid()) {
        if (resLib->opMode == IoOperationMode::Async) {
            // Add resource with an empty object
            handle = addResource(tdata->callbacks, uri, userParams, tdata->userParamsSize,
                                 tdata->asyncProgressObj, overrideHandle, nameHash, objAlloc);
            setResourceLoadFlag(handle, ResourceLoadState::LoadInProgress);

            // Register async request
            uint16_t reqHandle = resLib->asyncLoads.newHandle();
            if (reqHandle == UINT16_MAX) {
                deleteResource(handle, tdata);
                return ResourceHandle();
            }
            AsyncLoadRequest* req = resLib->asyncLoads.getHandleData<AsyncLoadRequest>(0, reqHandle);
            req->handle = handle;
            req->flags = flags;
            resLib->asyncLoadsTable.add(tinystl::hash_string(uri, strlen(uri)), reqHandle);

            // Load the file, result will be called in onReadComplete
            resLib->driver->read(uri, IoPathType::Assets);
        } else {
            // Load the file
            MemoryBlock* mem = resLib->driver->read(uri, IoPathType::Assets);
            if (!mem) {
                BX_WARN("Opening resource '%s' failed", uri);
                BX_WARN(getErrorString());
                if (overrideHandle.isValid())
                    deleteResource(overrideHandle, tdata);
                return ResourceHandle();
            }

            ResourceTypeParams params;
            params.uri = uri;
            params.userParams = userParams;
            params.flags = flags;
            uintptr_t obj;
            bool loaded = tdata->callbacks->loadObj(mem, params, &obj, objAlloc);
            releaseMemoryBlock(mem);

            if (!loaded)    {
                BX_WARN("Loading resource '%s' failed", uri);
                BX_WARN(getErrorString());
                obj = tdata->failObj;
            }

            handle = addResource(tdata->callbacks, uri, userParams, tdata->userParamsSize, obj, overrideHandle, nameHash,
                                 objAlloc);
            setResourceLoadFlag(handle, loaded ? ResourceLoadState::LoadOk : ResourceLoadState::LoadFailed);

            // Trigger onReload callback
            if (flags & ResourceFlag::Reload) {
                tdata->callbacks->onReload(handle, objAlloc);
            }
        }
    }

    return handle;
}

static ResourceHandle getResourceHandleInPlace(const ResourceTypeData* tdata, size_t typeNameHash, const char* uri, 
                                               uintptr_t obj)
{
    ResourceLib* resLib = g_resLib;
    void* userParams = alloca(tdata->userParamsSize);
    memset(userParams, 0x00, tdata->userParamsSize);

    // Find the possible already loaded handle by uri+params hash value
    int rresult = resLib->resourcesTable.find(hashResource(uri, userParams, tdata->userParamsSize, nullptr));
    if (rresult != -1) {
        uint16_t handleIdx = (uint16_t)resLib->resourcesTable.getValue(rresult);
        Resource* rs = resLib->resources.getHandleData<Resource>(0, handleIdx);
        rs->refcount++;
        return ResourceHandle(handleIdx);
    }

    // Create a new failed resource
    ResourceHandle handle = newResource(tdata->callbacks, uri, userParams, tdata->userParamsSize, tdata->failObj,
                                        typeNameHash, nullptr);
    setResourceLoadFlag(handle, ResourceLoadState::LoadFailed);
    return handle;
}

// Secretly create a fail object of the resource and return it
ResourceHandle termite::getResourceFailHandle(const char* name)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);

    size_t typeNameHash = tinystl::hash_string(name, strlen(name));
    int resTypeIdx = resLib->resourceTypesTable.find(typeNameHash);
    if (resTypeIdx == -1) {
        BX_WARN("ResourceType '%s' not found in DataStore", name);
        return ResourceHandle();
    }

    const ResourceTypeData* tdata = 
        resLib->resourceTypes.getHandleData<ResourceTypeData>(0, resLib->resourceTypesTable.getValue(resTypeIdx));

    return getResourceHandleInPlace(tdata, typeNameHash, "[FAIL]", tdata->failObj);
}

 ResourceHandle termite::getResourceAsyncHandle(const char* name)
{
     ResourceLib* resLib = g_resLib;
     assert(resLib);

     size_t typeNameHash = tinystl::hash_string(name, strlen(name));
     int resTypeIdx = resLib->resourceTypesTable.find(typeNameHash);
     if (resTypeIdx == -1) {
         BX_WARN("ResourceType '%s' not found in DataStore", name);
         return ResourceHandle();
     }

     const ResourceTypeData* tdata =
         resLib->resourceTypes.getHandleData<ResourceTypeData>(0, resLib->resourceTypesTable.getValue(resTypeIdx));

     return getResourceHandleInPlace(tdata, typeNameHash, "[ASYNC]", tdata->asyncProgressObj);
}

 ResourceHandle termite::addResourceRef(ResourceHandle handle)
 {
     assert(handle.isValid());
     Resource* rs = g_resLib->resources.getHandleData<Resource>(0, handle);
     rs->refcount++;
     return handle;
 }

 uint32_t termite::getResourceRefCount(ResourceHandle handle)
 {
     assert(handle.isValid());
     return g_resLib->resources.getHandleData<Resource>(0, handle)->refcount;
 }

static ResourceHandle loadResourceHashedInMem(size_t nameHash, const char* uri, const MemoryBlock* mem, 
                                              const void* userParams, ResourceFlag::Bits flags, bx::AllocatorI* objAlloc)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    ResourceHandle handle;
    ResourceHandle overrideHandle;

    // Find resource Type
    int resTypeIdx = resLib->resourceTypesTable.find(nameHash);
    if (resTypeIdx == -1) {
        BX_WARN("ResourceType for '%s' not found in DataStore", uri);
        return ResourceHandle();
    }
    const ResourceTypeData& tdata = *resLib->resourceTypes.getHandleData<ResourceTypeData>(0, 
                                                                      resLib->resourceTypesTable.getValue(resTypeIdx));

    // Find the possible already loaded handle by uri+params hash value
    int rresult = resLib->resourcesTable.find(hashResource(uri, userParams, tdata.userParamsSize, objAlloc));
    if (rresult != -1) {
        handle.value = (uint16_t)resLib->resourcesTable.getValue(rresult);
    }

    // Resource already exists ?
    if (handle.isValid()) {
        if (flags & ResourceFlag::Reload) {
            // Override the handle and set handle to INVALID in order to reload it later below
            overrideHandle = handle;
            handle = ResourceHandle();
        } else {
            // No Reload flag is set, just add the reference count and return
            resLib->resources.getHandleData<Resource>(0, handle)->refcount++;
        }
    }

    // Load the resource  for the first time
    if (!handle.isValid()) {
        assert(mem);

        ResourceTypeParams params;
        params.uri = uri;
        params.userParams = userParams;
        params.flags = flags;
        uintptr_t obj;
        bool loaded = tdata.callbacks->loadObj(mem, params, &obj, objAlloc);

        if (!loaded)    {
            BX_WARN("Loading resource '%s' failed", uri);
            BX_WARN(getErrorString());
            obj = tdata.failObj;
        }

        handle = addResource(tdata.callbacks, uri, userParams, tdata.userParamsSize, obj, overrideHandle, nameHash, objAlloc);
        setResourceLoadFlag(handle, loaded ? ResourceLoadState::LoadOk : ResourceLoadState::LoadFailed);

        // Trigger onReload callback
        if (flags & ResourceFlag::Reload) {
            tdata.callbacks->onReload(handle, objAlloc);
        }
    }

    return handle;
}

ResourceHandle termite::loadResource(const char* name, const char* uri, const void* userParams,
                                     ResourceFlag::Bits flags, bx::AllocatorI* objAlloc)
{
    return loadResourceHashed(tinystl::hash_string(name, strlen(name)), uri, userParams, flags, objAlloc);
}

ResourceHandle termite::loadResourceFromMem(const char* name, const char* uri, const MemoryBlock* mem, 
                                            const void* userParams /*= nullptr*/, ResourceFlag::Bits flags /*= ResourceFlag::None*/,
                                            bx::AllocatorI* objAlloc)
{
    return loadResourceHashedInMem(tinystl::hash_string(name, strlen(name)), uri, mem, userParams, flags, objAlloc);
}

void termite::unloadResource(ResourceHandle handle)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    assert(handle.isValid());

    Resource* rs = resLib->resources.getHandleData<Resource>(0, handle);
    rs->refcount--;
    if (rs->refcount == 0) {
        // Unregister from async loading
        if (resLib->opMode == IoOperationMode::Async) {
            int aIdx = resLib->asyncLoadsTable.find(bx::hashMurmur2A(rs->uri.cstr(), (uint32_t)rs->uri.getLength()));
            if (aIdx != -1) {
                resLib->asyncLoads.freeHandle(resLib->asyncLoadsTable.getValue(aIdx));
                resLib->asyncLoadsTable.remove(aIdx);
            }
        }

        // delete resource and unload resource object
        int resTypeIdx = resLib->resourceTypesTable.find(rs->typeNameHash);
        if (resTypeIdx != -1) {
            const ResourceTypeData* tdata = 
                resLib->resourceTypes.getHandleData<ResourceTypeData>(0, resLib->resourceTypesTable.getValue(resTypeIdx));
            deleteResource(handle, tdata);
        }
    }
}

uintptr_t termite::getResourceObj(ResourceHandle handle)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    assert(handle.isValid());

    return resLib->resources.getHandleData<Resource>(0, handle)->obj;
}

ResourceLoadState::Enum termite::getResourceLoadState(ResourceHandle handle)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    if (handle.isValid())
        return resLib->resources.getHandleData<Resource>(0, handle)->loadState;
    else
        return ResourceLoadState::LoadFailed;
}

int termite::getResourceParamSize(const char* name)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);

    int typeIdx = resLib->resourceTypesTable.find(tinystl::hash_string(name, strlen(name)));
    if (typeIdx != -1)
        return resLib->resourceTypes.getHandleData<ResourceTypeData>(0, 
                                                         resLib->resourceTypesTable.getValue(typeIdx))->userParamsSize;
    else
        return 0;
}

const char* termite::getResourceUri(ResourceHandle handle)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    assert(handle.isValid());
    
    return resLib->resources.getHandleData<Resource>(0, handle)->uri.cstr();
}

const char* termite::getResourceName(ResourceHandle handle)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    assert(handle.isValid());

    int r = resLib->resourceTypesTable.find(resLib->resources.getHandleData<Resource>(0, handle)->typeNameHash);
    if (r != 0) {
        return resLib->resourceTypes.getHandleData<ResourceTypeData>(0, resLib->resourceTypesTable.getValue(r))->name;
    }

    return "";
}

const void* termite::getResourceParams(ResourceHandle handle)
{
    ResourceLib* resLib = g_resLib;
    assert(resLib);
    assert(handle.isValid());

    return resLib->resources.getHandleData<Resource>(0, handle)->userParams;
}

// Async
void termite::ResourceLib::onOpenError(const char* uri)
{
    ResourceLib* resLib = g_resLib;

    int r = this->asyncLoadsTable.find(tinystl::hash_string(uri, strlen(uri)));
    if (r != -1) {
        uint16_t handle = this->asyncLoadsTable.getValue(r);
        AsyncLoadRequest* areq = this->asyncLoads.getHandleData<AsyncLoadRequest>(0, handle);
        BX_WARN("Opening resource '%s' failed", uri);

        if (areq->handle.isValid()) {
            setResourceLoadFlag(areq->handle, ResourceLoadState::LoadFailed);

            // Set fail obj to resource
            Resource* res = resLib->resources.getHandleData<Resource>(0, areq->handle);
            int typeIdx = resLib->resourceTypesTable.find(res->typeNameHash);
            if (typeIdx != -1) {
                res->obj = resLib->resourceTypes.getHandleData<ResourceTypeData>(
                    0, resLib->resourceTypesTable.getValue(typeIdx))->failObj;
            }
        }

        this->asyncLoads.freeHandle(handle);
        this->asyncLoadsTable.remove(r);
    }
}

void termite::ResourceLib::onReadError(const char* uri)
{
    ResourceLib* resLib = g_resLib;

    int r = this->asyncLoadsTable.find(tinystl::hash_string(uri, strlen(uri)));
    if (r != -1) {
        uint16_t handle = this->asyncLoadsTable.getValue(r);
        AsyncLoadRequest* areq = this->asyncLoads.getHandleData<AsyncLoadRequest>(0, handle);
        BX_WARN("Reading resource '%s' failed", uri);

        if (areq->handle.isValid()) {
            setResourceLoadFlag(areq->handle, ResourceLoadState::LoadFailed);

            // Set fail obj to resource
            Resource* res = resLib->resources.getHandleData<Resource>(0, areq->handle);
            int typeIdx = resLib->resourceTypesTable.find(res->typeNameHash);
            if (typeIdx != -1) {
                res->obj = resLib->resourceTypes.getHandleData<ResourceTypeData>(
                    0, resLib->resourceTypesTable.getValue(typeIdx))->failObj;
            }
        }

        this->asyncLoads.freeHandle(handle);
        this->asyncLoadsTable.remove(r);
    }
}

void termite::ResourceLib::onReadComplete(const char* uri, MemoryBlock* mem)
{
    ResourceLib* resLib = g_resLib;
    int r = this->asyncLoadsTable.find(tinystl::hash_string(uri, strlen(uri)));
    if (r != -1) {
        int handle = this->asyncLoadsTable[r];
        AsyncLoadRequest* areq = this->asyncLoads.getHandleData<AsyncLoadRequest>(0, handle);
        this->asyncLoads.freeHandle(handle);

        assert(areq->handle.isValid());
        Resource* rs = this->resources.getHandleData<Resource>(0, areq->handle);

        // Load using the callback
        ResourceTypeParams params;
        params.uri = uri;
        params.userParams = rs->userParams;
        params.flags = areq->flags;
        uintptr_t obj;
        bool loadResult = rs->callbacks->loadObj(mem, params, &obj, rs->objAlloc);
        releaseMemoryBlock(mem);
        this->asyncLoadsTable.remove(r);

        if (!loadResult) {
            BX_WARN("Loading resource '%s' failed", uri);
            BX_WARN(getErrorString());
            rs->loadState = ResourceLoadState::LoadFailed;

            // Set fail obj to resource
            int typeIdx = resLib->resourceTypesTable.find(rs->typeNameHash);
            if (typeIdx != -1) {
                rs->obj = resLib->resourceTypes.getHandleData<ResourceTypeData>(
                    0, resLib->resourceTypesTable.getValue(typeIdx))->failObj;
            }
            return;
        }

        // Update the obj 
        rs->obj = obj;
        rs->loadState = ResourceLoadState::LoadOk;

        // Trigger onReload callback
        if (areq->flags & ResourceFlag::Reload) {
            rs->callbacks->onReload(rs->handle, rs->objAlloc);
        }
    } else {
        releaseMemoryBlock(mem);
    }
}

void termite::ResourceLib::onModified(const char* uri)
{
    // Hot Loading
    // strip "assets/" from the begining of uri
    size_t uriOffset = 0;
    if (strstr(uri, "assets/") == uri)
        uriOffset = strlen("assets/");
    int index = this->hotLoadsTable.find(tinystl::hash_string(uri + uriOffset, strlen(uri + uriOffset)));
    if (index != -1) {
        bx::MultiHashTable<uint16_t>::Node* node = this->hotLoadsTable.getNode(index);
        // Recurse resources and reload them with their params
        while (node) {
            const Resource* rs = this->resources.getHandleData<Resource>(0, node->value);
            loadResourceHashed(rs->typeNameHash, rs->uri.cstr(), rs->userParams, ResourceFlag::Reload, rs->objAlloc);
            node = node->next;
        }
    }

    // Run user callback
    if (this->modifiedCallback)
        this->modifiedCallback(uri, this->fileModifiedUserParam);
}
