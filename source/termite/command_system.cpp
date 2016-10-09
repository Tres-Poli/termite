#include "pch.h"
#include "command_system.h"

#include "bxx/indexed_pool.h"
#include "bxx/array.h"
#include "bxx/hash_table.h"
#include "bxx/stack.h"

using namespace termite;

static const uint32_t kCommandIndexBits = 16;
static const uint32_t kCommandIndexMask = (1 << kCommandIndexBits) - 1;
static const uint32_t kCommandTypeHandleBits = 16;
static const uint32_t kCommandTypeHandleMask = (1 << kCommandTypeHandleBits) - 1;

#define COMMAND_INSTANCE_INDEX(_Handle) uint16_t(_Handle.value & kCommandIndexBits)
#define COMMAND_TYPE_INDEX(_Handle) uint16_t((_Handle.value >> kCommandIndexBits) & kCommandTypeHandleMask)
#define COMMAND_MAKE_HANDLE(_CTypeIdx, _CIdx) CommandHandle((uint32_t(_CTypeIdx) << kCommandTypeHandleBits) | uint32_t(_CIdx))

struct CommandMode
{
    enum Enum
    {
        Normal = 0,
        Chain,
        Group
    };
};

struct CommandState
{
    enum Enum
    {
        None = 0,
        Execute,
        Undo
    };
};

struct CommandType
{
    char name[32];
    ExecuteCommandFn executeFn;
    UndoCommandFn undoFn;
    CleanupCommandFn cleanupFn;
    size_t paramSize;
    bx::IndexedPool paramPool;

    CommandType()
    {
        strcpy(name, "");
        executeFn = nullptr;
        undoFn = nullptr;
        cleanupFn = nullptr;
        paramSize = 0;
    }
};

struct Command
{
    CommandHandle nextHandle;
    CommandHandle prevHandle;
    CommandHandle childHandle;
    uint16_t paramIndex;
    CommandMode::Enum mode;
    CommandState::Enum state;
    void* userData;

    Command()
    {
        paramIndex = UINT16_MAX;
        mode = CommandMode::Normal;
        state = CommandState::None;
        userData = nullptr;
    }
};

struct CommandSystem
{
    bx::Array<CommandType> commandTypes;
    bx::IndexedPool commandPool;
    uint16_t maxSize;
    bx::AllocatorI* alloc;
    bx::HashTableInt commandTypeTable;
    CommandHandle lastCommand;
    CommandHandle firstCommand;
    CommandHandle curChain;
    uint16_t numCommands;       // Number of commands in the main list

    CommandSystem(bx::AllocatorI* _alloc) :
        alloc(_alloc),
        commandTypeTable(bx::HashTableType::Mutable)
    {
        maxSize = 0;
    }
};

static CommandSystem* g_cmdSys = nullptr;

result_t termite::initCommandSystem(uint16_t historySize, bx::AllocatorI* alloc)
{
    assert(historySize);

    if (g_cmdSys) {
        assert(false);
        return T_ERR_ALREADY_INITIALIZED;
    }

    g_cmdSys = BX_NEW(alloc, CommandSystem)(alloc);
    if (!g_cmdSys)
        return T_ERR_OUTOFMEM;

    g_cmdSys->maxSize = historySize;

    const uint32_t itemSizes[] = {
        sizeof(Command)
    };
    
    if (!g_cmdSys->commandTypes.create(128, 256, alloc) ||
        !g_cmdSys->commandTypeTable.create(128, alloc) ||
        !g_cmdSys->commandPool.create(itemSizes, 1, historySize, historySize, alloc))
    {
        return T_ERR_OUTOFMEM;
    }

    return 0;
}

inline Command* getCommand(CommandHandle handle)
{
    return g_cmdSys->commandPool.getHandleData<Command>(0, COMMAND_INSTANCE_INDEX(handle));
}

static void removeCommand(CommandHandle handle)
{
    assert(handle.isValid());

    // Remove children
    Command* cmd = getCommand(handle);
    if (cmd->childHandle.isValid()) {
        CommandHandle childHandle = cmd->childHandle;
        while (childHandle.isValid()) {
            Command* child = getCommand(childHandle);
            CommandType& ctype = g_cmdSys->commandTypes[COMMAND_TYPE_INDEX(childHandle)];
            if (ctype.cleanupFn)
                ctype.cleanupFn(cmd->userData);

            g_cmdSys->commandPool.freeHandle(COMMAND_INSTANCE_INDEX(childHandle));
            childHandle = child->nextHandle;
        }
    }

    uint16_t typeIdx = COMMAND_TYPE_INDEX(handle);
    if (typeIdx != UINT16_MAX) {
        CommandType& ctype = g_cmdSys->commandTypes[typeIdx];
        if (ctype.cleanupFn)
            ctype.cleanupFn(cmd->userData);
    }
    g_cmdSys->commandPool.freeHandle(COMMAND_INSTANCE_INDEX(handle));
}

void termite::shutdownCommandSystem()
{
    if (!g_cmdSys)
        return;

    // Cleanup command list
    CommandHandle cmdHandle = g_cmdSys->lastCommand;
    while (cmdHandle.isValid()) {
        removeCommand(cmdHandle);
        cmdHandle = getCommand(cmdHandle)->prevHandle;
    }

    // Destroy all registered command types
    for (int i = 0; i < g_cmdSys->commandTypes.getCount(); i++) {
        CommandType* ctype = g_cmdSys->commandTypes.itemPtr(i);
        ctype->paramPool.destroy();
    }

    g_cmdSys->commandTypeTable.destroy();
    g_cmdSys->commandPool.destroy();
    g_cmdSys->commandTypes.destroy();

    BX_DELETE(g_cmdSys->alloc, g_cmdSys);
    g_cmdSys = nullptr;
}

CommandTypeHandle termite::registerCommand(const char* name, ExecuteCommandFn executeFn, UndoCommandFn undoFn, 
                                           CleanupCommandFn cleanupFn, size_t paramSize)
{
    assert(g_cmdSys);

    CommandType* ptr = g_cmdSys->commandTypes.push();
    if (!ptr)
        return CommandTypeHandle();

    CommandType* ctype = new(ptr) CommandType();

    bx::strlcpy(ctype->name, name, sizeof(ctype->name));
    assert(executeFn);
    ctype->executeFn = executeFn;
    ctype->undoFn = undoFn;
    ctype->cleanupFn = cleanupFn;
    
    ctype->paramSize = paramSize;

    if (paramSize > 0) {
        const uint32_t itemSizes[] = {
            uint32_t(paramSize),
            uint32_t(paramSize)
        };
        if (!ctype->paramPool.create(itemSizes, 2, 32, 128, g_cmdSys->alloc))
            return CommandTypeHandle();
    }

    return CommandTypeHandle(uint16_t(g_cmdSys->commandPool.getCount() - 1));
}

CommandTypeHandle termite::findCommand(const char* name)
{
    assert(g_cmdSys);

    int r = g_cmdSys->commandTypeTable.find(bx::hashMurmur2A(name, (uint32_t)strlen(name)));
    if (r != -1) {
        int index = g_cmdSys->commandTypeTable.getValue(r);
        return CommandTypeHandle(uint16_t(index));
    }

    return CommandTypeHandle();
}

static CommandHandle popFromMainList()
{
    assert(g_cmdSys->firstCommand.isValid());

    CommandHandle handle = g_cmdSys->firstCommand;
    CommandHandle nextHandle = getCommand(handle)->nextHandle;
    if (nextHandle.isValid()) {
        getCommand(nextHandle)->prevHandle.reset();
        g_cmdSys->firstCommand = nextHandle;
    }
    g_cmdSys->numCommands--;

    return handle;
}

static void pushToMainList(CommandHandle handle)
{
    assert(handle.isValid());

    if (g_cmdSys->lastCommand.isValid()) {
        getCommand(g_cmdSys->lastCommand)->nextHandle = handle;
        getCommand(handle)->prevHandle = g_cmdSys->lastCommand;
    }
    if (!g_cmdSys->firstCommand.isValid())
        g_cmdSys->firstCommand = handle;

    g_cmdSys->lastCommand = handle;

    g_cmdSys->numCommands++;
}

static void addToParent(CommandHandle handle, CommandHandle parentHandle)
{
    Command* cmd = getCommand(handle);
    Command* parent = getCommand(parentHandle);

    if (parent->childHandle.isValid()) {
        // Go to the end of the list and add the handle
        CommandHandle lastHandle = parent->childHandle;
        while (getCommand(lastHandle)->nextHandle.isValid())
            lastHandle = getCommand(lastHandle)->nextHandle;
        Command* last = getCommand(lastHandle);
        last->nextHandle = handle;
        getCommand(handle)->prevHandle = lastHandle;
    } else {
        parent->childHandle = handle;
    }
}

CommandHandle termite::addCommand(CommandTypeHandle handle, const void* param, const void* undoParam)
{
    assert(handle.isValid());

    CommandType& ctype = g_cmdSys->commandTypes[handle.value];
    uint16_t cIndex = g_cmdSys->commandPool.newHandle();
    Command* cmd = new(g_cmdSys->commandPool.getHandleData(0, cIndex)) Command();

    if (ctype.paramSize > 0) {
        cmd->paramIndex = ctype.paramPool.newHandle();
        memcpy(ctype.paramPool.getHandleData(0, cmd->paramIndex), param, ctype.paramSize);
        memcpy(ctype.paramPool.getHandleData(1, cmd->paramIndex), undoParam, ctype.paramSize);
    }

    CommandHandle cmdHandle = COMMAND_MAKE_HANDLE(handle.value, cIndex);
    pushToMainList(cmdHandle);

    // Remove from the first item
    if (g_cmdSys->numCommands > g_cmdSys->maxSize) {
        removeCommand(popFromMainList());
    }
        
    return cmdHandle;
}

CommandHandle termite::addCommandGroup(CommandTypeHandle handle, int numCommands, 
                                       const void* const* params, const void* const* undoParams)
{
    assert(numCommands > 0);
    assert(handle.isValid());

    // Create dummy command for group
    uint16_t cIndex = g_cmdSys->commandPool.newHandle();
    Command* groupCmd = new(g_cmdSys->commandPool.getHandleData(0, cIndex)) Command();
    groupCmd->mode = CommandMode::Group;
    CommandHandle groupCmdHandle = COMMAND_MAKE_HANDLE(UINT16_MAX, cIndex);

    for (int i = 0; i < numCommands; i++) {
        CommandType& ctype = g_cmdSys->commandTypes[handle.value];
        cIndex = g_cmdSys->commandPool.newHandle();
        Command* cmd = new(g_cmdSys->commandPool.getHandleData(0, cIndex)) Command();

        if (ctype.paramSize > 0) {
            cmd->paramIndex = ctype.paramPool.newHandle();
            memcpy(ctype.paramPool.getHandleData(0, cmd->paramIndex), params[i], ctype.paramSize);
            memcpy(ctype.paramPool.getHandleData(1, cmd->paramIndex), undoParams[i], ctype.paramSize);
        }

        addToParent(COMMAND_MAKE_HANDLE(handle.value, cIndex), groupCmdHandle);
    }

    return groupCmdHandle;
}

void termite::beginCommandChain()
{
    assert(!g_cmdSys->curChain.isValid());

    // Create dummy command for chain
    uint16_t cIndex = g_cmdSys->commandPool.newHandle();
    Command* cmd = new(g_cmdSys->commandPool.getHandleData(0, cIndex)) Command();
    cmd->mode = CommandMode::Chain;
    g_cmdSys->curChain = COMMAND_MAKE_HANDLE(UINT16_MAX, cIndex);
}

void termite::addCommandChain(CommandTypeHandle handle, const void* param, const void* undoParam)
{
    if (!g_cmdSys->curChain.isValid()) {
        assert(false);  // beginCommandChain is not called
        return;
    }

    CommandType& ctype = g_cmdSys->commandTypes[handle.value];
    uint16_t cIndex = g_cmdSys->commandPool.newHandle();
    Command* cmd = new(g_cmdSys->commandPool.getHandleData(0, cIndex)) Command();

    if (ctype.paramSize > 0) {
        cmd->paramIndex = ctype.paramPool.newHandle();
        memcpy(ctype.paramPool.getHandleData(0, cmd->paramIndex), param, ctype.paramSize);
        memcpy(ctype.paramPool.getHandleData(1, cmd->paramIndex), undoParam, ctype.paramSize);
    }

    CommandHandle cmdHandle = COMMAND_MAKE_HANDLE(handle.value, cIndex);
    addToParent(cmdHandle, g_cmdSys->curChain);
}

CommandHandle termite::endCommandChain()
{
    assert(g_cmdSys->curChain.isValid());

    CommandHandle handle = g_cmdSys->curChain;
    Command* chain = getCommand(handle);
    if (chain->childHandle.isValid()) {
        pushToMainList(g_cmdSys->curChain);
        g_cmdSys->curChain.reset();

        if (g_cmdSys->numCommands > g_cmdSys->maxSize) {
            removeCommand(popFromMainList());
        }
    }
    return handle;
}

void termite::executeCommand(CommandHandle handle)
{
    assert(handle.isValid());

    Command* cmd = getCommand(handle);

    if (cmd->state == CommandState::Execute)
        return;

    // Run all commands that are not executed before this one (from first to current)
    if (cmd->prevHandle.isValid()) {
        if (getCommand(cmd->prevHandle)->state != CommandState::Execute)
            executeCommand(cmd->prevHandle);
    }

    // Run current command
    switch (cmd->mode) {
    case CommandMode::Normal:
    {
        CommandType& ctype = g_cmdSys->commandTypes[COMMAND_TYPE_INDEX(handle)];
        ctype.executeFn(cmd->paramIndex != UINT16_MAX ? ctype.paramPool.getHandleData(0, cmd->paramIndex) : nullptr);
        break;
    }
    case CommandMode::Chain:
    case CommandMode::Group:
    {
        CommandHandle childHandle = cmd->childHandle;
        while (childHandle) {
            Command* child = getCommand(childHandle);
            CommandType& ctype = g_cmdSys->commandTypes[COMMAND_TYPE_INDEX(childHandle)];
            ctype.executeFn(cmd->paramIndex != UINT16_MAX ? ctype.paramPool.getHandleData(0, cmd->paramIndex) : nullptr);
            childHandle = child->nextHandle;
        }
        break;
    }
    }
}

void termite::undoCommand(CommandHandle handle)
{
    assert(handle.isValid());

    Command* cmd = getCommand(handle);

    if (cmd->state == CommandState::Undo)
        return;

    // Undo all commands that are not Undo(d) after this one (from the end to current)
    if (cmd->nextHandle.isValid()) {
        if (getCommand(cmd->nextHandle)->state != CommandState::Undo)
            undoCommand(cmd->nextHandle);
    }

    // Run current command
    switch (cmd->mode) {
    case CommandMode::Normal:
    {
        CommandType& ctype = g_cmdSys->commandTypes[COMMAND_TYPE_INDEX(handle)];
        if (ctype.undoFn)
            ctype.undoFn(cmd->paramIndex != UINT16_MAX ? ctype.paramPool.getHandleData(1, cmd->paramIndex) : nullptr);
        break;
    }
    case CommandMode::Chain:
    {
        // In chains we have to undo from the last command in the chain to beginning
        CommandHandle lastHandle = cmd->childHandle;
        while (getCommand(lastHandle)->nextHandle.isValid())
            lastHandle = getCommand(lastHandle)->nextHandle;
        
        while (lastHandle) {
            Command* last = getCommand(lastHandle);
            CommandType& ctype = g_cmdSys->commandTypes[COMMAND_TYPE_INDEX(lastHandle)];
            if (ctype.undoFn)
                ctype.undoFn(cmd->paramIndex != UINT16_MAX ? ctype.paramPool.getHandleData(1, cmd->paramIndex) : nullptr);
            lastHandle = last->prevHandle;
        }
        break;
    }
    case CommandMode::Group:
    {
        CommandHandle childHandle = cmd->childHandle;
        while (childHandle) {
            Command* child = getCommand(childHandle);
            CommandType& ctype = g_cmdSys->commandTypes[COMMAND_TYPE_INDEX(childHandle)];
            if (ctype.undoFn)
                ctype.undoFn(cmd->paramIndex != UINT16_MAX ? ctype.paramPool.getHandleData(1, cmd->paramIndex) : nullptr);
            childHandle = child->nextHandle;
        }
        break;
    }
    }
}

CommandHandle termite::getCommandHistory()
{
    return g_cmdSys->lastCommand;
}

CommandHandle termite::getPrevCommand(CommandHandle curHandle)
{
    assert(g_cmdSys);
    assert(curHandle.isValid());

    return g_cmdSys->commandPool.getHandleData<Command>(0, COMMAND_INSTANCE_INDEX(curHandle))->prevHandle;
}

const char* termite::getCommandName(CommandHandle handle)
{
    assert(g_cmdSys);
    assert(handle.isValid());

    Command* cmd = getCommand(handle);
    uint16_t typeIdx = COMMAND_TYPE_INDEX(handle);
    if (typeIdx != UINT16_MAX) {
        return g_cmdSys->commandTypes[typeIdx].name;
    } else if (cmd->childHandle.isValid())  {
        assert(cmd->mode != CommandMode::Normal);
        switch (cmd->mode) {
        case CommandMode::Chain:
            return "[Chain]";
        case CommandMode::Group:
        {
            static char name[64];
            bx::snprintf(name, sizeof(name), "%s [Group]", g_cmdSys->commandTypes[COMMAND_TYPE_INDEX(cmd->childHandle)].name);
            return name;
        }
        default:
            return "[]";
        }
    } else {
        return "[]";
    }
}

void termite::setCommandData(CommandHandle handle, void* userData)
{
    assert(g_cmdSys);
    assert(handle.isValid());
    
    getCommand(handle)->userData = userData;
}
