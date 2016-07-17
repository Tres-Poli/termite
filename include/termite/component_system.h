#pragma once

#include "types.h"
#include "bx/bx.h"
#include "bx/allocator.h"
#include "bxx/bitmask_operators.hpp"

namespace termite
{
    static const uint32_t kEntityIndexBits = 22;
    static const uint32_t kEntityIndexMask = (1 << kEntityIndexBits) - 1;
    static const uint32_t kEntityGenerationBits = 8;
    static const uint32_t kEntityGenerationMask = (1 << kEntityGenerationBits) - 1;

    struct Entity
    {
        uint32_t id;

        inline Entity()
        {
            id = 0;
        }

        explicit Entity(uint32_t _id) : id(_id)
        {
        }

        inline Entity(uint32_t index, uint32_t generation)
        {
            id = (index & kEntityIndexMask) | ((generation & kEntityGenerationMask) << kEntityIndexBits);
        }

        inline uint32_t getIndex()
        {
            return (id & kEntityIndexMask);
        }

        inline uint32_t getGeneration()
        {
            return (id >> kEntityIndexBits) & kEntityGenerationMask;
        }
    };

    struct EntityManager;
    struct ComponentTypeT {};
    struct ComponentT {};
    typedef PhantomType<uint16_t, ComponentTypeT, UINT16_MAX> ComponentTypeHandle;
    typedef PhantomType<uint32_t, ComponentT, UINT32_MAX> ComponentHandle;

    // Entity Management
    TERMITE_API EntityManager* createEntityManager(bx::AllocatorI* alloc, int bufferSize = 0);
    TERMITE_API void destroyEntityManager(EntityManager* emgr);

	TERMITE_API Entity createEntity(EntityManager* emgr);
	TERMITE_API void destroyEntity(EntityManager* emgr, Entity ent);
	TERMITE_API bool isEntityAlive(EntityManager* emgr, Entity ent);

    // Component System
    result_t initComponentSystem(bx::AllocatorI* alloc);
    void shutdownComponentSystem();
    
    struct ComponentCallbacks
    {
        bool(*createInstance)(Entity ent, ComponentHandle handle);
        void(*destroyInstance)(Entity ent, ComponentHandle handle);
        void(*updateAll)();
        void(*preUpdateAll)();
        void(*postUpdateAll)();
    };

    enum class ComponentFlag
    {
        None = 0x0,
        ImmediateDestroy = 0x01   // Destroys component immediately after owner entity is destroyed
    };

	TERMITE_API ComponentTypeHandle registerComponentType(const char* name, uint32_t id,
							                              const ComponentCallbacks* callbacks, ComponentFlag flags,
										                  uint32_t dataSize, uint16_t poolSize, uint16_t growSize);
	TERMITE_API void garbageCollectComponents(EntityManager* emgr);

	TERMITE_API ComponentHandle createComponent(EntityManager* emgr, Entity ent, ComponentTypeHandle handle);
	TERMITE_API void destroyComponent(EntityManager* emgr, Entity ent, ComponentHandle handle);

	TERMITE_API ComponentTypeHandle findComponentTypeByName(const char* name);
	TERMITE_API ComponentTypeHandle findComponentTypeById(uint32_t id);
	TERMITE_API ComponentHandle getComponent(ComponentTypeHandle handle, Entity ent);
	TERMITE_API void* getComponentData(ComponentHandle handle);
    TERMITE_API Entity getComponentEntity(ComponentHandle handle);
	TERMITE_API uint16_t getAllComponents(ComponentTypeHandle typeHandle, ComponentHandle* handles, uint16_t maxComponents);
	

    template <typename Ty> 
    Ty* getComponentData(ComponentHandle handle)
    {
        return (Ty*)getComponentData(handle);
    }
    
} // namespace termite

C11_DEFINE_FLAG_TYPE(termite::ComponentFlag);