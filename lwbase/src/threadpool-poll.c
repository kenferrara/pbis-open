/*
 * Copyright © BeyondTrust Software 2004 - 2019
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * BEYONDTRUST MAKES THIS SOFTWARE AVAILABLE UNDER OTHER LICENSING TERMS AS
 * WELL. IF YOU HAVE ENTERED INTO A SEPARATE LICENSE AGREEMENT WITH
 * BEYONDTRUST, THEN YOU MAY ELECT TO USE THE SOFTWARE UNDER THE TERMS OF THAT
 * SOFTWARE LICENSE AGREEMENT INSTEAD OF THE TERMS OF THE APACHE LICENSE,
 * NOTWITHSTANDING THE ABOVE NOTICE.  IF YOU HAVE QUESTIONS, OR WISH TO REQUEST
 * A COPY OF THE ALTERNATE LICENSING TERMS OFFERED BY BEYONDTRUST, PLEASE CONTACT
 * BEYONDTRUST AT beyondtrust.com/contact
 */

/*
 * Module Name:
 *
 *        threadpool-poll.c
 *
 * Abstract:
 *
 *        Thread pool API (poll backend)
 *
 * Authors: Brian Koropoff (bkoropoff@likewise.com)
 *
 */

#include "includes.h"
#include "threadpool-poll.h"

/* Maximum number of ticks (task function invocations) to
   process each iteration of the event loop */
#define MAX_TICKS 1000

static
VOID
TaskDelete(
    PPOLL_TASK pTask
    )
{
    RTL_FREE(&pTask->pUnixSignal);
    RtlMemoryFree(pTask);
}

/*
 * Wakes up a thread.  Call with the thread lock held
 */
static
VOID
SignalThread(
    PPOLL_THREAD pThread
    )
{
    char c = 0;

    if (!pThread->bSignalled)
    {
        int res = write(pThread->SignalFds[1], &c, sizeof(c));
        if (res != sizeof(c)) assert(res == sizeof(c));
        pThread->bSignalled = TRUE;
    }
}

static
VOID
LockAllThreads(
    PLW_THREAD_POOL pPool
    )
{
    ULONG i = 0;

    for (i = 0; i < pPool->ulEventThreadCount; i++)
    {
        LOCK_THREAD(&pPool->pEventThreads[i]);
    }
}

static
VOID
UnlockAllThreads(
    PLW_THREAD_POOL pPool
    )
{
    ULONG i = 0;

    for (i = 0; i < pPool->ulEventThreadCount; i++)
    {
        UNLOCK_THREAD(&pPool->pEventThreads[i]);
    }
}

/*
 * Runs one tick of a task.
 */
static
VOID
RunTask(
    PPOLL_TASK pTask,
    LONG64 llNow
    )
{
    LONG64 llNewTime = 0;

    /* If task had a deadline, set the time we pass into
       the function to the time remaining */
    if (pTask->llDeadline != 0)
    {
        llNewTime = pTask->llDeadline - llNow;

        if (llNewTime < 0)
        {
            llNewTime = 0;
        }
    }

    pTask->pfnFunc(
        pTask,
        pTask->pFuncContext,
        pTask->EventArgs,
        &pTask->EventWait,
        &llNewTime);

    /* Clear event arguments except sticky bits */
    pTask->EventArgs &= STICKY_EVENTS;

    /* If the function gave us a valid time, update the task deadline */
    if (llNewTime != 0)
    {
        pTask->llDeadline = llNow + llNewTime;
    }
    else
    {
        pTask->llDeadline = 0;
    }
}

/*
 * Updates the poll set with the events a task is waiting on.
 */
static
VOID
UpdateEventWait(
    PPOLL_TASK pTask
    )
{
    struct pollfd* pPoll = &pTask->pThread->pPoll[pTask->PollIndex];

    if (pTask->Fd != -1)
    {
        pPoll->events = 0;

        if (pTask->EventWait & LW_TASK_EVENT_FD_READABLE)
        {
            pPoll->events |= POLLIN;
        }

        if (pTask->EventWait & LW_TASK_EVENT_FD_WRITABLE)
        {
            pPoll->events |= POLLOUT;
        }

        if (pTask->EventWait & LW_TASK_EVENT_FD_EXCEPTION)
        {
            pPoll->events |= POLLERR;
        }
    }
}

/*
 * Updates the event args on tasks from epoll results and
 * schedules them to run.
 */

static
BOOLEAN
UpdateEventArgs(
    PPOLL_TASK pTask,
    struct pollfd* pPoll
    )
{
    if (pTask->Fd >= 0 && pPoll[pTask->PollIndex].revents)
    {
        if (pPoll[pTask->PollIndex].revents & POLLIN)
        {
            pTask->EventArgs |= LW_TASK_EVENT_FD_READABLE;
        }
        
        if (pPoll[pTask->PollIndex].revents & (POLLOUT | POLLHUP))
        {
            pTask->EventArgs |= LW_TASK_EVENT_FD_WRITABLE;
        }
        
        if (pPoll[pTask->PollIndex].revents & POLLERR)
        {
            pTask->EventArgs |= LW_TASK_EVENT_FD_EXCEPTION;
        }
        
        return TRUE;
    }

    return FALSE;
}

static
VOID
ScheduleWaitingTasks(
    struct pollfd* pPoll,
    int eventCount,
    PRING pWaiting,
    PRING pTimed,
    PRING pRunnable,
    PBOOLEAN pbSignalled
    )
{
    PRING pRing = NULL;
    PRING pNext = NULL;
    PLW_TASK pTask = NULL;

    if (pPoll[0].revents & POLLIN)
    {
        *pbSignalled = TRUE;
    }

    for (pRing = pWaiting->pNext; eventCount && pRing != pWaiting; pRing = pNext)
    {
        pNext = pRing->pNext;
        pTask = LW_STRUCT_FROM_FIELD(pRing, POLL_TASK, QueueRing);
        
        if (UpdateEventArgs(pTask, pPoll))
        {
            RingRemove(pRing);
            RingEnqueue(pRunnable, pRing);
            eventCount--;
        }
    }

    for (pRing = pTimed->pNext; eventCount && pRing != pTimed; pRing = pNext)
    {
        pNext = pRing->pNext;
        pTask = LW_STRUCT_FROM_FIELD(pRing, POLL_TASK, QueueRing);
        
        if (UpdateEventArgs(pTask, pPoll))
        {
            RingRemove(pRing);
            RingEnqueue(pRunnable, pRing);
            eventCount--;
        }
    }
}

static
VOID
ScheduleTimedTasks(
    PRING pTimed,
    LONG64 llNow,
    PRING pRunnable
    )
{
    PLW_TASK pTask = NULL;
    PRING pRing = NULL;
    PRING pNext = NULL;

    for (pRing = pTimed->pNext; pRing != pTimed; pRing = pNext)
    {
        pNext = pRing->pNext;
        pTask = LW_STRUCT_FROM_FIELD(pRing, POLL_TASK, QueueRing);
        
        /* No more tasks in the queue are past the deadline
           since the queue is sorted */
        if (pTask->llDeadline > llNow)
        {
            break;
        }

        RingRemove(&pTask->QueueRing);
        RingEnqueue(pRunnable, &pTask->QueueRing);
        
        pTask->EventArgs |= LW_TASK_EVENT_TIME;
    }
}

static
VOID
InsertTimedQueue(
    PRING pTimed,
    PLW_TASK pInsert
    )
{
    PLW_TASK pTask = NULL;
    PRING pRing = NULL;

    /* Find the first task in the queue with a later deadline than the task to insert */
    for (pRing = pTimed->pNext; pRing != pTimed; pRing = pRing->pNext)
    {
        pTask = LW_STRUCT_FROM_FIELD(pRing, POLL_TASK, QueueRing);
     
        if (pTask->llDeadline > pInsert->llDeadline)
            break;
    }

    /* Insert the task */
    RingInsertBefore(pRing, &pInsert->QueueRing);
}

static
NTSTATUS
Poll(
    IN PCLOCK pClock,
    IN OUT PLONG64 pllNow,
    IN LONG64 llNextDeadline,
    IN struct pollfd* pPoll,
    IN nfds_t PollSize,
    OUT int* pReady
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    int ready = 0;
    int timeout = 0;

    do
    {
        if (llNextDeadline >= 0)
        {
            /* Convert to timeout in milliseconds */
            timeout = (llNextDeadline - *pllNow) / 1000000ll;
            if (timeout < 0)
            {
                timeout = 0;
            }
        }
        else
        {
            timeout = -1;
        }

        ready = poll(pPoll, PollSize, timeout);
        if (ready < 0 && (errno == EINTR || errno == EAGAIN))
        {
            /* Update current time so the next timeout calculation is correct */
            status = ClockGetMonotonicTime(pClock, pllNow);
            GOTO_ERROR_ON_STATUS(status);
        }
    } while (ready < 0 && (errno == EINTR || errno == EAGAIN));

    if (ready < 0)
    {
        status = LwErrnoToNtStatus(errno);
        GOTO_ERROR_ON_STATUS(status);
    }

    *pReady = ready;

error:

    return status;
}

static
NTSTATUS
ProcessRunnable(
    PPOLL_THREAD pThread,
    PRING pRunnable,
    PRING pTimed,
    PRING pWaiting,
    LONG64 llNow
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG ulTicks = MAX_TICKS;
    PLW_TASK pTask = NULL;
    PLW_TASK_GROUP pGroup = NULL;
    PRING pRing = NULL;
    PRING pNext = NULL;
    
    /* We are guaranteed to run each task at least once.  If tasks remain
       on the runnable list by yielding, we will continue to run them
       all in a round robin until our ticks are depleted. */
    while (ulTicks && !RingIsEmpty(pRunnable))
    {
        for (pRing = pRunnable->pNext; pRing != pRunnable; pRing = pNext)
        {
            pNext = pRing->pNext;
            
            pTask = LW_STRUCT_FROM_FIELD(pRing, POLL_TASK, QueueRing);
            
            RunTask(pTask, llNow);

            if (ulTicks)
            {
                ulTicks--;
            }
            
            if (pTask->EventWait != LW_TASK_EVENT_COMPLETE)
            {
                /* Task is still waiting to be runnable, update events in poll set */
                UpdateEventWait(pTask);
                
                if (pTask->EventWait & LW_TASK_EVENT_YIELD)
                {
                    /* Task is yielding.  Set YIELD in its trigger arguments and
                       and leave it on the runnable list for the next iteration */
                    pTask->EventArgs |= LW_TASK_EVENT_YIELD;
                }                
                else if (pTask->EventWait & LW_TASK_EVENT_TIME)
                {
                    /* If the task is waiting for a timeout, insert it into the timed queue */
                    RingRemove(&pTask->QueueRing);
                    InsertTimedQueue(pTimed, pTask);
                }
                else
                {
                    /* Otherwise, put it in the generic waiting queue */
                    RingRemove(&pTask->QueueRing);
                    RingEnqueue(pWaiting, &pTask->QueueRing);
                }
            }
            else
            {
                /* Task is complete */
                RingRemove(&pTask->QueueRing);

                /* Turn off any fd in the poll set */
                if (pTask->Fd >= 0)
                {
                    status = LwRtlSetTaskFd(pTask, pTask->Fd, 0);
                    GOTO_ERROR_ON_STATUS(status);
                }

                /* Unsubscribe task from any UNIX signals */
                if (pTask->pUnixSignal)
                {
                    RegisterTaskUnixSignal(pTask, 0, FALSE);
                }

                LOCK_POOL(pThread->pPool);
                pThread->ulLoad--;
                UNLOCK_POOL(pThread->pPool);
                
                pGroup = pTask->pGroup;

                /* If task was in a task group, remove it and notify anyone waiting
                   on the group */
                if (pGroup)
                {
                    LOCK_GROUP(pGroup);
                    pTask->pGroup = NULL;
                    RingRemove(&pTask->GroupRing);
                    pthread_cond_broadcast(&pGroup->Event);
                    UNLOCK_GROUP(pGroup);
                }
                
                LOCK_THREAD(pThread);
                if (--pTask->ulRefCount)
                {
                    /* The task still has a reference, so mark it as completed
                       and notify anyone waiting on it */
                    pTask->EventSignal = TASK_COMPLETE_MASK;
                    pthread_cond_broadcast(&pThread->Event);
                    UNLOCK_THREAD(pThread);
                }
                else
                {
                    /* We held the last reference to the task, so delete it */
                    RingRemove(&pTask->SignalRing);
                    UNLOCK_THREAD(pThread);
                    TaskDelete(pTask);
                }
            }
        }
    }

error:

    return status;
}

static
VOID
ScheduleSignalled(
    PPOLL_THREAD pThread,
    PRING pRunnable,
    PBOOLEAN pbShutdown
    )
{
    PRING pRing = NULL;
    PRING pNext = NULL;
    PLW_TASK pTask = NULL;
    char c = 0;
    int res = 0;
    
    LOCK_THREAD(pThread);

    if (pThread->bSignalled)
    {
        pThread->bSignalled = FALSE;
        
        res = read(pThread->SignalFds[0], &c, sizeof(c));
        if (res != sizeof(c)) assert(res == sizeof(c));
        
        /* Add all signalled tasks to the runnable list */
        for (pRing = pThread->Tasks.pNext; pRing != &pThread->Tasks; pRing = pNext)
        {
            pNext = pRing->pNext;
            pTask = LW_STRUCT_FROM_FIELD(pRing, POLL_TASK, SignalRing);
            
            RingRemove(&pTask->SignalRing);
            RingRemove(&pTask->QueueRing);
            
            if (pTask->EventSignal != TASK_COMPLETE_MASK)
            {
                RingEnqueue(pRunnable, &pTask->QueueRing);
                /* Transfer the signal bits into the event args */
                pTask->EventArgs |= pTask->EventSignal;
                pTask->EventSignal = 0;
            }
        }
        
        if (pThread->bShutdown && !*pbShutdown)
        {
            *pbShutdown = pThread->bShutdown;
        }
    }

    UNLOCK_THREAD(pThread);
}
    

static
NTSTATUS
EventLoop(
    PPOLL_THREAD pThread
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    RING timed;
    RING runnable;
    RING waiting;
    CLOCK clock = {0};
    LONG64 llNow = 0;
    LONG64 llNextDeadline = 0;
    int ready = 0;
    BOOLEAN bShutdown = FALSE;
    BOOLEAN bSignalled = FALSE;

    RingInit(&runnable);
    RingInit(&timed);
    RingInit(&waiting);

    for (;;)
    {
        /* Get current time for this iteration */
        status = ClockGetMonotonicTime(&clock, &llNow);
        GOTO_ERROR_ON_STATUS(status);

        /* Schedule any timed tasks that have reached their deadline */
        ScheduleTimedTasks(
            &timed,
            llNow,
            &runnable);

        /* Schedule any waiting tasks that epoll indicated are ready
           and check if the thread received a signal */
        ScheduleWaitingTasks(
            pThread->pPoll,
            ready,
            &waiting,
            &timed,
            &runnable,
            &bSignalled);

        if (bSignalled)
        {
            /* Schedule explicitly-signalled tasks and check if we
               have been told to shut down */
            ScheduleSignalled(
                pThread,
                &runnable,
                &bShutdown);
        }

        /* Process runnable tasks */
        status = ProcessRunnable(
            pThread,
            &runnable,
            &timed,
            &waiting,
            llNow);
        GOTO_ERROR_ON_STATUS(status);

        if (!RingIsEmpty(&runnable))
        {
            /* If there are still runnable tasks, set the next deadline
               to now so we can check for other tasks becoming runnable but
               do not block in Poll() */
            llNextDeadline = llNow;
        }
        else if (!RingIsEmpty(&timed))
        {
            /* There are timed tasks, so set our next deadline to the
               deadline of the first task in the queue */
            llNextDeadline = LW_STRUCT_FROM_FIELD(timed.pNext, POLL_TASK, QueueRing)->llDeadline;
        }
        else if (!RingIsEmpty(&waiting) || !bShutdown)
        {
            /* There are waiting tasks or we are not shutting down, so poll indefinitely */
            llNextDeadline = -1;
        }
        else
        {
            /* We are shutting down and there are no remaining tasks, so leave */
            break;
        }

        /* Wait (or check) for activity */
        status = Poll(
            &clock,
            &llNow,
            llNextDeadline,
            pThread->pPoll,
            pThread->PollSize,
            &ready);
        GOTO_ERROR_ON_STATUS(status);
    }

error:

    return status;
}
    
static
PVOID
EventThread(
    PVOID pContext
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    status = EventLoop((PPOLL_THREAD) pContext);
    if (!NT_SUCCESS(status))
    {
        LW_RTL_LOG_ERROR(
            "Task thread exiting with fatal error: %s (0x%x)",
            LwNtStatusToName(status),
            status);
        abort();
    }

    return NULL;
}

NTSTATUS
LwRtlCreateTask(
    PLW_THREAD_POOL pPool,
    PLW_TASK* ppTask,
    PLW_TASK_GROUP pGroup,
    LW_TASK_FUNCTION pfnFunc,
    PVOID pContext
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PPOLL_TASK pTask = NULL;
    PPOLL_THREAD pThread = NULL;
    ULONG ulMinLoad = 0xFFFFFFFF;
    ULONG ulIndex = 0;

    if (pPool->pDelegate)
    {
        return LwRtlCreateTask(pPool->pDelegate, ppTask, pGroup, pfnFunc, pContext);
    }

    status = LW_RTL_ALLOCATE_AUTO(&pTask);
    GOTO_ERROR_ON_STATUS(status);

    RingInit(&pTask->GroupRing);
    RingInit(&pTask->QueueRing);
    RingInit(&pTask->SignalRing);

    pTask->pGroup = pGroup;
    pTask->ulRefCount = 2;
    pTask->pfnFunc = pfnFunc;
    pTask->pFuncContext = pContext;
    pTask->Fd = -1;
    pTask->EventArgs = LW_TASK_EVENT_INIT;
    pTask->EventWait = LW_TASK_EVENT_EXPLICIT;
    pTask->llDeadline = 0;

    LOCK_POOL(pPool);

    for (ulIndex = 0; ulIndex < pPool->ulEventThreadCount; ulIndex++)
    {
        if (pPool->pEventThreads[ulIndex].ulLoad < ulMinLoad)
        {
            pThread = &pPool->pEventThreads[ulIndex];
            ulMinLoad = pThread->ulLoad;
        }
    }

    pTask->pThread = pThread;

    if (pGroup)
    {
        LOCK_GROUP(pGroup);
        if (pGroup->bCancelled)
        {
            UNLOCK_GROUP(pGroup);
            UNLOCK_POOL(pPool);
            status = STATUS_CANCELLED;
            GOTO_ERROR_ON_STATUS(status);
        }
        RingInsertBefore(&pGroup->Tasks, &pTask->GroupRing);
        UNLOCK_GROUP(pGroup);
    }

    pThread->ulLoad++;

    UNLOCK_POOL(pPool);

    *ppTask = pTask;

cleanup:

    return status;

error:

    if (pTask)
    {
        TaskDelete(pTask);
    }

    *ppTask = NULL;

    goto cleanup;
}

NTSTATUS
LwRtlCreateTaskGroup(
    PLW_THREAD_POOL pPool,
    PLW_TASK_GROUP* ppGroup
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PLW_TASK_GROUP pGroup = NULL;

    if (pPool->pDelegate)
    {
        return LwRtlCreateTaskGroup(pPool->pDelegate, ppGroup);
    }

    status = LW_RTL_ALLOCATE_AUTO(&pGroup);
    GOTO_ERROR_ON_STATUS(status);

    pGroup->pPool = pPool;
    RingInit(&pGroup->Tasks);

    status = LwErrnoToNtStatus(pthread_mutex_init(&pGroup->Lock, NULL));
    GOTO_ERROR_ON_STATUS(status);
    pGroup->bLockInit = TRUE;

    status = LwErrnoToNtStatus(pthread_cond_init(&pGroup->Event, NULL));
    GOTO_ERROR_ON_STATUS(status);
    pGroup->bEventInit = TRUE;

    *ppGroup = pGroup;

cleanup:

    return status;

error:

    LwRtlFreeTaskGroup(&pGroup);
    *ppGroup = NULL;

    goto cleanup;
}

VOID
LwRtlReleaseTask(
    PLW_TASK* ppTask
    )
{
    PLW_TASK pTask = *ppTask;
    int ulRefCount = 0;

    if (pTask)
    {
        LOCK_THREAD(pTask->pThread);
        ulRefCount = --pTask->ulRefCount;
        if (ulRefCount == 0)
        {
            RingRemove(&pTask->SignalRing);
        }
        UNLOCK_THREAD(pTask->pThread);
        
        if (ulRefCount == 0)
        {
            TaskDelete(pTask);
        }
        
        *ppTask = NULL;
    }
}

VOID
RetainTask(
    PLW_TASK pTask
    )
{
    if (pTask)
    {
        LOCK_THREAD(pTask->pThread);
        ++pTask->ulRefCount;
        UNLOCK_THREAD(pTask->pThread);
    }
}

VOID
LwRtlFreeTaskGroup(
    PLW_TASK_GROUP* ppGroup
    )
{
    PLW_TASK_GROUP pGroup = *ppGroup;

    if (pGroup)
    {
        if (pGroup->bLockInit)
        {
            pthread_mutex_destroy(&pGroup->Lock);
        }

        if (pGroup->bEventInit)
        {
            pthread_cond_destroy(&pGroup->Event);
        }

        RtlMemoryFree(pGroup);

        *ppGroup = NULL;
    }
}

static
NTSTATUS
AddPollFd(
    PPOLL_THREAD pThread,
    int Fd,
    nfds_t* pIndex
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    nfds_t index = 0;
    nfds_t NewCapacity = 0;
    struct pollfd* pNewPoll = NULL;

    for (index = 0; index < pThread->PollCapacity; index++)
    {
        if (pThread->pPoll[index].fd == -1)
        {
            break;
        }
    }

    if (index == pThread->PollCapacity)
    {
        NewCapacity = pThread->PollCapacity * 2;
        if (NewCapacity == 0)
        {
            NewCapacity = 16;
        }

        pNewPoll = LwRtlMemoryRealloc(pThread->pPoll, NewCapacity * sizeof(*pNewPoll));
        if (!pNewPoll)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            GOTO_ERROR_ON_STATUS(status);
        }

        for (index = pThread->PollCapacity; index < NewCapacity; index++)
        {
            memset(&pNewPoll[index], 0, sizeof(*pNewPoll));
            pNewPoll[index].fd = -1;
        }

        index = pThread->PollCapacity;
        pThread->pPoll = pNewPoll;
        pThread->PollCapacity = NewCapacity;
    }

    if (index >= pThread->PollSize)
    {
        pThread->PollSize = index + 1;
    }

    memset(&pThread->pPoll[index], 0, sizeof(pThread->pPoll[index]));
    pThread->pPoll[index].fd = Fd;

    *pIndex = index;

error:

    return status;
}

static
VOID
RemovePollFd(
    PPOLL_THREAD pThread,
    nfds_t index
    )
{
    pThread->pPoll[index].fd = -1;

    if (index == pThread->PollSize - 1)
    {
        pThread->PollSize--;
    }
}

LW_NTSTATUS
LwRtlSetTaskFd(
    PLW_TASK pTask,
    int Fd,
    LW_TASK_EVENT_MASK Mask
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Fd < 0)
    {
        status = STATUS_INVALID_HANDLE;
        GOTO_ERROR_ON_STATUS(status);
    }

    if (Fd == pTask->Fd)
    {
        if (Mask == 0)
        {
            RemovePollFd(pTask->pThread, pTask->PollIndex);
            pTask->Fd = -1;
            pTask->PollIndex = 0;
        }
    }
    else if (Mask)
    {
        if (pTask->Fd >= 0)
        {
            /* Only one fd is supported */
            status = STATUS_INSUFFICIENT_RESOURCES;
            GOTO_ERROR_ON_STATUS(status);
        }

        status = AddPollFd(pTask->pThread, Fd, &pTask->PollIndex);
        GOTO_ERROR_ON_STATUS(status);

        pTask->Fd = Fd;
    }

error:

    return status;
}

NTSTATUS
LwRtlQueryTaskFd(
    PLW_TASK pTask,
    int Fd,
    PLW_TASK_EVENT_MASK pMask
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Fd < 0 || Fd != pTask->Fd)
    {
        status = STATUS_INVALID_HANDLE;
        GOTO_ERROR_ON_STATUS(status);
    }

    *pMask = pTask->EventArgs &
        (LW_TASK_EVENT_FD_READABLE |
         LW_TASK_EVENT_FD_WRITABLE |
         LW_TASK_EVENT_FD_EXCEPTION);

cleanup:

    return status;

error:

    *pMask = 0;

    goto cleanup;
}

VOID
LwRtlWakeTask(
    PLW_TASK pTask
    )
{
    LOCK_THREAD(pTask->pThread);

    if (pTask->EventSignal != TASK_COMPLETE_MASK)
    {
        pTask->EventSignal |= LW_TASK_EVENT_EXPLICIT;
        RingRemove(&pTask->SignalRing);
        RingEnqueue(&pTask->pThread->Tasks, &pTask->SignalRing);
        SignalThread(pTask->pThread);
    }

    UNLOCK_THREAD(pTask->pThread);
}

LW_NTSTATUS
LwRtlSetTaskUnixSignal(
    LW_IN PLW_TASK pTask,
    LW_IN int Sig,
    LW_IN LW_BOOLEAN bSubscribe
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (bSubscribe && !pTask->pUnixSignal)
    {
        status = LW_RTL_ALLOCATE_AUTO(&pTask->pUnixSignal);
        GOTO_ERROR_ON_STATUS(status);
    }

    status = RegisterTaskUnixSignal(pTask, Sig, bSubscribe);
    GOTO_ERROR_ON_STATUS(status);

error:

    return status;
}

void
NotifyTaskUnixSignal(
    PLW_TASK pTask,
    siginfo_t* pInfo
    )
{
    LOCK_THREAD(pTask->pThread);

    if (pTask->EventSignal != TASK_COMPLETE_MASK)
    {
        while (pTask->pUnixSignal->si_signo)
        {
            pthread_cond_wait(&pTask->pThread->Event, &pTask->pThread->Lock);
            if (pTask->EventSignal == TASK_COMPLETE_MASK)
            {
                goto cleanup;
            }
        }

        *pTask->pUnixSignal = *pInfo;
        pTask->EventSignal |= LW_TASK_EVENT_UNIX_SIGNAL;
        RingRemove(&pTask->SignalRing);
        RingEnqueue(&pTask->pThread->Tasks, &pTask->SignalRing);
        SignalThread(pTask->pThread);
    }

cleanup:

    UNLOCK_THREAD(pTask->pThread);
}

LW_BOOLEAN
LwRtlNextTaskUnixSignal(
    LW_IN PLW_TASK pTask,
    LW_OUT siginfo_t* pInfo
    )
{
    BOOLEAN bResult = FALSE;

    LOCK_THREAD(pTask->pThread);

    if (pTask->pUnixSignal == NULL || pTask->pUnixSignal->si_signo == 0)
    {
        bResult = FALSE;
    }
    else
    {
        if (pInfo)
        {
            *pInfo = *pTask->pUnixSignal;
        }
        pTask->pUnixSignal->si_signo = 0;
        pthread_cond_broadcast(&pTask->pThread->Event);
        bResult = TRUE;
    }

    UNLOCK_THREAD(pTask->pThread);

    return bResult;
}

VOID
LwRtlCancelTask(
    PLW_TASK pTask
    )
{
    LOCK_THREAD(pTask->pThread);

    if (pTask->EventSignal != TASK_COMPLETE_MASK)
    {
        pTask->EventSignal |= LW_TASK_EVENT_EXPLICIT | LW_TASK_EVENT_CANCEL;
        RingRemove(&pTask->SignalRing);
        RingEnqueue(&pTask->pThread->Tasks, &pTask->SignalRing);
        SignalThread(pTask->pThread);
    }

    UNLOCK_THREAD(pTask->pThread);
}

VOID
LwRtlWaitTask(
    PLW_TASK pTask
    )
{
    LOCK_THREAD(pTask->pThread);

    while (pTask->EventSignal != TASK_COMPLETE_MASK)
    {
        pthread_cond_wait(&pTask->pThread->Event, &pTask->pThread->Lock);
    }

    UNLOCK_THREAD(pTask->pThread);
}

VOID
LwRtlWakeTaskGroup(
    PLW_TASK_GROUP pGroup
    )
{
    PRING ring = NULL;
    PLW_TASK pTask = NULL;

    LOCK_GROUP(pGroup);
    LockAllThreads(pGroup->pPool);

    for (ring = pGroup->Tasks.pNext; ring != &pGroup->Tasks; ring = ring->pNext)
    {
        pTask = LW_STRUCT_FROM_FIELD(ring, POLL_TASK, GroupRing);

        if (pTask->EventSignal != TASK_COMPLETE_MASK)
        {
            pTask->EventSignal |= LW_TASK_EVENT_EXPLICIT;
            RingRemove(&pTask->SignalRing);
            RingEnqueue(&pTask->pThread->Tasks, &pTask->SignalRing);
            SignalThread(pTask->pThread);
        }
    }

    UnlockAllThreads(pGroup->pPool);
    UNLOCK_GROUP(pGroup);
}

VOID
LwRtlCancelTaskGroup(
    PLW_TASK_GROUP pGroup
    )
{
    PRING ring = NULL;
    PLW_TASK pTask = NULL;

    LOCK_GROUP(pGroup);

    pGroup->bCancelled = TRUE;

    LockAllThreads(pGroup->pPool);

    for (ring = pGroup->Tasks.pNext; ring != &pGroup->Tasks; ring = ring->pNext)
    {
        pTask = LW_STRUCT_FROM_FIELD(ring, POLL_TASK, GroupRing);

        if (pTask->EventSignal != TASK_COMPLETE_MASK)
        {
            pTask->EventSignal |= LW_TASK_EVENT_EXPLICIT | LW_TASK_EVENT_CANCEL;
            RingRemove(&pTask->SignalRing);
            RingEnqueue(&pTask->pThread->Tasks, &pTask->SignalRing);
            SignalThread(pTask->pThread);
        }
    }

    UnlockAllThreads(pGroup->pPool);
    UNLOCK_GROUP(pGroup);
}

VOID
LwRtlWaitTaskGroup(
    PLW_TASK_GROUP pGroup
    )
{
    PRING pRing = NULL;
    PLW_TASK pTask = NULL;
    BOOLEAN bStillAlive = TRUE;

    LOCK_GROUP(pGroup);

    while (bStillAlive)
    {
        bStillAlive = FALSE;

        LockAllThreads(pGroup->pPool);

        for (pRing = pGroup->Tasks.pNext; !bStillAlive && pRing != &pGroup->Tasks; pRing = pRing->pNext)
        {
            pTask = LW_STRUCT_FROM_FIELD(pRing, POLL_TASK, GroupRing);
         
            if (pTask->EventSignal != TASK_COMPLETE_MASK)
            {
                bStillAlive = TRUE;
            }
        }

        UnlockAllThreads(pGroup->pPool);

        if (bStillAlive)
        {
            pthread_cond_wait(&pGroup->Event, &pGroup->Lock);
        }
    }

    UNLOCK_GROUP(pGroup);
}

static
NTSTATUS
InitEventThread(
    PPOLL_POOL pPool,
    PLW_THREAD_POOL_ATTRIBUTES pAttrs,
    PPOLL_THREAD pThread,
    ULONG ulCpu
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    pthread_attr_t threadAttr;
    BOOLEAN bThreadAttrInit = FALSE;
    nfds_t index = 0;

    status = LwErrnoToNtStatus(pthread_attr_init(&threadAttr));
    GOTO_ERROR_ON_STATUS(status);
    bThreadAttrInit = TRUE;

    pThread->pPool = pPool;

    status = LwErrnoToNtStatus(pthread_mutex_init(&pThread->Lock, NULL));
    GOTO_ERROR_ON_STATUS(status);

    status = LwErrnoToNtStatus(pthread_cond_init(&pThread->Event, NULL));
    GOTO_ERROR_ON_STATUS(status);

    if (pipe(pThread->SignalFds) < 0)
    {
        status = LwErrnoToNtStatus(errno);
        GOTO_ERROR_ON_STATUS(status);
    }

    SetCloseOnExec(pThread->SignalFds[0]);
    SetCloseOnExec(pThread->SignalFds[1]);
    
    status = AddPollFd(pThread, pThread->SignalFds[0], &index);
    GOTO_ERROR_ON_STATUS(status);
    assert(index == 0);
    pThread->pPoll[index].events = POLLIN;

    RingInit(&pThread->Tasks);

    if (pAttrs && pAttrs->ulTaskThreadStackSize)
    {
        status = LwErrnoToNtStatus(
            pthread_attr_setstacksize(&threadAttr, pAttrs->ulTaskThreadStackSize));
        GOTO_ERROR_ON_STATUS(status);
    }

    status = LwErrnoToNtStatus(
        pthread_create(
            &pThread->Thread,
            &threadAttr,
            EventThread,
            pThread));
    GOTO_ERROR_ON_STATUS(status);

error:

    if (bThreadAttrInit)
    {
        pthread_attr_destroy(&threadAttr);
    }

    return status;
}

static
VOID
DestroyEventThread(
    PPOLL_THREAD pThread
    )
{
    pthread_mutex_destroy(&pThread->Lock);
    pthread_cond_destroy(&pThread->Event);

    RTL_FREE(&pThread->pPoll);

    if (pThread->SignalFds[0] >= 0)
    {
        close(pThread->SignalFds[0]);
    }

    if (pThread->SignalFds[1] >= 0)
    {
        close(pThread->SignalFds[1]);
    }
}

NTSTATUS
LwRtlCreateThreadPool(
    PLW_THREAD_POOL* ppPool,
    PLW_THREAD_POOL_ATTRIBUTES pAttrs
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PLW_THREAD_POOL pPool = NULL;
    int i = 0;
    int numCpus = 0;

    status = LW_RTL_ALLOCATE_AUTO(&pPool);
    GOTO_ERROR_ON_STATUS(status);
    
    status = LwErrnoToNtStatus(pthread_mutex_init(&pPool->Lock, NULL));
    GOTO_ERROR_ON_STATUS(status);

    status = LwErrnoToNtStatus(pthread_cond_init(&pPool->Event, NULL));
    GOTO_ERROR_ON_STATUS(status);

    numCpus = LwRtlGetCpuCount();

    if (GetDelegateAttr(pAttrs))
    {
        status = AcquireDelegatePool(&pPool->pDelegate);
        GOTO_ERROR_ON_STATUS(status);
    }
    else
    {
        pPool->ulEventThreadCount = GetTaskThreadsAttr(pAttrs, numCpus);
        
        if (pPool->ulEventThreadCount)
        {
            status = LW_RTL_ALLOCATE_ARRAY_AUTO(
                &pPool->pEventThreads,
                pPool->ulEventThreadCount);
            GOTO_ERROR_ON_STATUS(status);
            
            for (i = 0; i < pPool->ulEventThreadCount; i++)
            {
                status = InitEventThread(pPool, pAttrs, &pPool->pEventThreads[i], i % numCpus);
                GOTO_ERROR_ON_STATUS(status);
            }
        }
    }

    status = InitWorkThreads(&pPool->WorkThreads, pAttrs, numCpus);
    GOTO_ERROR_ON_STATUS(status);
    
    *ppPool = pPool;

cleanup:

    return status;

error:

    LwRtlFreeThreadPool(&pPool);

    goto cleanup;
}

VOID
LwRtlFreeThreadPool(
    PLW_THREAD_POOL* ppPool
    )
{
    PLW_THREAD_POOL pPool = *ppPool;
    PPOLL_THREAD pThread = NULL;
    int i = 0;

    if (pPool)
    {
        LOCK_POOL(pPool);
        pPool->bShutdown = TRUE;
        pthread_cond_broadcast(&pPool->Event);
        UNLOCK_POOL(pPool);
        
        if (pPool->pEventThreads)
        {
            for (i = 0; i < pPool->ulEventThreadCount; i++)
            {
                pThread = &pPool->pEventThreads[i];
                LOCK_THREAD(pThread);
                pThread->bShutdown = TRUE;
                SignalThread(pThread);
                UNLOCK_THREAD(pThread);
                pthread_join(pThread->Thread, NULL);
                DestroyEventThread(pThread);
            }
            
            RtlMemoryFree(pPool->pEventThreads);
        }
        
        if (pPool->pDelegate)
        {
            ReleaseDelegatePool(&pPool->pDelegate);
        }

        pthread_cond_destroy(&pPool->Event);

        pthread_mutex_destroy(&pPool->Lock);
        
        DestroyWorkThreads(&pPool->WorkThreads);

        RtlMemoryFree(pPool);

        *ppPool = NULL;
    }
}

LW_NTSTATUS
LwRtlCreateWorkItem(
    LW_IN PLW_THREAD_POOL pPool,
    LW_OUT PLW_WORK_ITEM* ppWorkItem,
    LW_IN LW_WORK_ITEM_FUNCTION pfnFunc,
    LW_IN PVOID pContext
    )
{
    return CreateWorkItem(&pPool->WorkThreads, ppWorkItem, pfnFunc, pContext);
}

LW_VOID
LwRtlFreeWorkItem(
    LW_IN LW_OUT PLW_WORK_ITEM* ppWorkItem
    )
{
    FreeWorkItem(ppWorkItem);
}

LW_VOID
LwRtlScheduleWorkItem(
    LW_IN PLW_WORK_ITEM pWorkItem,
    LW_IN LW_SCHEDULE_FLAGS Flags
    )
{
    ScheduleWorkItem(NULL, pWorkItem, Flags);
}

LW_VOID
LwRtlWaitWorkItems(
    LW_IN PLW_THREAD_POOL pPool
    )
{
    WaitWorkItems(&pPool->WorkThreads);
}
