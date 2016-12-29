#include "pch.h"
#include "component_system.h"

#include "bx/uint32_t.h"
#include "bxx/array.h"
#include "bxx/pool.h"
#include "bxx/queue.h"
#include "bxx/handle_pool.h"
#include "bxx/hash_table.h"
#include "bxx/logger.h"
#include "bxx/linear_allocator.h"

#define MIN_FREE_INDICES 1024

static const uint32_t kComponentHandleBits = 16;
static const uint32_t kComponentHandleMask = (1 << kComponentHandleBits) - 1;
static const uint32_t kComponentTypeHandleBits = 16;
static const uint32_t kComponentTypeHandleMask = (1 << kComponentTypeHandleBits) - 1;

#define COMPONENT_INSTANCE_HANDLE(_Handle) uint16_t(_Handle.value & kComponentHandleMask)
#define COMPONENT_TYPE_INDEX(_Handle) uint16_t((_Handle.value >> kComponentHandleBits) & kComponentTypeHandleMask)
#define COMPONENT_MAKE_HANDLE(_CTypeIdx, _CHdl) ComponentHandle((uint32_t(_CTypeIdx) << kComponentTypeHandleBits) | uint32_t(_CHdl))

using namespace termite;

namespace termite
{
    typedef bx::MultiHashTable<int, uint32_t> DestroyHashTable;

    struct EntityManager
    {
        struct FreeIndex
        {
            typedef bx::Queue<FreeIndex*>::Node QNode;

            uint32_t index;
            QNode qnode;

            explicit FreeIndex(uint32_t _index) :
                index(_index),
                qnode(this)
            {
            }
        };

        bx::AllocatorI* alloc;
        bx::Pool<FreeIndex> freeIndexPool;
        bx::Queue<FreeIndex*> freeIndexQueue;
        uint32_t freeIndexSize;
        bx::Array<uint16_t> generations;
        DestroyHashTable destroyTable; // keep a multi-hash for all components that entity has to destroy
		bx::Pool<DestroyHashTable::Node> nodePool;
        
        EntityManager(bx::AllocatorI* _alloc) : 
            alloc(_alloc),
            freeIndexSize(0),
            destroyTable(bx::HashTableType::Mutable)
        {
        }
    };
}

struct ComponentType
{
    ComponentTypeHandle myHandle;
    char name[32];
    ComponentCallbacks callbacks;
    ComponentFlag::Bits flags;
    uint32_t dataSize;
    bx::HandlePool dataPool;
    bx::HashTable<ComponentHandle, uint32_t> entTable;  // Entity -> ComponentHandle

    ComponentType() : 
        entTable(bx::HashTableType::Mutable)
    {
        strcpy(name, "");
        memset(&callbacks, 0x00, sizeof(callbacks));
        flags = ComponentFlag::None;
        dataSize = 0;
    }
};

struct ComponentGroup
{
    struct Batch
    {
        int index;
        int count;
    };

    bx::Array<ComponentHandle> components;
    bx::Array<Batch> batches;
    bool sorted;

    ComponentGroup() :
        sorted(false)
    {
    }
};

struct ComponentSystem
{
    bx::AllocatorI* alloc;
    bx::Array<ComponentType> components;
    bx::HashTableInt nameTable;
    bx::HandlePool componentGroups;

    ComponentSystem(bx::AllocatorI* _alloc) : 
        alloc(_alloc),
        nameTable(bx::HashTableType::Mutable)
    {
    }
};

static ComponentSystem* g_csys = nullptr;

EntityManager* termite::createEntityManager(bx::AllocatorI* alloc, int bufferSize)
{
    EntityManager* emgr = BX_NEW(alloc, EntityManager)(alloc);
    if (!emgr)
        return nullptr;
    if (bufferSize <= 0)
        bufferSize = MIN_FREE_INDICES;
    if (!emgr->generations.create(bufferSize, bufferSize, alloc) ||
        !emgr->freeIndexPool.create(bufferSize, alloc) ||
		!emgr->nodePool.create(bufferSize, alloc) ||
        !emgr->destroyTable.create(bufferSize, alloc, &emgr->nodePool))
    {
        destroyEntityManager(emgr);
        return nullptr;
    }

    return emgr;
}

void termite::destroyEntityManager(EntityManager* emgr)
{
    assert(emgr);

    emgr->freeIndexPool.destroy();
	emgr->nodePool.destroy();
    emgr->generations.destroy();
    emgr->destroyTable.destroy();

    BX_DELETE(emgr->alloc, emgr);
}

Entity termite::createEntity(EntityManager* emgr)
{
    uint32_t idx;
    if (emgr->freeIndexSize > MIN_FREE_INDICES) {
        EntityManager::FreeIndex* fidx;
        emgr->freeIndexQueue.pop(&fidx);
        idx = fidx->index;
        emgr->freeIndexSize--;
    } else {
        idx = emgr->generations.getCount();
        uint16_t* gen = emgr->generations.push();
        *gen = 1;
        assert(idx < (1 << kEntityIndexBits));
    }
    return Entity(idx, emgr->generations[idx]);
}

static void addToComponentGroup(ComponentGroupHandle handle, ComponentHandle component)
{
    ComponentGroup* group = g_csys->componentGroups.getHandleData<ComponentGroup>(0, handle);
    ComponentHandle* pchandle = group->components.push();
    if (pchandle) {
        *pchandle = component;
        group->sorted = false;
    }
}

static void removeFromComponentGroup(ComponentGroupHandle handle, ComponentHandle component)
{
    assert(component.isValid());
    assert(handle.isValid());

    ComponentGroup* group = g_csys->componentGroups.getHandleData<ComponentGroup>(0, handle);

    int count = group->components.getCount();
    // Find the component and Swap current with the last group
    int index = group->components.find(component);
    if (index != -1) {
        ComponentHandle* buff = group->components.getBuffer();
        if (index != count - 1) {
            std::swap<ComponentHandle>(buff[index], buff[count-1]);
            group->sorted = false;
        }
        group->components.pop();
    }
}

static void destroyComponentNoImmDestroy(Entity ent, ComponentHandle handle)
{
    assert(handle.isValid());

    ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(handle)];
    uint16_t instHandle = COMPONENT_INSTANCE_HANDLE(handle);

    // Remove from component group
    ComponentGroupHandle groupHandle = *ctype.dataPool.getHandleData<ComponentGroupHandle>(2, instHandle);
    if (groupHandle.isValid())
        removeFromComponentGroup(groupHandle, handle);

    // Call destroy callback
    if (ctype.callbacks.destroyInstance)
        ctype.callbacks.destroyInstance(ent, handle, ctype.dataPool.getHandleData(1, instHandle));

    ctype.dataPool.freeHandle(instHandle);

    int r = ctype.entTable.find(ent.id);
    if (r != -1)
        ctype.entTable.remove(r);
}

void termite::destroyEntity(EntityManager* emgr, Entity ent)
{
    assert(isEntityAlive(emgr, ent));

    // Check if the entity has immediate destroy components
    // And destroy all components registered to entity
    int entIdx = emgr->destroyTable.find(ent.id);
    if (entIdx != -1) {
        DestroyHashTable::Node* node = emgr->destroyTable.getNode(entIdx);
        while (node) {
            DestroyHashTable::Node* next = node->next;
            ComponentHandle component(uint32_t(node->value));
            destroyComponentNoImmDestroy(ent, component);

            emgr->destroyTable.remove(entIdx, node);
            node = next;
        }
    }

    uint32_t idx = ent.getIndex();
    ++emgr->generations[idx];
    
    EntityManager::FreeIndex* fi = emgr->freeIndexPool.newInstance<int>(idx);
    if (fi) {
        emgr->freeIndexQueue.push(&fi->qnode);
        emgr->freeIndexSize++;
    }
}

bool termite::isEntityAlive(EntityManager* emgr, Entity ent)
{
    return emgr->generations[ent.getIndex()] == ent.getGeneration();
}

void termite::setEntityActive(Entity ent, bool active)
{
    const int maxHandles = 100;
    ComponentHandle* handles = (ComponentHandle*)alloca(maxHandles*sizeof(ComponentHandle));
    int numHandles = getEntityComponents(ent, handles, maxHandles);
    for (int i = 0; i < numHandles; i++) {
        ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(handles[i])];

        uint16_t cHandle = COMPONENT_INSTANCE_HANDLE(handles[i]);
        bool prevActive = *ctype.dataPool.getHandleData<bool>(3, cHandle);
        if (prevActive != active) {
            *ctype.dataPool.getHandleData<bool>(3, cHandle) = active;

            ComponentGroupHandle groupHandle = *ctype.dataPool.getHandleData<bool>(3, cHandle);
            if (groupHandle.isValid()) {
                if (active)
                    addToComponentGroup(groupHandle, handles[i]);
                else
                    removeFromComponentGroup(groupHandle, handles[i]);
            }
        }
    }
}

result_t termite::initComponentSystem(bx::AllocatorI* alloc)
{
    if (g_csys) {
        assert(false);
        return T_ERR_ALREADY_INITIALIZED;
    }

    g_csys = BX_NEW(alloc, ComponentSystem)(alloc);
    if (!g_csys)
        return T_ERR_OUTOFMEM;

    uint32_t cgSz = sizeof(ComponentGroup);
    if (!g_csys->components.create(32, 128, alloc) || 
        !g_csys->nameTable.create(128, alloc) ||
        !g_csys->componentGroups.create(&cgSz, 1, 32, 32, alloc))
    {
        return T_ERR_OUTOFMEM;
    }

    return 0;
}

void termite::shutdownComponentSystem()
{
    if (!g_csys)
        return;

    for (int i = 0; i < g_csys->components.getCount(); i++) {
        ComponentType& ctype = g_csys->components[i];
        ctype.dataPool.destroy();
        ctype.entTable.destroy();
    }
    g_csys->componentGroups.destroy();
    g_csys->components.destroy();
    g_csys->nameTable.destroy();

    BX_DELETE(g_csys->alloc, g_csys);
}

ComponentGroupHandle termite::createComponentGroup(bx::AllocatorI* alloc, uint16_t poolSize /*= 0*/)
{
    ComponentGroupHandle handle = ComponentGroupHandle(g_csys->componentGroups.newHandle());
    if (handle.isValid()) {
        ComponentGroup* group = new(g_csys->componentGroups.getHandleData(0, handle)) ComponentGroup();
        if (!group->components.create(poolSize, poolSize, alloc) ||
            !group->batches.create(32, 64, alloc)) 
        {
            destroyComponentGroup(handle);
            return ComponentGroupHandle();
        }
    }
    return handle;
}

void termite::destroyComponentGroup(ComponentGroupHandle handle)
{
    assert(handle.isValid());
    ComponentGroup* group = g_csys->componentGroups.getHandleData<ComponentGroup>(0, handle);

    // Unlink all component references
    // It is recommended that you Call this function before destroying components/entities 
    for (int i = 0; i < group->components.getCount(); i++) {
        ComponentHandle chandle = group->components[i];
        ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(chandle)];
        *ctype.dataPool.getHandleData<ComponentGroupHandle>(2, COMPONENT_INSTANCE_HANDLE(chandle)) = ComponentGroupHandle();
    }

    group->batches.destroy();
    group->components.destroy();
    g_csys->componentGroups.freeHandle(handle);
}

ComponentTypeHandle termite::registerComponentType(const char* name, const ComponentCallbacks* callbacks, 
                                                   ComponentFlag::Bits flags, uint32_t dataSize, uint16_t poolSize, 
                                                   uint16_t growSize, bx::AllocatorI* alloc)
{
    assert(g_csys);
    assert(g_csys->components.getCount() < UINT16_MAX);

    ComponentType* buff = g_csys->components.push();
    if (!buff)
        return ComponentTypeHandle();

    ComponentType* ctype = new(buff) ComponentType();

    bx::strlcpy(ctype->name, name, sizeof(ctype->name));
    if (callbacks)
        memcpy(&ctype->callbacks, callbacks, sizeof(ComponentCallbacks));
    ctype->flags = flags;
    ctype->dataSize = dataSize;
    const uint32_t itemSizes[4] = {sizeof(Entity), dataSize, sizeof(ComponentGroupHandle), sizeof(bool)};
    if (!ctype->dataPool.create(itemSizes, BX_COUNTOF(itemSizes), poolSize, growSize, alloc ? alloc : g_csys->alloc) ||
        !ctype->entTable.create(poolSize, alloc ? alloc : g_csys->alloc)) 
    {
        return ComponentTypeHandle();
    }

    // Add to ComponentType database
    int index = g_csys->components.getCount() - 1;
    g_csys->nameTable.add(tinystl::hash_string(name, strlen(name)), index);

    ComponentTypeHandle handle = ComponentTypeHandle(uint16_t(index));
    ctype->myHandle = handle;
    return handle;
}

void termite::garbageCollectComponents(EntityManager* emgr)
{
    // For each component perform garbage collection
    for (int i = 0, c = g_csys->components.getCount(); i < c; i++) {
        ComponentType& ctype = g_csys->components[i];

        if ((ctype.flags & ComponentFlag::ImmediateDestroy) == 0) {
            int aliveInRow = 0;
            while (ctype.dataPool.getCount() && aliveInRow < 4) {
                uint16_t r = ctype.dataPool.handleAt((uint16_t)getRandomIntUniform(0, (int)ctype.dataPool.getCount() - 1));
                Entity ent = *ctype.dataPool.getHandleData<Entity>(0, r);
                if (isEntityAlive(emgr, ent)) {
                    aliveInRow++;
                    continue;
                }

                aliveInRow = 0;
                destroyComponent(emgr, ent, COMPONENT_MAKE_HANDLE(i, r));
            }
        }
    }
}

ComponentHandle termite::createComponent(EntityManager* emgr, Entity ent, ComponentTypeHandle handle, 
                                         ComponentGroupHandle group)
{
    ComponentType& ctype = g_csys->components[handle.value];

    if (ctype.entTable.find(ent.id) != -1) {
        assert(false);  // Component instance Already exists for the entity
        return ComponentHandle();
    }

    uint16_t cIdx = ctype.dataPool.newHandle();
    if (cIdx == UINT16_MAX)
        return ComponentHandle();
    *ctype.dataPool.getHandleData<Entity>(0, cIdx) = ent;
    void* data = ctype.dataPool.getHandleData(1, cIdx);
    *ctype.dataPool.getHandleData<ComponentGroupHandle>(2, cIdx) = group;
    *ctype.dataPool.getHandleData<bool>(3, cIdx) = true;

    ComponentHandle chandle = COMPONENT_MAKE_HANDLE(handle.value, cIdx);

    if (group.isValid())
        addToComponentGroup(group, chandle);

    ctype.entTable.add(ent.id, chandle);

    if (ctype.flags & ComponentFlag::ImmediateDestroy) {
        emgr->destroyTable.add(ent.id, int(chandle.value));
    }

    // Call create callback
    if (ctype.callbacks.createInstance) {
        ctype.callbacks.createInstance(ent, chandle, data);
    }

    return chandle;
}

void termite::destroyComponent(EntityManager* emgr, Entity ent, ComponentHandle handle)
{
    destroyComponentNoImmDestroy(ent, handle);
    
    ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(handle)];

    if (ctype.flags & ComponentFlag::ImmediateDestroy) {
        int r = emgr->destroyTable.find(ent.id);
        if (r != -1) {
            DestroyHashTable::Node* node = emgr->destroyTable.getNode(r);
            while (node) {
                if (node->value == int(handle.value)) {
                    emgr->destroyTable.remove(r, node);
                    break;
                }
                node = node->next;
            }
        }
    }
}

static void sortAndBatchComponents(ComponentGroup* group)
{
    // Sort components if it's invalidated
    int count = group->components.getCount();
    if (count > 1 && !group->sorted) {
        std::sort(group->components.itemPtr(0), group->components.itemPtr(count - 1),
                  [](const ComponentHandle& a, const ComponentHandle& b) { return a.value < b.value; });

        // Batch by component-type
        group->batches.clear();
        ComponentTypeHandle prevHandle;
        ComponentGroup::Batch* curBatch = nullptr;

        for (int i = 0; i < count; i++) {
            ComponentTypeHandle curHandle = ComponentTypeHandle(COMPONENT_TYPE_INDEX(group->components[i]));
            if (curHandle != prevHandle) {
                curBatch = group->batches.push();
                curBatch->index = i;
                curBatch->count = 0;
                prevHandle = curHandle;
            }
            curBatch->count++;
        }

        group->sorted = true;
    }
}

void termite::runComponentGroup(ComponentStage::Enum stage, ComponentGroupHandle groupHandle, float dt)
{
    assert(groupHandle.isValid());
    ComponentGroup* group = g_csys->componentGroups.getHandleData<ComponentGroup>(0, groupHandle);
    sortAndBatchComponents(group);

    // Call their callbacks
    for (int i = 0, c = group->batches.getCount(); i < c; i++) {
        ComponentGroup::Batch batch = group->batches[i];
        const ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(group->components[batch.index])];
        if (ctype.callbacks.stageFn[stage])
            ctype.callbacks.stageFn[stage](group->components.itemPtr(batch.index), batch.count, dt);
    }
}

ComponentTypeHandle termite::findComponentTypeByName(const char* name)
{
    int index = g_csys->nameTable.find(tinystl::hash_string(name, strlen(name)));
    if (index != -1)
        return ComponentTypeHandle(uint16_t(g_csys->nameTable[index]));
    else
        return ComponentTypeHandle();
}

ComponentTypeHandle termite::findComponentTypeByNameHash(size_t nameHash)
{
    int index = g_csys->nameTable.find(nameHash);
    if (index != -1)
        return ComponentTypeHandle(uint16_t(g_csys->nameTable[index]));
    else
        return ComponentTypeHandle();
}

ComponentHandle termite::getComponent(ComponentTypeHandle handle, Entity ent)
{
    assert(handle.isValid());

    const ComponentType& ctype = g_csys->components[handle.value];
    int r = ctype.entTable.find(ent.id);
    if (r != -1)
        return ComponentHandle(ctype.entTable[r]);
    else
        return ComponentHandle();
}

const char* termite::getComponentName(ComponentHandle handle)
{
    assert(handle.isValid());

    ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(handle)];
    return ctype.name;
}

void* termite::getComponentData(ComponentHandle handle)
{
    assert(handle.isValid());

    ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(handle)];
    return ctype.dataPool.getHandleData(1, COMPONENT_INSTANCE_HANDLE(handle));    
}

Entity termite::getComponentEntity(ComponentHandle handle)
{
    assert(handle.isValid());

    ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(handle)];
    return *ctype.dataPool.getHandleData<Entity>(0, COMPONENT_INSTANCE_HANDLE(handle));
}

ComponentGroupHandle termite::getComponentGroup(ComponentHandle handle)
{
    assert(handle.isValid());

    ComponentType& ctype = g_csys->components[COMPONENT_TYPE_INDEX(handle)];
    return *ctype.dataPool.getHandleData<ComponentGroupHandle>(2, COMPONENT_INSTANCE_HANDLE(handle));
}

uint16_t termite::getAllComponents(ComponentTypeHandle typeHandle, ComponentHandle* handles, uint16_t maxComponents)
{
	assert(typeHandle.isValid());

    const ComponentType& ctype = g_csys->components[typeHandle.value];
    uint16_t count = ctype.dataPool.getCount();
    if (handles == nullptr)
        return count;

	count = bx::uint32_min(count, maxComponents);
	for (uint16_t i = 0; i < count; i++) {
		handles[i] = COMPONENT_MAKE_HANDLE(typeHandle.value, ctype.dataPool.handleAt(i));
	}

	return count;
}

uint16_t termite::getEntityComponents(Entity ent, ComponentHandle* handles, uint16_t maxComponents)
{
    int index = 0;
    for (int i = 0, c = g_csys->components.getCount(); i < c; i++) {
        const ComponentType& ctype = g_csys->components[i];
        int r = ctype.entTable.find(ent.id);
        if (r == -1)
            continue;

        if (index == maxComponents)
            return maxComponents;

        if (handles)
            handles[index] = ctype.entTable[r];
        index ++;
    }

    return index;
}

uint16_t termite::getGroupComponents(ComponentGroupHandle groupHandle, ComponentHandle* handles, uint16_t maxComponents)
{
    assert(groupHandle.isValid());
    ComponentGroup* group = g_csys->componentGroups.getHandleData<ComponentGroup>(0, groupHandle);
    uint16_t count = std::min<uint16_t>(maxComponents, (uint16_t)group->components.getCount());

    if (handles)
        memcpy(handles, group->components.itemPtr(0), count*sizeof(ComponentHandle));
    return count;
}

uint16_t termite::getGroupComponentsByType(ComponentGroupHandle groupHandle, ComponentHandle* handles, uint16_t maxComponents, 
                                           ComponentTypeHandle typeHandle)
{
    assert(groupHandle.isValid());
    ComponentGroup* group = g_csys->componentGroups.getHandleData<ComponentGroup>(0, groupHandle);

    sortAndBatchComponents(group);

    for (int i = 0, c = group->batches.getCount(); i < c; i++) {
        const ComponentGroup::Batch& batch = group->batches[i];
        if (typeHandle == ComponentTypeHandle(COMPONENT_TYPE_INDEX(group->components[i]))) {
            uint16_t count = std::min<uint16_t>(maxComponents, (uint16_t)batch.count);
            if (handles) {
                memcpy(handles, group->components.itemPtr(batch.index), count*sizeof(ComponentHandle));
            }
            return count;
        }
    }

    return 0;
}

