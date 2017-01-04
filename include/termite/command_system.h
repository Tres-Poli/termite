#pragma once

#include "bx/allocator.h"

namespace termite
{
    struct CommandTypeT {};
    struct CommandT {};

    typedef PhantomType<uint32_t, CommandT, UINT32_MAX> CommandHandle;
    typedef PhantomType<uint16_t, CommandTypeT, UINT16_MAX> CommandTypeHandle;

    typedef bool(*ExecuteCommandFn)(void* param);
    typedef void(*UndoCommandFn)(void* param);
    typedef void(*CleanupCommandFn)(void* param);

    result_t initCommandSystem(uint16_t historySize, bx::AllocatorI* alloc);
    void shutdownCommandSystem();

    TERMITE_API CommandTypeHandle registerCommand(const char* name, ExecuteCommandFn executeFn, UndoCommandFn undoFn, 
                                                  CleanupCommandFn cleanupFn /*=nullptr*/, size_t paramSize);
    TERMITE_API CommandTypeHandle findCommand(const char* name);
    
    // Add single command to the queue
    TERMITE_API CommandHandle addCommand(CommandTypeHandle handle, const void* param);
    
    template <typename Tx>
    CommandHandle addCommand(CommandTypeHandle handle, const Tx& param)
    {
        return addCommand(handle, (const void*)&param);
    }

    // Add a group of functions with same type. And different parameters, trigger with one Command
    // Example: Moving a group of objects
    TERMITE_API CommandHandle addCommandGroup(CommandTypeHandle handle, int numCommands, const void* const* params);

    // Begins a commands chain. command chains consists of multiple commands with different types, but executed at once
    // Example: Clone object into a position (Clone, Move, Rotate, Scale)
    TERMITE_API void beginCommandChain();
    TERMITE_API void addCommandChain(CommandTypeHandle handle, const void* param);
    TERMITE_API CommandHandle endCommandChain();

    template <typename Tx>
    CommandHandle addCommandChain(CommandTypeHandle handle, const Tx& param)
    {
        return addCommandChain(handle, (const void*)&param);
    }

    TERMITE_API void executeCommand(CommandHandle handle);
    TERMITE_API void undoCommand(CommandHandle handle);
    TERMITE_API void resetCommands();

    TERMITE_API void undoLastCommand();
    TERMITE_API void redoLastCommand();
    TERMITE_API CommandHandle getLastCommand();
    TERMITE_API CommandHandle getFirstCommand();
    TERMITE_API CommandHandle getPrevCommand(CommandHandle curHandle);
    TERMITE_API CommandHandle getNextCommand(CommandHandle curHandle);
    TERMITE_API const char* getCommandName(CommandHandle handle);
} // namespace termite