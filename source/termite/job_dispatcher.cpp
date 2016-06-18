#include "pch.h"

#include "job_dispatcher.h"

#include "bx/cpu.h"
#include "bx/thread.h"
#include "bx/uint32_t.h"
#include "bx/string.h"
#include "bxx/pool.h"
#include "bxx/stack.h"
#include "bxx/logger.h"
#include "bxx/linked_list.h"
#include "bxx/lock.h"

#include "fcontext/fcontext.h"

#include <mutex>
#include <condition_variable>
#include <thread>  

using namespace termite;

#define DEFAULT_MAX_SMALL_FIBERS 128
#define DEFAULT_MAX_BIG_FIBERS 32
#define DEFAULT_SMALL_STACKSIZE 65536   // 64kb
#define DEFAULT_BIG_STACKSIZE 524288   // 512kb
#define MAX_WAIT_STACKS 32
#define WAIT_STACK_SIZE 8192    // 8kb

class FiberPool;

struct Fiber
{
    typedef bx::ListNode<Fiber*> LNode;

    uint32_t ownerThread;    // by default, owner thread is -1, which indicates that thread owns this fiber
                             // If we wait on a job (fiber), owner thread gets a valid value
    uint16_t jobIndex;
    uint16_t stackIndex;
    jobCounter* counter;
    jobCounter* waitCounter;
    fcontext_t context;
    FiberPool* ownerPool;

    LNode lnode;

    jobCallback callback;
    jobPriority priority;
    void* userData;
};

class FiberPool
{
private:
    bx::AllocatorI* m_alloc;

    Fiber* m_fibers;
    Fiber** m_ptrs;
    fcontext_stack_t* m_stacks;

    uint16_t m_maxFibers;
    int32_t m_index;

    bx::Lock m_lock;

public:
    FiberPool();
    bool create(uint16_t maxFibers, uint32_t stackSize, bx::AllocatorI* alloc);
    void destroy();

    Fiber* newFiber(jobCallback callbackFn, void* userData, uint16_t index, jobPriority priority, FiberPool* pool,
                    jobCounter* counter);
    void deleteFiber(Fiber* fiber);

    uint16_t getMax() const
    {
        return m_maxFibers;
    }
};

struct ThreadData
{
    Fiber* running;     // Current running fiber
    fcontext_stack_t stacks[MAX_WAIT_STACKS];
    int stackIdx;
    bool main;
    uint32_t threadId;   

    ThreadData()
    {
        running = nullptr;
        stackIdx = 0;
        main = false;
        threadId = 0;
        memset(stacks, 0x00, sizeof(stacks));
    }
};

struct CounterContainer
{
    jobCounter counter;
};

struct JobDispatcher
{
    bx::AllocatorI* alloc;
    bx::Thread** threads;
    uint8_t numThreads;
    FiberPool smallFibers;
    FiberPool bigFibers;

    Fiber::LNode* waitList[int(jobPriority::Count)];  // 3 lists for each priority
    bx::Lock jobLock;
    bx::Lock counterLock;
    bx::TlsData threadData;
    volatile int32_t stop;
    volatile int32_t numWaits;

    fcontext_stack_t mainStack;
    bx::FixedPool<CounterContainer> counterPool;
    jobCounter dummyCounter;

    std::mutex workMutex;
    std::condition_variable workCv;

    JobDispatcher()
    {
        alloc = nullptr;
        threads = nullptr;
        numThreads = 0;
        stop = 0;
        dummyCounter = 0;
        numWaits = 0;
        memset(waitList, 0x00, sizeof(waitList));
        memset(&mainStack, 0x00, sizeof(mainStack));
    }
};

static JobDispatcher* g_dispatcher = nullptr;

FiberPool::FiberPool()
{
    m_fibers = nullptr;
    m_stacks = nullptr;
    m_maxFibers = 0;
    m_index = 0;
    m_ptrs = nullptr;
    m_alloc = nullptr;
}

bool FiberPool::create(uint16_t maxFibers, uint32_t stackSize, bx::AllocatorI* alloc)
{
    m_alloc = alloc;

    // Create pool structure
    size_t totalSize =
        sizeof(Fiber)*maxFibers +
        sizeof(Fiber*)*maxFibers +
        sizeof(fcontext_stack_t)*maxFibers;

    uint8_t* buff = (uint8_t*)BX_ALLOC(alloc, totalSize);
    if (!buff)
        return false;
    memset(buff, 0x00, totalSize);

    m_fibers = (Fiber*)buff;
    buff += sizeof(Fiber)*maxFibers;
    m_ptrs = (Fiber**)buff;
    buff += sizeof(Fiber*)*maxFibers;
    m_stacks = (fcontext_stack_t*)buff;

    for (uint16_t i = 0; i < maxFibers; i++)
        m_ptrs[maxFibers - i - 1] = &m_fibers[i];
    m_maxFibers = maxFibers;
    m_index = maxFibers;

    // Create contexts and their stack memories
    for (uint16_t i = 0; i < maxFibers; i++) {
        m_stacks[i] = create_fcontext_stack(stackSize);
        m_fibers[i].stackIndex = i;
        if (!m_stacks[i].sptr)
            return false;
    }

    return true;
}

void FiberPool::destroy()
{
    for (uint16_t i = 0; i < m_maxFibers; i++) {
        if (m_stacks[i].sptr)
            destroy_fcontext_stack(&m_stacks[i]);
    }

    // Free the whole buffer (context+ptrs+stacks)
    if (m_fibers)
        BX_FREE(m_alloc, m_fibers);
}

static ThreadData* createThreadData(bx::AllocatorI* alloc, uint32_t threadId, bool main)
{
    ThreadData* data = BX_NEW(alloc, ThreadData);
    if (!data)
        return nullptr;
    data->main = main;
    data->threadId = threadId;
    memset(data->stacks, 0x00, sizeof(fcontext_stack_t)*MAX_WAIT_STACKS);

    for (int i = 0; i < MAX_WAIT_STACKS; i++) {
        data->stacks[i] = create_fcontext_stack(WAIT_STACK_SIZE);
        if (!data->stacks[i].sptr)
            return nullptr;
    }

    return data;
}

static void destroyThreadData(ThreadData* data, bx::AllocatorI* alloc)
{
    for (int i = 0; i < MAX_WAIT_STACKS; i++) {
        if (data->stacks[i].sptr)
            destroy_fcontext_stack(&data->stacks[i]);
    }
    BX_DELETE(alloc, data);
}

static fcontext_stack_t* pushWaitStack(ThreadData* data)
{
    if (data->stackIdx == MAX_WAIT_STACKS)
        return nullptr;
    return &data->stacks[data->stackIdx++];
}

static fcontext_stack_t* popWaitStack(ThreadData* data)
{
    if (data->stackIdx == 0)
        return nullptr;
    return &data->stacks[--data->stackIdx];
}

static void fiberCallback(fcontext_transfer_t transfer)
{
    Fiber* fiber = (Fiber*)transfer.data;
    ThreadData* data = (ThreadData*)g_dispatcher->threadData.get();

    data->running = fiber;

    fiber->callback(fiber->jobIndex, fiber->userData);

    // Job is finished
    bx::atomicFetchAndSub(fiber->counter, 1);

    data->running = nullptr;

    // Delete the fiber
    fiber->ownerPool->deleteFiber(fiber);

    // Go back
    jump_fcontext(transfer.ctx, transfer.data);
}

Fiber* FiberPool::newFiber(jobCallback callbackFn, void* userData, uint16_t index, jobPriority priority, FiberPool* pool,
                           jobCounter* counter)
{
    bx::LockScope lk(m_lock);
    if (m_index > 0) {
        Fiber* fiber = m_ptrs[--m_index];
        fiber->ownerThread = 0;
        fiber->context = make_fcontext(m_stacks[fiber->stackIndex].sptr, m_stacks[fiber->stackIndex].ssize, fiberCallback);
        fiber->callback = callbackFn;
        fiber->userData = userData;
        fiber->jobIndex = index;
        fiber->waitCounter = &g_dispatcher->dummyCounter;
        fiber->counter = counter;
        fiber->priority = priority;
        fiber->ownerPool = pool;
        memset(&fiber->lnode, 0x00, sizeof(Fiber::LNode));
        return fiber;
    } else {
        return nullptr;
    }
}

void FiberPool::deleteFiber(Fiber* fiber)
{
    bx::LockScope lk(m_lock);
    assert(m_index != m_maxFibers);
    m_ptrs[m_index++] = fiber;
}

static void jobPusherCallback(fcontext_transfer_t transfer)
{
    ThreadData* data = (ThreadData*)transfer.data;

    while (!g_dispatcher->stop) {
        Fiber* fiber = nullptr;
        if (g_dispatcher->jobLock.tryLock()) {
            for (int i = 0; i < int(jobPriority::Count) && !fiber; i++) {
                Fiber::LNode** list = &g_dispatcher->waitList[i];
                Fiber::LNode* node = *list;
                while (node && !fiber) {
                    Fiber* f = node->data;
                    if (*f->waitCounter == 0) {
                        if (f->ownerThread == 0 || f->ownerThread == data->threadId) {
                            // Job is ready to run, pull it from the wait list
                            fiber = f;
                            bx::removeListNode<Fiber*>(list, &f->lnode);

                            std::lock_guard<std::mutex> lk(g_dispatcher->workMutex);
                            bx::atomicFetchAndSub<int32_t>(&g_dispatcher->numWaits, 1);
                        }
                    }
                    node = node->next;
                }
            }
            g_dispatcher->jobLock.unlock();
        }

        if (fiber) {
            // If jobPusher is called within another 'Wait' call, get back to it's context, which should be the same working thread
            // Else just run the job from beginning
            if (fiber->ownerThread) {
                fiber->ownerThread = 0;
                jump_fcontext(transfer.ctx, transfer.data);
            } else {
                jump_fcontext(fiber->context, fiber);
            }
        } else if (data->main && g_dispatcher->numWaits == 0) {
            break;
        }

        if (!data->main) {
            std::unique_lock<std::mutex> lk(g_dispatcher->workMutex);
            g_dispatcher->workCv.wait(lk, [] {return g_dispatcher->numWaits > 0; });
        }
    }

    // Back to main thread func
    jump_fcontext(transfer.ctx, transfer.data);
}

static jobHandle dispatch(const jobDesc* jobs, uint16_t numJobs, FiberPool* pool) T_THREAD_SAFE
{
    // Get dispatcher counter to assign to jobs
    ThreadData* data = (ThreadData*)g_dispatcher->threadData.get();

    // Get a counter
    g_dispatcher->counterLock.lock();
    jobCounter* counter = &g_dispatcher->counterPool.newInstance()->counter;
    g_dispatcher->counterLock.unlock();
    if (!counter) {
        BX_WARN("Exceeded maximum counters");
        return nullptr;
    }
    *counter = numJobs;

    if (data->running)
        data->running->waitCounter = counter;

    // Create N Fibers/Job
    // Push to list in reverse form to optimize fiber cache reading a bit
    uint32_t c = 0;
    g_dispatcher->jobLock.lock();
    for (uint16_t i = 0; i < numJobs; i++) {
        int index = numJobs - i - 1;
        Fiber* fiber = pool->newFiber(jobs[index].callback, jobs[index].userParam, index, jobs[index].priority, pool, counter);
        if (fiber) {
            bx::addListNode<Fiber*>(&g_dispatcher->waitList[int(fiber->priority)], &fiber->lnode, fiber);
            c++;
        } else {
            BX_WARN("Exceeded maximum jobs (%d)", pool->getMax());
        }
    }
    g_dispatcher->jobLock.unlock();

    // Notify all threads to continue
    if (c) {
        g_dispatcher->workMutex.lock();
        bx::atomicFetchAndAdd<int32_t>(&g_dispatcher->numWaits, c);
        g_dispatcher->workMutex.unlock();
        g_dispatcher->workCv.notify_all();
    }
    return counter;
}

jobHandle termite::jobDispatchSmall(const jobDesc* jobs, uint16_t numJobs) T_THREAD_SAFE
{
    return dispatch(jobs, numJobs, &g_dispatcher->smallFibers);
}

jobHandle termite::jobDispatchBig(const jobDesc* jobs, uint16_t numJobs) T_THREAD_SAFE
{
    return dispatch(jobs, numJobs, &g_dispatcher->bigFibers);
}

void termite::jobWait(jobHandle handle) T_THREAD_SAFE
{
    ThreadData* data = (ThreadData*)g_dispatcher->threadData.get();

    if (*handle > 0) {
        // Jump back to worker thread (current job is not finished)
        fcontext_stack_t* stack = pushWaitStack(data);
        if (!stack) {
            BX_WARN("Maximum wait stacks '%d' exceeded. Cannot wait", MAX_WAIT_STACKS);
            return;
        }

        fcontext_t jobPusherCtx = make_fcontext(stack->sptr, stack->ssize, jobPusherCallback);

        // ReAdd the running fiber (the one that the user has called Wait on), to the waiting list again
        if (data->running) {
            Fiber* fiber = data->running;
            data->running = nullptr;
            fiber->ownerThread = data->threadId;

            g_dispatcher->jobLock.lock();
            bx::addListNode<Fiber*>(&g_dispatcher->waitList[int(fiber->priority)], &fiber->lnode, fiber);
            g_dispatcher->jobLock.unlock();

            // Notify threads to continue
            g_dispatcher->workMutex.lock();
            bx::atomicFetchAndAdd<int32_t>(&g_dispatcher->numWaits, 1);
            g_dispatcher->workMutex.unlock();
            g_dispatcher->workCv.notify_all();
        }

        // Run job pusher and exit current one
        jump_fcontext(jobPusherCtx, data);
        popWaitStack(data);
    }

    // Delete the counter
    g_dispatcher->counterLock.lock();
    g_dispatcher->counterPool.deleteInstance((CounterContainer*)handle);
    g_dispatcher->counterLock.unlock();
}

static int32_t threadFunc(void* userData)
{
    // Initialize thread data
    ThreadData* data = createThreadData(g_dispatcher->alloc, bx::getTid(), false);
    if (!data)
        return -1;
    g_dispatcher->threadData.set(data);     

    fcontext_stack_t* stack = pushWaitStack(data);
    fcontext_t threadCtx = make_fcontext(stack->sptr, stack->ssize, jobPusherCallback);
    jump_fcontext(threadCtx, data);

    destroyThreadData(data, g_dispatcher->alloc);
    return 0;
}

result_t termite::jobInit(bx::AllocatorI* alloc,
                          uint16_t maxSmallFibers, uint32_t smallFiberStackSize,
                          uint16_t maxBigFibers, uint32_t bigFiberStackSize,
                          bool lockThreadsToCores, uint8_t numWorkerThreads)
{
    if (g_dispatcher) {
        assert(false);
        return T_ERR_FAILED;
    }
    g_dispatcher = BX_NEW(alloc, JobDispatcher);
    if (!g_dispatcher)
        return T_ERR_OUTOFMEM;
    g_dispatcher->alloc = alloc;

    // Main thread data and stack
    g_dispatcher->mainStack = create_fcontext_stack(8 * 1024);
    if (!g_dispatcher->mainStack.sptr) {
        return T_ERR_FAILED;
    }

    ThreadData* mainData = createThreadData(alloc, bx::getTid(), true);
    if (!mainData)
        return T_ERR_FAILED;
    g_dispatcher->threadData.set(mainData);

    // Create fibers with stack memories
    maxSmallFibers = maxSmallFibers ? maxSmallFibers : DEFAULT_MAX_SMALL_FIBERS;
    maxBigFibers = maxBigFibers ? maxBigFibers : DEFAULT_MAX_BIG_FIBERS;
    smallFiberStackSize = smallFiberStackSize ? smallFiberStackSize : DEFAULT_SMALL_STACKSIZE;
    bigFiberStackSize = bigFiberStackSize ? bigFiberStackSize : DEFAULT_BIG_STACKSIZE;

    if (!g_dispatcher->counterPool.create(maxSmallFibers + maxBigFibers, alloc)) {
        return T_ERR_OUTOFMEM;
    }

    BX_BEGINP("Creating %d fibers with %d(kb) stack", maxBigFibers, bigFiberStackSize/1024);
    if (!g_dispatcher->bigFibers.create(maxBigFibers, bigFiberStackSize, alloc)) {
        BX_END_FATAL();
        return T_ERR_FAILED;
    }
    BX_END_OK();

    BX_BEGINP("Creating %d fibers with %d(kb) stack", maxSmallFibers, smallFiberStackSize/1024);
    if (!g_dispatcher->smallFibers.create(maxSmallFibers, smallFiberStackSize, alloc)) {
        BX_END_FATAL();
        return T_ERR_FAILED;
    }
    BX_END_OK();

    // Create threads
    if (numWorkerThreads == UINT8_MAX) {
        uint32_t numCores = std::thread::hardware_concurrency();
        numWorkerThreads = (uint8_t)bx::uint32_min(numCores ? (numCores - 1) : 0, UINT8_MAX);
    }

    if (numWorkerThreads > 0) {
        BX_BEGINP("Starting %d worker threads", numWorkerThreads);
        g_dispatcher->threads = (bx::Thread**)BX_ALLOC(alloc, sizeof(bx::Thread*)*numWorkerThreads);
        assert(g_dispatcher->threads);
        
        g_dispatcher->numThreads = numWorkerThreads;
        for (uint8_t i = 0; i < numWorkerThreads; i++) {
            g_dispatcher->threads[i] = BX_NEW(alloc, bx::Thread);
            char name[32];
            bx::snprintf(name, sizeof(name), "Thread #%d", i + 1);
            g_dispatcher->threads[i]->init(threadFunc, nullptr, 8 * 1024, name);
        }

        BX_END_OK();
    }
    return T_OK;
}

void termite::jobShutdown()
{
    if (!g_dispatcher)
        return;

    BX_BEGINP("Shutting down job scheduler");

    // Command all worker threads to stop
    g_dispatcher->stop = 1;
    g_dispatcher->workMutex.lock();
    g_dispatcher->numWaits = g_dispatcher->numThreads + 1;
    g_dispatcher->workMutex.unlock();
    g_dispatcher->workCv.notify_all();
    for (uint8_t i = 0; i < g_dispatcher->numThreads; i++) {
        g_dispatcher->threads[i]->shutdown();
        BX_DELETE(g_dispatcher->alloc, g_dispatcher->threads[i]);
    }
    BX_FREE(g_dispatcher->alloc, g_dispatcher->threads);

    ThreadData* data = (ThreadData*)g_dispatcher->threadData.get();
    destroyThreadData(data, g_dispatcher->alloc);

    g_dispatcher->bigFibers.destroy();
    g_dispatcher->smallFibers.destroy();
    destroy_fcontext_stack(&g_dispatcher->mainStack);

    g_dispatcher->counterPool.destroy();

    BX_DELETE(g_dispatcher->alloc, g_dispatcher);
    g_dispatcher = nullptr;
    BX_END_OK();
}
