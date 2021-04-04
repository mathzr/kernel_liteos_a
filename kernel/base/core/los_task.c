/*
 * Copyright (c) 2013-2019, Huawei Technologies Co., Ltd. All rights reserved.
 * Copyright (c) 2020, Huawei Device Co., Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of
 *    conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list
 *    of conditions and the following disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "los_task_pri.h"
#include "los_base_pri.h"
#include "los_priqueue_pri.h"
#include "los_sem_pri.h"
#include "los_event_pri.h"
#include "los_mux_pri.h"
#include "los_hw_pri.h"
#include "los_exc.h"
#include "los_memstat_pri.h"
#include "los_mp.h"
#include "los_spinlock.h"
#include "los_percpu_pri.h"
#include "los_process_pri.h"
#if (LOSCFG_KERNEL_TRACE == YES)
#include "los_trace.h"
#endif

#ifdef LOSCFG_KERNEL_TICKLESS
#include "los_tickless_pri.h"
#endif
#ifdef LOSCFG_KERNEL_CPUP
#include "los_cpup_pri.h"
#endif
#if (LOSCFG_BASE_CORE_SWTMR == YES)
#include "los_swtmr_pri.h"
#endif
#ifdef LOSCFG_EXC_INTERACTION
#include "los_exc_interaction_pri.h"
#endif
#if (LOSCFG_KERNEL_LITEIPC == YES)
#include "hm_liteipc.h"
#endif
#include "los_strncpy_from_user.h"
#include "los_vm_syscall.h"
#ifdef LOSCFG_ENABLE_OOM_LOOP_TASK
#include "los_oom.h"
#endif
#include "los_vm_map.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#if (LOSCFG_BASE_CORE_TSK_LIMIT <= 0)
#error "task maxnum cannot be zero"
#endif  /* LOSCFG_BASE_CORE_TSK_LIMIT <= 0 */

//任务控制块数组
LITE_OS_SEC_BSS LosTaskCB    *g_taskCBArray;
//空闲任务队列
LITE_OS_SEC_BSS LOS_DL_LIST  g_losFreeTask;
//待回收任务队列
LITE_OS_SEC_BSS LOS_DL_LIST  g_taskRecyleList;
//总的任务数目
LITE_OS_SEC_BSS UINT32       g_taskMaxNum;
//参与任务调度的cpu位图
LITE_OS_SEC_BSS UINT32       g_taskScheduled; /* one bit for each cores */
//回收僵尸任务事件
LITE_OS_SEC_BSS EVENT_CB_S   g_resourceEvent;
/* spinlock for task module, only available on SMP mode */
//任务模块的自旋锁
LITE_OS_SEC_BSS SPIN_LOCK_INIT(g_taskSpin);

//这个函数弱引用OsSetConsoleID
STATIC VOID OsConsoleIDSetHook(UINT32 param1,
                               UINT32 param2) __attribute__((weakref("OsSetConsoleID")));
//这个函数弱引用OsExcStackCheck
STATIC VOID OsExcStackCheckHook(VOID) __attribute__((weakref("OsExcStackCheck")));

//任务处于下述3种状态之一都认为是阻塞态
#define OS_CHECK_TASK_BLOCK (OS_TASK_STATUS_DELAY |    \
                             OS_TASK_STATUS_PEND |     \
                             OS_TASK_STATUS_SUSPEND)

/* temp task blocks for booting procedure */
//将启动过程包装成一个任务，每个CPU都有启动过程，所以每个CPU对应一个任务
LITE_OS_SEC_BSS STATIC LosTaskCB                g_mainTask[LOSCFG_KERNEL_CORE_NUM];

//当前CPU对应的启动任务
VOID* OsGetMainTask()
{
    return (g_mainTask + ArchCurrCpuid());
}

//为每个CPU核初始化启动线程
VOID OsSetMainTask()
{
    UINT32 i;
    CHAR *name = "osMain";  //线程名

	//初始化每个CPU核对应的main线程
    for (i = 0; i < LOSCFG_KERNEL_CORE_NUM; i++) {
        g_mainTask[i].taskStatus = OS_TASK_STATUS_UNUSED;  //空闲状态
        g_mainTask[i].taskID = LOSCFG_BASE_CORE_TSK_LIMIT; //特殊的ID
        g_mainTask[i].priority = OS_TASK_PRIORITY_LOWEST;  //最低优先级
#if (LOSCFG_KERNEL_SMP_LOCKDEP == YES)
        g_mainTask[i].lockDep.lockDepth = 0;   //自旋锁深度目前为0
        g_mainTask[i].lockDep.waitLock = NULL; //没有正在等待的自旋锁 
#endif
		//设置线程名为osMain
        (VOID)strncpy_s(g_mainTask[i].taskName, OS_TCB_NAME_LEN, name, OS_TCB_NAME_LEN - 1);
        LOS_ListInit(&g_mainTask[i].lockList);   //还没有拥有任何互斥锁
    }
}

//空闲任务的处理逻辑
LITE_OS_SEC_TEXT WEAK VOID OsIdleTask(VOID)
{
    while (1) {
#ifdef LOSCFG_KERNEL_TICKLESS
        if (OsTickIrqFlagGet()) {  //检查是否需要进入tickless
            OsTickIrqFlagSet(0);   //关闭时钟中断
            OsTicklessStart();     //启动tickless
        }
#endif
        Wfi();    //等待下一次中断唤醒
    }
}

/*
 * Description : Change task priority.
 * Input       : taskCB    --- task control block
 *               priority  --- priority
 */
 //修订任务的优先级
LITE_OS_SEC_TEXT_MINOR VOID OsTaskPriModify(LosTaskCB *taskCB, UINT16 priority)
{
    LosProcessCB *processCB = NULL;

    LOS_ASSERT(LOS_SpinHeld(&g_taskSpin));

    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
		//原来已经在就绪队列，则需要从一个就绪队列移动到另一个就绪队列
        processCB = OS_PCB_FROM_PID(taskCB->processID);  
        OS_TASK_PRI_QUEUE_DEQUEUE(processCB, taskCB); //从就绪队列移除  
        taskCB->priority = priority;  //修订优先级
        OS_TASK_PRI_QUEUE_ENQUEUE(processCB, taskCB); //移动到新的就绪队列
    } else {
		//原来不在就绪队列，直接修订优先级即可，下次放入就绪队列时，能放入合适的队列
        taskCB->priority = priority;
    }
}

//将当前任务加入超时队列
LITE_OS_SEC_TEXT STATIC INLINE VOID OsAdd2TimerList(LosTaskCB *taskCB, UINT32 timeOut)
{
    SET_SORTLIST_VALUE(&taskCB->sortList, timeOut); //设置超时时间
    OsAdd2SortLink(&OsPercpuGet()->taskSortLink, &taskCB->sortList);  //放入超时队列
#if (LOSCFG_KERNEL_SMP == YES)
    taskCB->timerCpu = ArchCurrCpuid();  //记录为当前任务进行计时的CPU
#endif
}

//将当前任务从超时队列移除
LITE_OS_SEC_TEXT STATIC INLINE VOID OsTimerListDelete(LosTaskCB *taskCB)
{
//每个cpu都有一个独立计时装置
#if (LOSCFG_KERNEL_SMP == YES)
    SortLinkAttribute *sortLinkHeader = &g_percpu[taskCB->timerCpu].taskSortLink;
#else
    SortLinkAttribute *sortLinkHeader = &g_percpu[0].taskSortLink;
#endif
	//从超时队列移除任务
    OsDeleteSortLink(sortLinkHeader, &taskCB->sortList);
}

//将任务放入空闲队列
STATIC INLINE VOID OsInsertTCBToFreeList(LosTaskCB *taskCB)
{
    UINT32 taskID = taskCB->taskID;
	//清空任务描述符
    (VOID)memset_s(taskCB, sizeof(LosTaskCB), 0, sizeof(LosTaskCB));
    taskCB->taskID = taskID;  //任务ID保持原值
    taskCB->taskStatus = OS_TASK_STATUS_UNUSED; //空闲任务
    taskCB->processID = OS_INVALID_VALUE;  //不属于任何进程
    LOS_ListAdd(&g_losFreeTask, &taskCB->pendList); //放入空闲队列
}

//唤醒等待此任务删除的任务
LITE_OS_SEC_TEXT_INIT VOID OsTaskJoinPostUnsafe(LosTaskCB *taskCB)
{
    LosTaskCB *resumedTask = NULL;

    if (taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) {
		//我允许其它任务来等待删除
        if (!LOS_ListEmpty(&taskCB->joinList)) {
			//如果当前有任务在等待我删除
			//那么找出这个任务
            resumedTask = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&(taskCB->joinList)));
            OsTaskWake(resumedTask); //并唤醒它
        }
		//我不再允许其它任务来等到我删除
        taskCB->taskStatus &= ~OS_TASK_FLAG_PTHREAD_JOIN;
    }
	//标记我为退出状态
    taskCB->taskStatus |= OS_TASK_STATUS_EXIT;
}

//等待指定任务的退出
LITE_OS_SEC_TEXT UINT32 OsTaskJoinPendUnsafe(LosTaskCB *taskCB)
{
	//此任务对应的进程
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (!(processCB->processStatus & OS_PROCESS_STATUS_RUNNING)) {
        return LOS_EPERM;  //如果此进程没有任务在运行，那么不允许等待它
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_INIT) {
        return LOS_EINVAL;  //被等待的任务还没有ready
    }

    if ((taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) && LOS_ListEmpty(&taskCB->joinList)) {
		//此任务运行被等待，且现在没有其它任务在等待它，
		//那么我们可以等待它退出
        return OsTaskWait(&taskCB->joinList, LOS_WAIT_FOREVER, TRUE);
    } else if (taskCB->taskStatus & OS_TASK_STATUS_EXIT) {
    	//如果被等待的任务已退出，那么我们直接返回，没有必要等待了
        return LOS_OK;
    }

    return LOS_EINVAL;
}

//设置线程为子删除，而不是其它线程来等待我删除，对应pthread_detach
LITE_OS_SEC_TEXT UINT32 OsTaskSetDeatchUnsafe(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (!(processCB->processStatus & OS_PROCESS_STATUS_RUNNING)) {
        return LOS_EPERM; //对应进程需要处于运行态
    }

    if (taskCB->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) {
		//如果线程运行pthread_join
        if (LOS_ListEmpty(&(taskCB->joinList))) {
			//且当前其它线程没有join
            LOS_ListDelete(&(taskCB->joinList));
			//那么设置成不允许join
            taskCB->taskStatus &= ~OS_TASK_FLAG_PTHREAD_JOIN;
			//并设置成detach状态，即结束后自行删除，不由其它线程辅助删除
            taskCB->taskStatus |= OS_TASK_FLAG_DETACHED;
            return LOS_OK;
        }
        /* This error code has a special purpose and is not allowed to appear again on the interface */
        return LOS_ESRCH;
    }

    return LOS_EINVAL;
}

//判断线程等待的超时时间是否到达，本函数每个tick调用1次
//本函数在中断上下文执行
LITE_OS_SEC_TEXT VOID OsTaskScan(VOID)
{
    SortLinkList *sortList = NULL;
    LosTaskCB *taskCB = NULL;
    BOOL needSchedule = FALSE;
    UINT16 tempStatus;
    LOS_DL_LIST *listObject = NULL;
    SortLinkAttribute *taskSortLink = NULL;

    taskSortLink = &OsPercpuGet()->taskSortLink;  //当前CPU的计时模块
    //每个tick, 逻辑上的钟表(想象墙上的钟表)往前走动1个刻度(即1个tick, 目前是10毫秒)
    taskSortLink->cursor = (taskSortLink->cursor + 1) & OS_TSK_SORTLINK_MASK;
    listObject = taskSortLink->sortLink + taskSortLink->cursor;  //与本刻度相关的超时任务队列

    /*
     * When task is pended with timeout, the task block is on the timeout sortlink
     * (per cpu) and ipc(mutex,sem and etc.)'s block at the same time, it can be waken
     * up by either timeout or corresponding ipc it's waiting.
     *
     * Now synchronize sortlink preocedure is used, therefore the whole task scan needs
     * to be protected, preventing another core from doing sortlink deletion at same time.
     */
     //大致翻译一下上述注释。任务在等待其它资源的同时(信号量，事件组，互斥锁等)，也可以设置超时时间。
     //不管是超时时间先到，还是等待的资源先到，都可以唤醒任务
    LOS_SpinLock(&g_taskSpin);  //因为多个CPU都会访问到同样的任务描述符，所以这里需要保护起来
    //特别是这里的代码会让任务从超时队列移除，以及遍历超时队列

    if (LOS_ListEmpty(listObject)) {
        LOS_SpinUnlock(&g_taskSpin);  //当前刻度没有对应的任务在等待
        return;
    }
	//取出当前刻度第一个等待的任务
    sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
	//减少其等待的时间值，距离上次调整，刻度已经走了1圈，所以这里圈数减1。
    ROLLNUM_DEC(sortList->idxRollNum);

    while (ROLLNUM(sortList->idxRollNum) == 0) {
		//剩余圈数(还需要等待的圈数)为0，表明这个任务超时了，后续的任务都要观察剩余圈数是否为0，如果是，则超时了。
        LOS_ListDelete(&sortList->sortLinkNode);  //从当前刻度移除这个任务
        taskCB = LOS_DL_LIST_ENTRY(sortList, LosTaskCB, sortList);
        taskCB->taskStatus &= ~OS_TASK_STATUS_PEND_TIME;  //任务不再等待时间资源
        tempStatus = taskCB->taskStatus;  //记录任务的当前状态
        if (tempStatus & OS_TASK_STATUS_PEND) {  
			//如果任务还有在等待其它资源，那么也没有必要等待了，因为超时了
            taskCB->taskStatus &= ~OS_TASK_STATUS_PEND;
#if (LOSCFG_KERNEL_LITEIPC == YES)
			//消息队列接收超时，那么没有必要再等对方发消息了
            taskCB->ipcStatus &= ~IPC_THREAD_STATUS_PEND;  
#endif
			//记录当前任务属于等待某种资源超时状态
            taskCB->taskStatus |= OS_TASK_STATUS_TIMEOUT; 
            LOS_ListDelete(&taskCB->pendList);  //从等待队列中移除(不再等待那个资源)
            taskCB->taskSem = NULL;   //不等待信号量
            taskCB->taskMux = NULL;   //不等待互斥锁，
            // 线程在等待某种资源时，已经挂起，不存在同时等待多个资源的情况
        } else {
        	// 没有等待其他资源，但只等待了时间资源，即sleep睡眠的情况
        	// 这里表示sleep结束，需要唤醒任务了，先清除任务delay标志
            taskCB->taskStatus &= ~OS_TASK_STATUS_DELAY;
        }

		//强制挂起的任务只能通过对应的唤醒函数强制唤醒，不通过其他方式唤醒
        if (!(tempStatus & OS_TASK_STATUS_SUSPEND)) {
			//任务不是强制挂起的情况下，放入就绪队列
            OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, OS_PROCESS_STATUS_PEND);
			//并记录下来，后续使用调度器来调度任务
            needSchedule = TRUE;
        }

        if (LOS_ListEmpty(listObject)) {
            break;  //当前刻度没有任务等待了
        }

		//当前事件刻度上的下一个等待任务
        sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
    }

	//当前刻度遍历完成，或者当前刻度剩余的任务都还没有超时
    LOS_SpinUnlock(&g_taskSpin);

    if (needSchedule != FALSE) {
        LOS_MpSchedule(OS_MP_CPU_ALL);
		//也许刚唤醒的一些任务里面存在优先级较高的任务，可以选择其先运行
		//这里就有一个内核抢占的味道了
        LOS_Schedule();  
    }
}


//初始化任务管理模块
LITE_OS_SEC_TEXT_INIT UINT32 OsTaskInit(VOID)
{
    UINT32 index;
    UINT32 ret;
    UINT32 size;

	//任务总数(不含mainTask，即mainTask独立于这里的任务列表)
    g_taskMaxNum = LOSCFG_BASE_CORE_TSK_LIMIT;
    size = (g_taskMaxNum + 1) * sizeof(LosTaskCB);  //额外多申请一个任务控制块
    /*
     * This memory is resident memory and is used to save the system resources
     * of task control block and will not be freed.
     */
     //任务控制块一旦申请，不再释放
    g_taskCBArray = (LosTaskCB *)LOS_MemAlloc(m_aucSysMem0, size);
    if (g_taskCBArray == NULL) {
        return LOS_ERRNO_TSK_NO_MEMORY;
    }
    (VOID)memset_s(g_taskCBArray, size, 0, size);

    LOS_ListInit(&g_losFreeTask);  //空闲任务队列
    LOS_ListInit(&g_taskRecyleList); //待回收任务队列
    for (index = 0; index < g_taskMaxNum; index++) {
        g_taskCBArray[index].taskStatus = OS_TASK_STATUS_UNUSED; //标记每个任务为空闲状态
        g_taskCBArray[index].taskID = index;  //设置每个任务ID为其在数组中的位置
        LOS_ListTailInsert(&g_losFreeTask, &g_taskCBArray[index].pendList); //将任务存入空闲队列
    }

    ret = OsPriQueueInit();  //任务调度优先级队列初始化
    if (ret != LOS_OK) {
        return LOS_ERRNO_TSK_NO_MEMORY;
    }

    /* init sortlink for each core */
	//每个CPU核都有一个计时装置
    for (index = 0; index < LOSCFG_KERNEL_CORE_NUM; index++) {
        ret = OsSortLinkInit(&g_percpu[index].taskSortLink); //初始化计时装置，用于任务相关的计时
        if (ret != LOS_OK) {
            return LOS_ERRNO_TSK_NO_MEMORY;
        }
    }
    return LOS_OK;
}

//获取idle任务id
UINT32 OsGetIdleTaskId(VOID)
{
    Percpu *perCpu = OsPercpuGet();
    return perCpu->idleTaskID;
}

//创建idle任务
LITE_OS_SEC_TEXT_INIT UINT32 OsIdleTaskCreate(VOID)
{
    UINT32 ret;
    TSK_INIT_PARAM_S taskInitParam;
    Percpu *perCpu = OsPercpuGet();
    UINT32 *idleTaskID = &perCpu->idleTaskID;  //每个CPU都有一个idle任务

    (VOID)memset_s((VOID *)(&taskInitParam), sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    taskInitParam.pfnTaskEntry = (TSK_ENTRY_FUNC)OsIdleTask;
    taskInitParam.uwStackSize = LOSCFG_BASE_CORE_TSK_IDLE_STACK_SIZE;  //idle任务只需要比较小的栈
    taskInitParam.pcName = "Idle";
    taskInitParam.usTaskPrio = OS_TASK_PRIORITY_LOWEST;  //只需要比较低的优先级
    taskInitParam.uwResved = OS_TASK_FLAG_IDLEFLAG;  //idle任务标记
#if (LOSCFG_KERNEL_SMP == YES)
    taskInitParam.usCpuAffiMask = CPUID_TO_AFFI_MASK(ArchCurrCpuid());  //每个idle任务只在1个cpu上运行
#endif
    ret = LOS_TaskCreate(idleTaskID, &taskInitParam);  //创建idle任务
    OS_TCB_FROM_TID(*idleTaskID)->taskStatus |= OS_TASK_FLAG_SYSTEM_TASK; //idle任务是系统任务

    return ret;
}

/*
 * Description : get id of current running task.
 * Return      : task id
 */
 //获取当前任务ID
LITE_OS_SEC_TEXT UINT32 LOS_CurTaskIDGet(VOID)
{
    LosTaskCB *runTask = OsCurrTaskGet();

    if (runTask == NULL) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }
    return runTask->taskID;
}

#if (LOSCFG_BASE_CORE_TSK_MONITOR == YES)
//任务切换时对栈进行检查
LITE_OS_SEC_TEXT STATIC VOID OsTaskStackCheck(LosTaskCB *oldTask, LosTaskCB *newTask)
{
    if (!OS_STACK_MAGIC_CHECK(oldTask->topOfStack)) {
		//栈顶校验数字被修订，极大可能发生过栈溢出
        LOS_Panic("CURRENT task ID: %s:%d stack overflow!\n", oldTask->taskName, oldTask->taskID);
    }

    if (((UINTPTR)(newTask->stackPointer) <= newTask->topOfStack) ||
        ((UINTPTR)(newTask->stackPointer) > (newTask->topOfStack + newTask->stackSize))) {
        //当前栈顶指针不在栈空间中，也是异常情况
        LOS_Panic("HIGHEST task ID: %s:%u SP error! StackPointer: %p TopOfStack: %p\n",
                  newTask->taskName, newTask->taskID, newTask->stackPointer, newTask->topOfStack);
    }

    if (OsExcStackCheckHook != NULL) {
        OsExcStackCheckHook();  //对异常处理相关的栈也做一个检查
    }
}

#endif

//任务切换时的检查
LITE_OS_SEC_TEXT_MINOR UINT32 OsTaskSwitchCheck(LosTaskCB *oldTask, LosTaskCB *newTask)
{
#if (LOSCFG_BASE_CORE_TSK_MONITOR == YES)
    OsTaskStackCheck(oldTask, newTask);  //先对栈进行检查
#endif /* LOSCFG_BASE_CORE_TSK_MONITOR == YES */

#if (LOSCFG_KERNEL_TRACE == YES)
	//运维记账
    LOS_Trace(LOS_TRACE_SWITCH, newTask->taskID, oldTask->taskID);
#endif

    return LOS_OK;
}

//任务执行结束后的收尾工作
LITE_OS_SEC_TEXT VOID OsTaskToExit(LosTaskCB *taskCB, UINT32 status)
{
    UINT32 intSave;
    LosProcessCB *runProcess = NULL;
    LosTaskCB *mainTask = NULL;

    SCHEDULER_LOCK(intSave);
	//当前进程
    runProcess = OS_PCB_FROM_PID(taskCB->processID);
	//当前进程的主线程
    mainTask = OS_TCB_FROM_TID(runProcess->threadGroupID);
    SCHEDULER_UNLOCK(intSave);
    if (mainTask == taskCB) {
		//本线程是主线程
		//则当前线程退出线程组
        OsTaskExitGroup(status);
    }

    SCHEDULER_LOCK(intSave);
    if (runProcess->threadNumber == 1) { /* 1: The last task of the process exits */
        SCHEDULER_UNLOCK(intSave);
		//进程随最后一个线程的退出而退出
        (VOID)OsProcessExit(taskCB, status);
        return;
    }

    if (taskCB->taskStatus & OS_TASK_FLAG_DETACHED) {
		//自删除线程的删除逻辑
        (VOID)OsTaskDeleteUnsafe(taskCB, status, intSave);
    }

	//也许某线程在等待我退出，这个时候唤醒它
    OsTaskJoinPostUnsafe(taskCB);
	//因为我不能继续运行了，只能选其它任务来运行
    OsSchedResched();  
    SCHEDULER_UNLOCK(intSave);
    return;
}

/*
 * Description : All task entry
 * Input       : taskID     --- The ID of the task to be run
 */
 //通用的任务处理入口
LITE_OS_SEC_TEXT_INIT VOID OsTaskEntry(UINT32 taskID)
{
    LosTaskCB *taskCB = NULL;

    LOS_ASSERT(!OS_TID_CHECK_INVALID(taskID));

    /*
     * task scheduler needs to be protected throughout the whole process
     * from interrupt and other cores. release task spinlock and enable
     * interrupt in sequence at the task entry.
     */
    LOS_SpinUnlock(&g_taskSpin);  //释放任务调度自旋锁
    (VOID)LOS_IntUnLock();  //并开中断，
    //上面的代码可以认为是任务调度逻辑的一部分，下面的代码是任务的业务代码

    taskCB = OS_TCB_FROM_TID(taskID);
	//调用任务处理对应的具体函数，传入约定的参数，获取返回值
    taskCB->joinRetval = taskCB->taskEntry(taskCB->args[0], taskCB->args[1],
                                           taskCB->args[2], taskCB->args[3]); /* 2 & 3: just for args array index */
    if (taskCB->taskStatus & OS_TASK_FLAG_DETACHED) {
        taskCB->joinRetval = 0;  //如果是自删除任务，不关心其处理结果
    }

    OsTaskToExit(taskCB, 0);  //任务运行结束，正常退出
}


//创建任务前的参数检查
LITE_OS_SEC_TEXT_INIT STATIC UINT32 OsTaskCreateParamCheck(const UINT32 *taskID,
    TSK_INIT_PARAM_S *initParam, VOID **pool)
{
    LosProcessCB *process = NULL;
    UINT32 poolSize = OS_SYS_MEM_SIZE;
    *pool = (VOID *)m_aucSysMem1;

    if (taskID == NULL) {
		//必须要能保存创建后的任务ID结果
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (initParam == NULL) {
		//必须要指定创建任务的参数
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    process = OS_PCB_FROM_PID(initParam->processID);
    if (process->processMode > OS_USER_MODE) {
		//进程模式非法
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (!OsProcessIsUserMode(process)) {
		//内核态进程
        if (initParam->pcName == NULL) {
			//必须指定任务名称
            return LOS_ERRNO_TSK_NAME_EMPTY;
        }
    }

    if (initParam->pfnTaskEntry == NULL) {
		//必须指定任务处理函数
        return LOS_ERRNO_TSK_ENTRY_NULL;
    }

    if (initParam->usTaskPrio > OS_TASK_PRIORITY_LOWEST) {
		//任务优先级必须合法
        return LOS_ERRNO_TSK_PRIOR_ERROR;
    }

#ifdef LOSCFG_EXC_INTERACTION
    if (!OsExcInteractionTaskCheck(initParam)) {
		//如果是idle任务或者shell任务，则使用独立的内存池
        *pool = m_aucSysMem0;
        poolSize = OS_EXC_INTERACTMEM_SIZE;
    }
#endif
    if (initParam->uwStackSize > poolSize) {
		//指定的栈尺寸过大
        return LOS_ERRNO_TSK_STKSZ_TOO_LARGE;
    }

    if (initParam->uwStackSize == 0) {
		//使用默认的栈尺寸
        initParam->uwStackSize = LOSCFG_BASE_CORE_TSK_DEFAULT_STACK_SIZE;
    }
	//栈尺寸对齐处理
    initParam->uwStackSize = (UINT32)ALIGN(initParam->uwStackSize, LOSCFG_STACK_POINT_ALIGN_SIZE);

    if (initParam->uwStackSize < LOS_TASK_MIN_STACK_SIZE) {
		//指定的栈尺寸过小
        return LOS_ERRNO_TSK_STKSZ_TOO_SMALL;
    }

    return LOS_OK;
}

//分配任务栈
LITE_OS_SEC_TEXT_INIT STATIC VOID OsTaskStackAlloc(VOID **topStack, UINT32 stackSize, VOID *pool)
{
	//分配任务栈，压栈方向为高地址到低地址
    *topStack = (VOID *)LOS_MemAllocAlign(pool, stackSize, LOSCFG_STACK_POINT_ALIGN_SIZE);
}

//创建多核间任务同步需要的信号量，信号量初始值为0
STATIC INLINE UINT32 OsTaskSyncCreate(LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    UINT32 ret = LOS_SemCreate(0, &taskCB->syncSignal);
    if (ret != LOS_OK) {
        return LOS_ERRNO_TSK_MP_SYNC_RESOURCE;
    }
#else
    (VOID)taskCB;
#endif
    return LOS_OK;
}

//释放上述信号量
STATIC INLINE VOID OsTaskSyncDestroy(UINT32 syncSignal)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    (VOID)LOS_SemDelete(syncSignal);
#else
    (VOID)syncSignal;
#endif
}

//等待任务在其正在运行的CPU上处理信号，
//等待其处理完成
LITE_OS_SEC_TEXT UINT32 OsTaskSyncWait(const LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    UINT32 ret = LOS_OK;

    LOS_ASSERT(LOS_SpinHeld(&g_taskSpin));
    LOS_SpinUnlock(&g_taskSpin);
	//taskCB虽然正在运行，但是有可能在等待这个自旋锁，所以这里必须释放自旋锁
	//taskCB才有机会释放syncSignal信号量
	//但是，这里并没有开中断，所以，这里的定时器超时等待意义不大(形同虚设)
    /*
     * gc soft timer works every OS_MP_GC_PERIOD period, to prevent this timer
     * triggered right at the timeout has reached, we set the timeout as double
     * of the gc peroid.
     */
    if (LOS_SemPend(taskCB->syncSignal, OS_MP_GC_PERIOD * 2) != LOS_OK) {
        ret = LOS_ERRNO_TSK_MP_SYNC_FAILED;
    }

	//此刻taskCB已经释放了信号量syncSignal
    LOS_SpinLock(&g_taskSpin);

    return ret;
#else
    (VOID)taskCB;
    return LOS_OK;
#endif
}

//唤醒等待此taskCB的任务
STATIC INLINE VOID OsTaskSyncWake(const LosTaskCB *taskCB)
{
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
	//释放信号量即可
    (VOID)OsSemPostUnsafe(taskCB->syncSignal, NULL);  //释放后，不重新调度
#else
    (VOID)taskCB;
#endif
}

//释放任务的内核资源
STATIC VOID OsTaskKernelResourcesToFree(UINT32 syncSignal, UINTPTR topOfStack)
{
    VOID *poolTmp = (VOID *)m_aucSysMem1;

	//删除多cpu间任务同步用到的信号量
    OsTaskSyncDestroy(syncSignal);

#ifdef LOSCFG_EXC_INTERACTION
	//对于shell任务和idle任务，其使用独立的内存池
    if (topOfStack < (UINTPTR)m_aucSysMem1) {
        poolTmp = (VOID *)m_aucSysMem0;
    }
#endif
	//释放本任务在内核中的栈空间
    (VOID)LOS_MemFree(poolTmp, (VOID *)topOfStack);
}


//回收待回收队列上的任务
LITE_OS_SEC_TEXT VOID OsTaskCBRecyleToFree()
{
    LosTaskCB *taskCB = NULL;
    UINT32 intSave;

    SCHEDULER_LOCK(intSave);
	//遍历待回收队列
    while (!LOS_ListEmpty(&g_taskRecyleList)) {
		//取出一个任务
        taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_taskRecyleList));
        LOS_ListDelete(&taskCB->pendList); //任务从队列移除
        SCHEDULER_UNLOCK(intSave);

		//回收这个任务
        OsTaskResourcesToFree(taskCB);

        SCHEDULER_LOCK(intSave);
    }
    SCHEDULER_UNLOCK(intSave);
}

//回收任务资源
LITE_OS_SEC_TEXT VOID OsTaskResourcesToFree(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    UINT32 syncSignal = LOSCFG_BASE_IPC_SEM_LIMIT;
    UINT32 mapSize, intSave;
    UINTPTR mapBase, topOfStack;
    UINT32 ret;

    if (OsProcessIsUserMode(processCB) && (taskCB->userMapBase != 0)) {
		//用户态任务
        SCHEDULER_LOCK(intSave);
		//获取用户空间中栈空间地址和尺寸
        mapBase = (UINTPTR)taskCB->userMapBase;
        mapSize = taskCB->userMapSize;
		//取消任务的栈空间占用
        taskCB->userMapBase = 0;
        taskCB->userArea = 0;
        SCHEDULER_UNLOCK(intSave);

        LOS_ASSERT(!(processCB->vmSpace == NULL));
		//释放栈空间内存
        ret = OsUnMMap(processCB->vmSpace, (UINTPTR)mapBase, mapSize);
        if ((ret != LOS_OK) && (mapBase != 0) && !(processCB->processStatus & OS_PROCESS_STATUS_INIT)) {
            PRINT_ERR("process(%u) ummap user task(%u) stack failed! mapbase: 0x%x size :0x%x, error: %d\n",
                      processCB->processID, taskCB->taskID, mapBase, mapSize, ret);
        }

#if (LOSCFG_KERNEL_LITEIPC == YES)
		//释放本任务IPC资源
        LiteIpcRemoveServiceHandle(taskCB);
#endif
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
		//继续释放内核中的资源
        topOfStack = taskCB->topOfStack;   //内核栈地址
        taskCB->topOfStack = 0;
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
        syncSignal = taskCB->syncSignal;   //任务用于CPU间同步的信号量
        taskCB->syncSignal = LOSCFG_BASE_IPC_SEM_LIMIT;
#endif
		//删除信号量和内核栈
        OsTaskKernelResourcesToFree(syncSignal, topOfStack);

        SCHEDULER_LOCK(intSave);
        OsInsertTCBToFreeList(taskCB);  //任务存入空闲队列
        SCHEDULER_UNLOCK(intSave);
    }
    return;
}

//初始化任务控制块
LITE_OS_SEC_TEXT_INIT STATIC VOID OsTaskCBInitBase(LosTaskCB *taskCB,
                                                   const VOID *stackPtr,
                                                   const VOID *topStack,
                                                   const TSK_INIT_PARAM_S *initParam)
{
	//任务栈
    taskCB->stackPointer = (VOID *)stackPtr;  
	//任务入口函数对应的参数
    taskCB->args[0]      = initParam->auwArgs[0]; /* 0~3: just for args array index */
    taskCB->args[1]      = initParam->auwArgs[1];
    taskCB->args[2]      = initParam->auwArgs[2];
    taskCB->args[3]      = initParam->auwArgs[3];
	//栈顶
    taskCB->topOfStack   = (UINTPTR)topStack;
	//栈尺寸
    taskCB->stackSize    = initParam->uwStackSize;
	//任务优先级
    taskCB->priority     = initParam->usTaskPrio;
	//任务入口函数
    taskCB->taskEntry    = initParam->pfnTaskEntry;
    taskCB->signal       = SIGNAL_NONE; //信号

#if (LOSCFG_KERNEL_SMP == YES)
    taskCB->currCpu      = OS_TASK_INVALID_CPUID;  //任务还未运行
    //默认所有CPU都可以调度本任务
    taskCB->cpuAffiMask  = (initParam->usCpuAffiMask) ?
                            initParam->usCpuAffiMask : LOSCFG_KERNEL_CPU_MASK;
#endif
#if (LOSCFG_KERNEL_LITEIPC == YES)
    LOS_ListInit(&(taskCB->msgListHead));  //任务接收其它任务发来的消息队列头
#endif
	//调度策略
    taskCB->policy = (initParam->policy == LOS_SCHED_FIFO) ? LOS_SCHED_FIFO : LOS_SCHED_RR;
    taskCB->taskStatus = OS_TASK_STATUS_INIT;  //任务当前刚初始化
    if (initParam->uwResved & OS_TASK_FLAG_DETACHED) {
        taskCB->taskStatus |= OS_TASK_FLAG_DETACHED;  //自删除任务
    } else {
        LOS_ListInit(&taskCB->joinList);
        taskCB->taskStatus |= OS_TASK_FLAG_PTHREAD_JOIN; //由其它任务来删除的任务
    }

    taskCB->futex.index = OS_INVALID_VALUE;  //用户态线程的同步和互斥优化
    LOS_ListInit(&taskCB->lockList);  //任务当前所持有的互斥锁列表
}


//初始化任务控制块
STATIC UINT32 OsTaskCBInit(LosTaskCB *taskCB, const TSK_INIT_PARAM_S *initParam,
                           const VOID *stackPtr, const VOID *topStack)
{
    UINT32 intSave;
    UINT32 ret;
    UINT32 numCount;
    UINT16 mode;
    LosProcessCB *processCB = NULL;

	//先初始化一部分
    OsTaskCBInitBase(taskCB, stackPtr, topStack, initParam);

    SCHEDULER_LOCK(intSave);
    processCB = OS_PCB_FROM_PID(initParam->processID);  //任务所在进程
    taskCB->processID = processCB->processID; //记录进程号
    mode = processCB->processMode;  //进程模式
    LOS_ListTailInsert(&(processCB->threadSiblingList), &(taskCB->threadList)); //任务加入进程
    if (mode == OS_USER_MODE) {
		//用户态进程
		//设置用户态栈空间
        taskCB->userArea = initParam->userParam.userArea;
        taskCB->userMapBase = initParam->userParam.userMapBase;
        taskCB->userMapSize = initParam->userParam.userMapSize;
		//将用户态任务上下文(PC, SP)保存在内核栈中，在任务切换的时候能顺利的执行用户态任务
        OsUserTaskStackInit(taskCB->stackPointer, taskCB->taskEntry, initParam->userParam.userSP);
    }

    if (!processCB->threadNumber) {
		//当前任务为进程中的第一个有效任务，设为主任务
        processCB->threadGroupID = taskCB->taskID;
    }
    processCB->threadNumber++;  //进程中有效任务数目增加

    numCount = processCB->threadCount;
    processCB->threadCount++;   //进程中总任务数目增加
    SCHEDULER_UNLOCK(intSave);

    if (initParam->pcName != NULL) {
		//设置任务名称，不同步设置进程名称
        ret = (UINT32)OsSetTaskName(taskCB, initParam->pcName, FALSE);
        if (ret == LOS_OK) {
            return LOS_OK;
        }
    }

	//如果用户没有指定任务名称，则根据进程中当前已有任务数来生成任务名称
    if (snprintf_s(taskCB->taskName, OS_TCB_NAME_LEN, OS_TCB_NAME_LEN - 1, "thread%u", numCount) < 0) {
        return LOS_NOK;
    }
    return LOS_OK;
}

//获取空闲任务控制块
LITE_OS_SEC_TEXT LosTaskCB *OsGetFreeTaskCB(VOID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    SCHEDULER_LOCK(intSave);
    if (LOS_ListEmpty(&g_losFreeTask)) {
        SCHEDULER_UNLOCK(intSave);
        PRINT_ERR("No idle TCB in the system!\n");
        return NULL; //任务控制块耗尽
    }

    taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_losFreeTask));
    LOS_ListDelete(LOS_DL_LIST_FIRST(&g_losFreeTask)); //取出一个空闲的任务控制块
    SCHEDULER_UNLOCK(intSave);

    return taskCB;  //返回空闲的任务控制块
}

//只创建任务，不调度运行
LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskCreateOnly(UINT32 *taskID, TSK_INIT_PARAM_S *initParam)
{
    UINT32 intSave, errRet;
    VOID *topStack = NULL;
    VOID *stackPtr = NULL;
    LosTaskCB *taskCB = NULL;
    VOID *pool = NULL;

	//先检查参数
    errRet = OsTaskCreateParamCheck(taskID, initParam, &pool);
    if (errRet != LOS_OK) {
        return errRet;
    }

	//获取空闲任务控制块
    taskCB = OsGetFreeTaskCB();
    if (taskCB == NULL) {
        errRet = LOS_ERRNO_TSK_TCB_UNAVAILABLE;
        goto LOS_ERREND;
    }

	//创建多cpu场景下用于线程间同步处理信号的信号量
    errRet = OsTaskSyncCreate(taskCB);
    if (errRet != LOS_OK) {
        goto LOS_ERREND_REWIND_TCB;
    }

	//分配任务栈
    OsTaskStackAlloc(&topStack, initParam->uwStackSize, pool);
    if (topStack == NULL) {
        errRet = LOS_ERRNO_TSK_NO_MEMORY;
        goto LOS_ERREND_REWIND_SYNC;
    }

	//初始化任务栈
    stackPtr = OsTaskStackInit(taskCB->taskID, initParam->uwStackSize, topStack, TRUE);
	//初始化任务控制块以及相关数据
    errRet = OsTaskCBInit(taskCB, initParam, stackPtr, topStack);
    if (errRet != LOS_OK) {
        goto LOS_ERREND_TCB_INIT;
    }
    if (OsConsoleIDSetHook != NULL) {
		//设置任务的控制台ID与本任务的控制台ID保持一致
        OsConsoleIDSetHook(taskCB->taskID, OsCurrTaskGet()->taskID);
    }

    *taskID = taskCB->taskID;  //返回成功创建的任务ID
    return LOS_OK;

LOS_ERREND_TCB_INIT:
    (VOID)LOS_MemFree(pool, topStack);
LOS_ERREND_REWIND_SYNC:
#if (LOSCFG_KERNEL_SMP_TASK_SYNC == YES)
    OsTaskSyncDestroy(taskCB->syncSignal);
#endif
LOS_ERREND_REWIND_TCB:
    SCHEDULER_LOCK(intSave);
    OsInsertTCBToFreeList(taskCB);
    SCHEDULER_UNLOCK(intSave);
LOS_ERREND:
    return errRet;
}

LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskCreate(UINT32 *taskID, TSK_INIT_PARAM_S *initParam)
{
    UINT32 ret;
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    if (initParam == NULL) {
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    if (OS_INT_ACTIVE) {
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    if (initParam->uwResved & OS_TASK_FLAG_IDLEFLAG) {
        initParam->processID = OsGetIdleProcessID();
    } else if (OsProcessIsUserMode(OsCurrProcessGet())) {
        initParam->processID = OsGetKernelInitProcessID();
    } else {
        initParam->processID = OsCurrProcessGet()->processID;
    }
    initParam->uwResved &= ~OS_TASK_FLAG_IDLEFLAG;
    initParam->uwResved &= ~OS_TASK_FLAG_PTHREAD_JOIN;
    if (initParam->uwResved & LOS_TASK_STATUS_DETACHED) {
        initParam->uwResved = OS_TASK_FLAG_DETACHED;
    }

    ret = LOS_TaskCreateOnly(taskID, initParam);
    if (ret != LOS_OK) {
        return ret;
    }
    taskCB = OS_TCB_FROM_TID(*taskID);

    SCHEDULER_LOCK(intSave);
    taskCB->taskStatus &= ~OS_TASK_STATUS_INIT;
    OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, 0);
    SCHEDULER_UNLOCK(intSave);

    /* in case created task not running on this core,
       schedule or not depends on other schedulers status. */
    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (OS_SCHEDULER_ACTIVE) {
        LOS_Schedule();
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskResume(UINT32 taskID)
{
    UINT32 intSave;
    UINT16 tempStatus;
    UINT32 errRet;
    LosTaskCB *taskCB = NULL;
    BOOL needSched = FALSE;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);

    /* clear pending signal */
    taskCB->signal &= ~SIGNAL_SUSPEND;

    tempStatus = taskCB->taskStatus;
    if (tempStatus & OS_TASK_STATUS_UNUSED) {
        errRet = LOS_ERRNO_TSK_NOT_CREATED;
        OS_GOTO_ERREND();
    } else if (!(tempStatus & OS_TASK_STATUS_SUSPEND)) {
        errRet = LOS_ERRNO_TSK_NOT_SUSPENDED;
        OS_GOTO_ERREND();
    }

    taskCB->taskStatus &= ~OS_TASK_STATUS_SUSPEND;
    if (!(taskCB->taskStatus & OS_CHECK_TASK_BLOCK)) {
        OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, OS_PROCESS_STATUS_PEND);
        if (OS_SCHEDULER_ACTIVE) {
            needSched = TRUE;
        }
    }

    SCHEDULER_UNLOCK(intSave);

    if (needSched) {
        LOS_MpSchedule(OS_MP_CPU_ALL);
        LOS_Schedule();
    }

    return LOS_OK;

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

/*
 * Check if needs to do the suspend operation on the running task.
 * Return TRUE, if needs to do the suspension.
 * Rerturn FALSE, if meets following circumstances:
 * 1. Do the suspension across cores, if SMP is enabled
 * 2. Do the suspension when preemption is disabled
 * 3. Do the suspension in hard-irq
 * then LOS_TaskSuspend will directly return with 'ret' value.
 */
LITE_OS_SEC_TEXT_INIT STATIC BOOL OsTaskSuspendCheckOnRun(LosTaskCB *taskCB, UINT32 *ret)
{
    /* init default out return value */
    *ret = LOS_OK;

#if (LOSCFG_KERNEL_SMP == YES)
    /* ASYNCHRONIZED. No need to do task lock checking */
    if (taskCB->currCpu != ArchCurrCpuid()) {
        taskCB->signal = SIGNAL_SUSPEND;
        LOS_MpSchedule(taskCB->currCpu);
        return FALSE;
    }
#endif

    if (!OsPreemptableInSched()) {
        /* Suspending the current core's running task */
        *ret = LOS_ERRNO_TSK_SUSPEND_LOCKED;
        return FALSE;
    }

    if (OS_INT_ACTIVE) {
        /* suspend running task in interrupt */
        taskCB->signal = SIGNAL_SUSPEND;
        return FALSE;
    }

    return TRUE;
}

LITE_OS_SEC_TEXT STATIC UINT32 OsTaskSuspend(LosTaskCB *taskCB)
{
    UINT32 errRet;
    UINT16 tempStatus;
    LosTaskCB *runTask = NULL;

    tempStatus = taskCB->taskStatus;
    if (tempStatus & OS_TASK_STATUS_UNUSED) {
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    if (tempStatus & OS_TASK_STATUS_SUSPEND) {
        return LOS_ERRNO_TSK_ALREADY_SUSPENDED;
    }

    if ((tempStatus & OS_TASK_STATUS_RUNNING) &&
        !OsTaskSuspendCheckOnRun(taskCB, &errRet)) {
        return errRet;
    }

    if (tempStatus & OS_TASK_STATUS_READY) {
        OS_TASK_SCHED_QUEUE_DEQUEUE(taskCB, OS_PROCESS_STATUS_PEND);
    }

    taskCB->taskStatus |= OS_TASK_STATUS_SUSPEND;

    runTask = OsCurrTaskGet();
    if (taskCB == runTask) {
        OsSchedResched();
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskSuspend(UINT32 taskID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    UINT32 errRet;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }

    SCHEDULER_LOCK(intSave);
    errRet = OsTaskSuspend(taskCB);
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

STATIC INLINE VOID OsTaskStatusUnusedSet(LosTaskCB *taskCB)
{
    taskCB->taskStatus |= OS_TASK_STATUS_UNUSED;
    taskCB->eventMask = 0;

    OS_MEM_CLEAR(taskCB->taskID);
}

STATIC INLINE VOID OsTaskReleaseHoldLock(LosProcessCB *processCB, LosTaskCB *taskCB)
{
    LosMux *mux = NULL;
    UINT32 ret;

    while (!LOS_ListEmpty(&taskCB->lockList)) {
        mux = LOS_DL_LIST_ENTRY(LOS_DL_LIST_FIRST(&taskCB->lockList), LosMux, holdList);
        ret = OsMuxUnlockUnsafe(taskCB, mux, NULL);
        if (ret != LOS_OK) {
            LOS_ListDelete(&mux->holdList);
            PRINT_ERR("mux ulock failed! : %u\n", ret);
        }
    }

    if (processCB->processMode == OS_USER_MODE) {
        OsTaskJoinPostUnsafe(taskCB);
        OsFutexNodeDeleteFromFutexHash(&taskCB->futex, TRUE, NULL, NULL);
    }

    OsTaskSyncWake(taskCB);
}

LITE_OS_SEC_TEXT VOID OsRunTaskToDelete(LosTaskCB *taskCB)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    OsTaskReleaseHoldLock(processCB, taskCB);
    OsTaskStatusUnusedSet(taskCB);

    LOS_ListDelete(&taskCB->threadList);
    processCB->threadNumber--;
    LOS_ListTailInsert(&g_taskRecyleList, &taskCB->pendList);
    OsEventWriteUnsafe(&g_resourceEvent, OS_RESOURCE_EVENT_FREE, FALSE, NULL);

    OsSchedResched();
    return;
}

/*
 * Check if needs to do the delete operation on the running task.
 * Return TRUE, if needs to do the deletion.
 * Rerturn FALSE, if meets following circumstances:
 * 1. Do the deletion across cores, if SMP is enabled
 * 2. Do the deletion when preemption is disabled
 * 3. Do the deletion in hard-irq
 * then LOS_TaskDelete will directly return with 'ret' value.
 */
STATIC BOOL OsRunTaskToDeleteCheckOnRun(LosTaskCB *taskCB, UINT32 *ret)
{
    /* init default out return value */
    *ret = LOS_OK;

#if (LOSCFG_KERNEL_SMP == YES)
    /* ASYNCHRONIZED. No need to do task lock checking */
    if (taskCB->currCpu != ArchCurrCpuid()) {
        /*
         * the task is running on another cpu.
         * mask the target task with "kill" signal, and trigger mp schedule
         * which might not be essential but the deletion could more in time.
         */
        taskCB->signal = SIGNAL_KILL;
        LOS_MpSchedule(taskCB->currCpu);
        *ret = OsTaskSyncWait(taskCB);
        return FALSE;
    }
#endif

    if (!OsPreemptableInSched()) {
        /* If the task is running and scheduler is locked then you can not delete it */
        *ret = LOS_ERRNO_TSK_DELETE_LOCKED;
        return FALSE;
    }

    if (OS_INT_ACTIVE) {
        /*
         * delete running task in interrupt.
         * mask "kill" signal and later deletion will be handled.
         */
        taskCB->signal = SIGNAL_KILL;
        return FALSE;
    }

    return TRUE;
}

STATIC VOID OsTaskDeleteInactive(LosProcessCB *processCB, LosTaskCB *taskCB)
{
    LosMux *mux = (LosMux *)taskCB->taskMux;

    LOS_ASSERT(!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING));

    OsTaskReleaseHoldLock(processCB, taskCB);

    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
        OS_TASK_SCHED_QUEUE_DEQUEUE(taskCB, 0);
    } else if (taskCB->taskStatus & OS_TASK_STATUS_PEND) {
        LOS_ListDelete(&taskCB->pendList);
        if (LOS_MuxIsValid(mux) == TRUE) {
            OsMuxBitmapRestore(mux, taskCB, (LosTaskCB *)mux->owner);
        }
    }

    if (taskCB->taskStatus & (OS_TASK_STATUS_DELAY | OS_TASK_STATUS_PEND_TIME)) {
        OsTimerListDelete(taskCB);
    }
    OsTaskStatusUnusedSet(taskCB);

    LOS_ListDelete(&taskCB->threadList);
    processCB->threadNumber--;
    LOS_ListTailInsert(&g_taskRecyleList, &taskCB->pendList);
    return;
}

LITE_OS_SEC_TEXT UINT32 OsTaskDeleteUnsafe(LosTaskCB *taskCB, UINT32 status, UINT32 intSave)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    UINT32 mode = processCB->processMode;
    UINT32 errRet = LOS_OK;

    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        errRet = LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
        goto EXIT;
    }

    if ((taskCB->taskStatus & OS_TASK_STATUS_RUNNING) && !OsRunTaskToDeleteCheckOnRun(taskCB, &errRet)) {
        goto EXIT;
    }

    if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING)) {
        OsTaskDeleteInactive(processCB, taskCB);
        SCHEDULER_UNLOCK(intSave);
        OsWriteResourceEvent(OS_RESOURCE_EVENT_FREE);
        return errRet;
    }

    if (mode == OS_USER_MODE) {
        SCHEDULER_UNLOCK(intSave);
        OsTaskResourcesToFree(taskCB);
        SCHEDULER_LOCK(intSave);
    }

#if (LOSCFG_KERNEL_SMP == YES)
    LOS_ASSERT(OsPercpuGet()->taskLockCnt == 1);
#else
    LOS_ASSERT(OsPercpuGet()->taskLockCnt == 0);
#endif
    OsRunTaskToDelete(taskCB);

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return errRet;
}

LITE_OS_SEC_TEXT_INIT UINT32 LOS_TaskDelete(UINT32 taskID)
{
    UINT32 intSave;
    UINT32 ret;
    LosTaskCB *taskCB = NULL;
    LosProcessCB *processCB = NULL;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (OS_INT_ACTIVE) {
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        ret = LOS_ERRNO_TSK_NOT_CREATED;
        OS_GOTO_ERREND();
    }

    if (taskCB->taskStatus & (OS_TASK_FLAG_SYSTEM_TASK | OS_TASK_FLAG_NO_DELETE)) {
        SCHEDULER_UNLOCK(intSave);
        OsBackTrace();
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }
    processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (processCB->threadNumber == 1) {
        if (processCB == OsCurrProcessGet()) {
            SCHEDULER_UNLOCK(intSave);
            OsProcessExit(taskCB, OS_PRO_EXIT_OK);
            return LOS_OK;
        }

        ret = LOS_ERRNO_TSK_ID_INVALID;
        OS_GOTO_ERREND();
    }

    return OsTaskDeleteUnsafe(taskCB, OS_PRO_EXIT_OK, intSave);

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return ret;
}

LITE_OS_SEC_TEXT UINT32 LOS_TaskDelay(UINT32 tick)
{
    UINT32 intSave;
    LosTaskCB *runTask = NULL;

    if (OS_INT_ACTIVE) {
        PRINT_ERR("In interrupt not allow delay task!\n");
        return LOS_ERRNO_TSK_DELAY_IN_INT;
    }

    runTask = OsCurrTaskGet();
    if (runTask->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        OsBackTrace();
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }

    if (!OsPreemptable()) {
        return LOS_ERRNO_TSK_DELAY_IN_LOCK;
    }

    if (tick == 0) {
        return LOS_TaskYield();
    } else {
        SCHEDULER_LOCK(intSave);
        OS_TASK_SCHED_QUEUE_DEQUEUE(runTask, OS_PROCESS_STATUS_PEND);
        OsAdd2TimerList(runTask, tick);
        runTask->taskStatus |= OS_TASK_STATUS_DELAY;
        OsSchedResched();
        SCHEDULER_UNLOCK(intSave);
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT_MINOR UINT16 LOS_TaskPriGet(UINT32 taskID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    UINT16 priority;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return (UINT16)OS_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return (UINT16)OS_INVALID;
    }

    priority = taskCB->priority;
    SCHEDULER_UNLOCK(intSave);
    return priority;
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskPriSet(UINT32 taskID, UINT16 taskPrio)
{
    BOOL isReady = FALSE;
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    UINT16 tempStatus;
    LosProcessCB *processCB = NULL;

    if (taskPrio > OS_TASK_PRIORITY_LOWEST) {
        return LOS_ERRNO_TSK_PRIOR_ERROR;
    }

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    if (taskCB->taskStatus & OS_TASK_FLAG_SYSTEM_TASK) {
        return LOS_ERRNO_TSK_OPERATE_SYSTEM_TASK;
    }

    SCHEDULER_LOCK(intSave);
    tempStatus = taskCB->taskStatus;
    if (tempStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    /* delete the task and insert with right priority into ready queue */
    isReady = tempStatus & OS_TASK_STATUS_READY;
    if (isReady) {
        processCB = OS_PCB_FROM_PID(taskCB->processID);
        OS_TASK_PRI_QUEUE_DEQUEUE(processCB, taskCB);
        taskCB->priority = taskPrio;
        OS_TASK_PRI_QUEUE_ENQUEUE(processCB, taskCB);
    } else {
        taskCB->priority = taskPrio;
        if (tempStatus & OS_TASK_STATUS_RUNNING) {
            isReady = TRUE;
        }
    }

    SCHEDULER_UNLOCK(intSave);
    /* delete the task and insert with right priority into ready queue */
    if (isReady) {
        LOS_MpSchedule(OS_MP_CPU_ALL);
        LOS_Schedule();
    }
    return LOS_OK;
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_CurTaskPriSet(UINT16 taskPrio)
{
    return LOS_TaskPriSet(OsCurrTaskGet()->taskID, taskPrio);
}

/*
 * Description : pend a task in list
 * Input       : list       --- wait task list
 *               taskStatus --- task status
 *               timeOut    ---  Expiry time
 * Return      : LOS_OK on success or LOS_NOK on failure
 */
UINT32 OsTaskWait(LOS_DL_LIST *list, UINT32 timeout, BOOL needSched)
{
    LosTaskCB *runTask = NULL;
    LOS_DL_LIST *pendObj = NULL;

    runTask = OsCurrTaskGet();
    OS_TASK_SCHED_QUEUE_DEQUEUE(runTask, OS_PROCESS_STATUS_PEND);
    pendObj = &runTask->pendList;
    runTask->taskStatus |= OS_TASK_STATUS_PEND;
    LOS_ListTailInsert(list, pendObj);
    if (timeout != LOS_WAIT_FOREVER) {
        runTask->taskStatus |= OS_TASK_STATUS_PEND_TIME;
        OsAdd2TimerList(runTask, timeout);
    }

    if (needSched == TRUE) {
        OsSchedResched();
        if (runTask->taskStatus & OS_TASK_STATUS_TIMEOUT) {
            runTask->taskStatus &= ~OS_TASK_STATUS_TIMEOUT;
            return LOS_ERRNO_TSK_TIMEOUT;
        }
    }
    return LOS_OK;
}

/*
 * Description : delete the task from pendlist and also add to the priqueue
 * Input       : resumedTask --- resumed task
 *               taskStatus  --- task status
 */
VOID OsTaskWake(LosTaskCB *resumedTask)
{
    LOS_ListDelete(&resumedTask->pendList);
    resumedTask->taskStatus &= ~OS_TASK_STATUS_PEND;

    if (resumedTask->taskStatus & OS_TASK_STATUS_PEND_TIME) {
        OsTimerListDelete(resumedTask);
        resumedTask->taskStatus &= ~OS_TASK_STATUS_PEND_TIME;
    }
    if (!(resumedTask->taskStatus & OS_TASK_STATUS_SUSPEND)) {
        OS_TASK_SCHED_QUEUE_ENQUEUE(resumedTask, OS_PROCESS_STATUS_PEND);
    }
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskYield(VOID)
{
    UINT32 tskCount;
    UINT32 intSave;
    LosTaskCB *runTask = NULL;
    LosProcessCB *runProcess = NULL;

    if (OS_INT_ACTIVE) {
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    if (!OsPreemptable()) {
        return LOS_ERRNO_TSK_YIELD_IN_LOCK;
    }

    runTask = OsCurrTaskGet();
    if (OS_TID_CHECK_INVALID(runTask->taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    SCHEDULER_LOCK(intSave);

    /* reset timeslice of yeilded task */
    runTask->timeSlice = 0;
    runProcess = OS_PCB_FROM_PID(runTask->processID);
    tskCount = OS_TASK_PRI_QUEUE_SIZE(runProcess, runTask);
    if (tskCount > 0) {
        OS_TASK_PRI_QUEUE_ENQUEUE(runProcess, runTask);
        runTask->taskStatus |= OS_TASK_STATUS_READY;
    } else {
        SCHEDULER_UNLOCK(intSave);
        return LOS_OK;
    }
    OsSchedResched();
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

LITE_OS_SEC_TEXT_MINOR VOID LOS_TaskLock(VOID)
{
    UINT32 intSave;
    UINT32 *losTaskLock = NULL;

    intSave = LOS_IntLock();
    losTaskLock = &OsPercpuGet()->taskLockCnt;
    (*losTaskLock)++;
    LOS_IntRestore(intSave);
}

LITE_OS_SEC_TEXT_MINOR VOID LOS_TaskUnlock(VOID)
{
    UINT32 intSave;
    UINT32 *losTaskLock = NULL;
    Percpu *percpu = NULL;

    intSave = LOS_IntLock();

    percpu = OsPercpuGet();
    losTaskLock = &OsPercpuGet()->taskLockCnt;
    if (*losTaskLock > 0) {
        (*losTaskLock)--;
        if ((*losTaskLock == 0) && (percpu->schedFlag == INT_PEND_RESCH) &&
            OS_SCHEDULER_ACTIVE) {
            percpu->schedFlag = INT_NO_RESCH;
            LOS_IntRestore(intSave);
            LOS_Schedule();
            return;
        }
    }

    LOS_IntRestore(intSave);
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskInfoGet(UINT32 taskID, TSK_INFO_S *taskInfo)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    if (taskInfo == NULL) {
        return LOS_ERRNO_TSK_PTR_NULL;
    }

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING) || OS_INT_ACTIVE) {
        taskInfo->uwSP = (UINTPTR)taskCB->stackPointer;
    } else {
        taskInfo->uwSP = ArchSPGet();
    }

    taskInfo->usTaskStatus = taskCB->taskStatus;
    taskInfo->usTaskPrio = taskCB->priority;
    taskInfo->uwStackSize = taskCB->stackSize;
    taskInfo->uwTopOfStack = taskCB->topOfStack;
    taskInfo->uwEventMask = taskCB->eventMask;
    taskInfo->taskEvent = taskCB->taskEvent;
    taskInfo->pTaskSem = taskCB->taskSem;
    taskInfo->pTaskMux = taskCB->taskMux;
    taskInfo->uwTaskID = taskID;

    if (strncpy_s(taskInfo->acName, LOS_TASK_NAMELEN, taskCB->taskName, LOS_TASK_NAMELEN - 1) != EOK) {
        PRINT_ERR("Task name copy failed!\n");
    }
    taskInfo->acName[LOS_TASK_NAMELEN - 1] = '\0';

    taskInfo->uwBottomOfStack = TRUNCATE(((UINTPTR)taskCB->topOfStack + taskCB->stackSize),
                                         OS_TASK_STACK_ADDR_ALIGN);
    taskInfo->uwCurrUsed = (UINT32)(taskInfo->uwBottomOfStack - taskInfo->uwSP);

    taskInfo->bOvf = OsStackWaterLineGet((const UINTPTR *)taskInfo->uwBottomOfStack,
                                         (const UINTPTR *)taskInfo->uwTopOfStack, &taskInfo->uwPeakUsed);
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_TaskCpuAffiSet(UINT32 taskID, UINT16 cpuAffiMask)
{
#if (LOSCFG_KERNEL_SMP == YES)
    LosTaskCB *taskCB = NULL;
    UINT32 intSave;
    BOOL needSched = FALSE;
    UINT16 currCpuMask;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ERRNO_TSK_ID_INVALID;
    }

    if (!(cpuAffiMask & LOSCFG_KERNEL_CPU_MASK)) {
        return LOS_ERRNO_TSK_CPU_AFFINITY_MASK_ERR;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return LOS_ERRNO_TSK_NOT_CREATED;
    }

    taskCB->cpuAffiMask = cpuAffiMask;
    currCpuMask = CPUID_TO_AFFI_MASK(taskCB->currCpu);
    if (!(currCpuMask & cpuAffiMask)) {
        needSched = TRUE;
        taskCB->signal = SIGNAL_AFFI;
    }
    SCHEDULER_UNLOCK(intSave);

    if (needSched && OS_SCHEDULER_ACTIVE) {
        LOS_MpSchedule(currCpuMask);
        LOS_Schedule();
    }
#endif
    (VOID)taskID;
    (VOID)cpuAffiMask;
    return LOS_OK;
}

LITE_OS_SEC_TEXT_MINOR UINT16 LOS_TaskCpuAffiGet(UINT32 taskID)
{
#if (LOSCFG_KERNEL_SMP == YES)
#define INVALID_CPU_AFFI_MASK   0
    LosTaskCB *taskCB = NULL;
    UINT16 cpuAffiMask;
    UINT32 intSave;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return INVALID_CPU_AFFI_MASK;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        SCHEDULER_UNLOCK(intSave);
        return INVALID_CPU_AFFI_MASK;
    }

    cpuAffiMask = taskCB->cpuAffiMask;
    SCHEDULER_UNLOCK(intSave);

    return cpuAffiMask;
#else
    (VOID)taskID;
    return 1;
#endif
}

/*
 * Description : Process pending signals tagged by others cores
 */
LITE_OS_SEC_TEXT_MINOR UINT32 OsTaskProcSignal(VOID)
{
    Percpu *percpu = NULL;
    LosTaskCB *runTask = NULL;
    UINT32 intSave, ret;

    /*
     * private and uninterruptable, no protection needed.
     * while this task is always running when others cores see it,
     * so it keeps recieving signals while follow code excuting.
     */
    runTask = OsCurrTaskGet();
    if (runTask->signal == SIGNAL_NONE) {
        goto EXIT;
    }

    if (runTask->signal & SIGNAL_KILL) {
        /*
         * clear the signal, and do the task deletion. if the signaled task has been
         * scheduled out, then this deletion will wait until next run.
         */
        SCHEDULER_LOCK(intSave);
        runTask->signal = SIGNAL_NONE;
        ret = OsTaskDeleteUnsafe(runTask, OS_PRO_EXIT_OK, intSave);
        if (ret) {
            PRINT_ERR("Task proc signal delete task(%u) failed err:0x%x\n", runTask->taskID, ret);
        }
    } else if (runTask->signal & SIGNAL_SUSPEND) {
        runTask->signal &= ~SIGNAL_SUSPEND;

        /* suspend killed task may fail, ignore the result */
        (VOID)LOS_TaskSuspend(runTask->taskID);
#if (LOSCFG_KERNEL_SMP == YES)
    } else if (runTask->signal & SIGNAL_AFFI) {
        runTask->signal &= ~SIGNAL_AFFI;

        /* pri-queue has updated, notify the target cpu */
        LOS_MpSchedule((UINT32)runTask->cpuAffiMask);
#endif
    }

EXIT:
    /* check if needs to schedule */
    percpu = OsPercpuGet();
    if (OsPreemptable() && (percpu->schedFlag == INT_PEND_RESCH)) {
        percpu->schedFlag = INT_NO_RESCH;
        return INT_PEND_RESCH;
    }

    return INT_NO_RESCH;
}

LITE_OS_SEC_TEXT INT32 OsSetTaskName(LosTaskCB *taskCB, const CHAR *name, BOOL setPName)
{
    UINT32 intSave;
    errno_t err;
    LosProcessCB *processCB = NULL;
    const CHAR *namePtr = NULL;
    CHAR nameBuff[OS_TCB_NAME_LEN] = { 0 };

    if ((taskCB == NULL) || (name == NULL)) {
        return EINVAL;
    }

    if (LOS_IsUserAddress((VADDR_T)(UINTPTR)name)) {
        err = LOS_StrncpyFromUser(nameBuff, (const CHAR *)name, OS_TCB_NAME_LEN);
        if (err < 0) {
            return -err;
        }
        namePtr = nameBuff;
    } else {
        namePtr = name;
    }

    SCHEDULER_LOCK(intSave);

    err = strncpy_s(taskCB->taskName, OS_TCB_NAME_LEN, (VOID *)namePtr, OS_TCB_NAME_LEN - 1);
    if (err != EOK) {
        err = EINVAL;
        goto EXIT;
    }

    err = LOS_OK;
    processCB = OS_PCB_FROM_PID(taskCB->processID);
    /* if thread is main thread, then set processName as taskName */
    if ((taskCB->taskID == processCB->threadGroupID) && (setPName == TRUE)) {
        err = (INT32)OsSetProcessName(processCB, (const CHAR *)taskCB->taskName);
        if (err != LOS_OK) {
            err = EINVAL;
        }
    }

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return err;
}

LITE_OS_SEC_TEXT VOID OsTaskExitGroup(UINT32 status)
{
    LosProcessCB *processCB = NULL;
    LosTaskCB *taskCB = NULL;
    LOS_DL_LIST *list = NULL;
    LOS_DL_LIST *head = NULL;
    LosTaskCB *runTask[LOSCFG_KERNEL_CORE_NUM] = { 0 };
    UINT32 intSave;
#if (LOSCFG_KERNEL_SMP == YES)
    UINT16 cpu;
#endif

    SCHEDULER_LOCK(intSave);
    processCB = OsCurrProcessGet();
    if (processCB->processStatus & OS_PROCESS_FLAG_EXIT) {
        SCHEDULER_UNLOCK(intSave);
        return;
    }

    processCB->processStatus |= OS_PROCESS_FLAG_EXIT;
    runTask[ArchCurrCpuid()] = OsCurrTaskGet();
    runTask[ArchCurrCpuid()]->sig.sigprocmask = OS_INVALID_VALUE;

    list = &processCB->threadSiblingList;
    head = list;
    do {
        taskCB = LOS_DL_LIST_ENTRY(list->pstNext, LosTaskCB, threadList);
        if (!(taskCB->taskStatus & OS_TASK_STATUS_RUNNING)) {
            OsTaskDeleteInactive(processCB, taskCB);
        } else {
#if (LOSCFG_KERNEL_SMP == YES)
            if (taskCB->currCpu != ArchCurrCpuid()) {
                taskCB->signal = SIGNAL_KILL;
                runTask[taskCB->currCpu] = taskCB;
                LOS_MpSchedule(taskCB->currCpu);
            }
#endif
            list = list->pstNext;
        }
    } while (head != list->pstNext);

#if (LOSCFG_KERNEL_SMP == YES)
    for (cpu = 0; cpu < LOSCFG_KERNEL_CORE_NUM; cpu++) {
        if ((cpu == ArchCurrCpuid()) || (runTask[cpu] == NULL)) {
            continue;
        }

        (VOID)OsTaskSyncWait(runTask[cpu]);
    }
#endif
    processCB->threadGroupID = OsCurrTaskGet()->taskID;
    SCHEDULER_UNLOCK(intSave);

    LOS_ASSERT(processCB->threadNumber == 1);
    return;
}

LITE_OS_SEC_TEXT VOID OsExecDestroyTaskGroup(VOID)
{
    OsTaskExitGroup(OS_PRO_EXIT_OK);
    OsTaskCBRecyleToFree();
}

LITE_OS_SEC_TEXT VOID OsProcessSuspendAllTask(VOID)
{
    LosProcessCB *process = NULL;
    LosTaskCB *taskCB = NULL;
    LosTaskCB *runTask = NULL;
    LOS_DL_LIST *list = NULL;
    LOS_DL_LIST *head = NULL;
    UINT32 intSave;
    UINT32 ret;

    SCHEDULER_LOCK(intSave);
    process = OsCurrProcessGet();
    runTask = OsCurrTaskGet();

    list = &process->threadSiblingList;
    head = list;
    do {
        taskCB = LOS_DL_LIST_ENTRY(list->pstNext, LosTaskCB, threadList);
        if (taskCB != runTask) {
            ret = OsTaskSuspend(taskCB);
            if ((ret != LOS_OK) && (ret != LOS_ERRNO_TSK_ALREADY_SUSPENDED)) {
                PRINT_ERR("process(%d) suspend all task(%u) failed! ERROR: 0x%x\n",
                          process->processID, taskCB->taskID, ret);
            }
        }
        list = list->pstNext;
    } while (head != list->pstNext);

    SCHEDULER_UNLOCK(intSave);
    return;
}

UINT32 OsUserTaskOperatePermissionsCheck(LosTaskCB *taskCB)
{
    if (taskCB == NULL) {
        return LOS_EINVAL;
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        return LOS_EINVAL;
    }

    if (OsCurrProcessGet()->processID != taskCB->processID) {
        return LOS_EPERM;
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT_INIT STATIC UINT32 OsCreateUserTaskParamCheck(UINT32 processID, TSK_INIT_PARAM_S *param)
{
    UserTaskParam *userParam = NULL;

    if (param == NULL) {
        return OS_INVALID_VALUE;
    }

    userParam = &param->userParam;
    if ((processID == OS_INVALID_VALUE) && !LOS_IsUserAddress(userParam->userArea)) {
        return OS_INVALID_VALUE;
    }

    if (!LOS_IsUserAddress((UINTPTR)param->pfnTaskEntry)) {
        return OS_INVALID_VALUE;
    }

    if ((!userParam->userMapSize) || !LOS_IsUserAddressRange(userParam->userMapBase, userParam->userMapSize)) {
        return OS_INVALID_VALUE;
    }

    if (userParam->userArea &&
        ((userParam->userSP <= userParam->userMapBase) ||
        (userParam->userSP > (userParam->userMapBase + userParam->userMapSize)))) {
        return OS_INVALID_VALUE;
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT_INIT UINT32 OsCreateUserTask(UINT32 processID, TSK_INIT_PARAM_S *initParam)
{
    LosProcessCB *processCB = NULL;
    UINT32 taskID;
    UINT32 ret;
    UINT32 intSave;

    ret = OsCreateUserTaskParamCheck(processID, initParam);
    if (ret != LOS_OK) {
        return ret;
    }

    initParam->uwStackSize = OS_USER_TASK_SYSCALL_SATCK_SIZE;
    initParam->usTaskPrio = OS_TASK_PRIORITY_LOWEST;
    initParam->policy = LOS_SCHED_RR;
    if (processID == OS_INVALID_VALUE) {
        SCHEDULER_LOCK(intSave);
        processCB = OsCurrProcessGet();
        initParam->processID = processCB->processID;
        initParam->consoleID = processCB->consoleID;
        SCHEDULER_UNLOCK(intSave);
    } else {
        processCB = OS_PCB_FROM_PID(processID);
        if (!(processCB->processStatus & (OS_PROCESS_STATUS_INIT | OS_PROCESS_STATUS_RUNNING))) {
            return OS_INVALID_VALUE;
        }
        initParam->processID = processID;
        initParam->consoleID = 0;
    }

    ret = LOS_TaskCreateOnly(&taskID, initParam);
    if (ret != LOS_OK) {
        return OS_INVALID_VALUE;
    }

    return taskID;
}

LITE_OS_SEC_TEXT INT32 LOS_GetTaskScheduler(INT32 taskID)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;
    INT32 policy;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return -LOS_EINVAL;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    SCHEDULER_LOCK(intSave);
    if (taskCB->taskStatus & OS_TASK_STATUS_UNUSED) {
        policy = -LOS_EINVAL;
        OS_GOTO_ERREND();
    }

    policy = taskCB->policy;

LOS_ERREND:
    SCHEDULER_UNLOCK(intSave);
    return policy;
}

LITE_OS_SEC_TEXT INT32 OsTaskSchedulerSetUnsafe(LosTaskCB *taskCB, UINT16 policy, UINT16 priority,
                                                BOOL policyFlag, UINT32 intSave)
{
    BOOL needSched = TRUE;
    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
        OS_TASK_PRI_QUEUE_DEQUEUE(OS_PCB_FROM_PID(taskCB->processID), taskCB);
    }

    if (policyFlag == TRUE) {
        if (policy == LOS_SCHED_FIFO) {
            taskCB->timeSlice = 0;
        }
        taskCB->policy = policy;
    }
    taskCB->priority = priority;

    if (taskCB->taskStatus & OS_TASK_STATUS_INIT) {
        taskCB->taskStatus &= ~OS_TASK_STATUS_INIT;
        taskCB->taskStatus |= OS_TASK_STATUS_READY;
    }

    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
        taskCB->taskStatus &= ~OS_TASK_STATUS_READY;
        OS_TASK_SCHED_QUEUE_ENQUEUE(taskCB, OS_PROCESS_STATUS_INIT);
    } else if (taskCB->taskStatus & OS_TASK_STATUS_RUNNING) {
        goto SCHEDULE;
    } else {
        needSched = FALSE;
    }

SCHEDULE:
    SCHEDULER_UNLOCK(intSave);

    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (OS_SCHEDULER_ACTIVE && (needSched == TRUE)) {
        LOS_Schedule();
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT INT32 LOS_SetTaskScheduler(INT32 taskID, UINT16 policy, UINT16 priority)
{
    UINT32 intSave;
    LosTaskCB *taskCB = NULL;

    if (OS_TID_CHECK_INVALID(taskID)) {
        return LOS_ESRCH;
    }

    if (priority > OS_TASK_PRIORITY_LOWEST) {
        return LOS_EINVAL;
    }

    if ((policy != LOS_SCHED_FIFO) && (policy != LOS_SCHED_RR)) {
        return LOS_EINVAL;
    }

    SCHEDULER_LOCK(intSave);
    taskCB = OS_TCB_FROM_TID(taskID);
    return OsTaskSchedulerSetUnsafe(taskCB, policy, priority, TRUE, intSave);
}

LITE_OS_SEC_TEXT VOID OsWriteResourceEvent(UINT32 events)
{
    (VOID)LOS_EventWrite(&g_resourceEvent, events);
}

STATIC VOID OsResourceRecoveryTask(VOID)
{
    UINT32 ret;

    while (1) {
        ret = LOS_EventRead(&g_resourceEvent, OS_RESOURCE_EVENT_MASK,
                            LOS_WAITMODE_OR | LOS_WAITMODE_CLR, LOS_WAIT_FOREVER);
        if (ret & (OS_RESOURCE_EVENT_FREE | OS_RESOURCE_EVENT_OOM)) {
            OsTaskCBRecyleToFree();

            OsProcessCBRecyleToFree();
        }

#ifdef LOSCFG_ENABLE_OOM_LOOP_TASK
        if (ret & OS_RESOURCE_EVENT_OOM) {
            (VOID)OomCheckProcess();
        }
#endif
    }
}

LITE_OS_SEC_TEXT UINT32 OsCreateResourceFreeTask(VOID)
{
    UINT32 ret;
    UINT32 taskID;
    TSK_INIT_PARAM_S taskInitParam;

    ret = LOS_EventInit((PEVENT_CB_S)&g_resourceEvent);
    if (ret != LOS_OK) {
        return LOS_NOK;
    }

    (VOID)memset_s((VOID *)(&taskInitParam), sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    taskInitParam.pfnTaskEntry = (TSK_ENTRY_FUNC)OsResourceRecoveryTask;
    taskInitParam.uwStackSize = OS_TASK_RESOURCE_STATCI_SIZE;
    taskInitParam.pcName = "ResourcesTask";
    taskInitParam.usTaskPrio = OS_TASK_RESOURCE_FREE_PRIORITY;
    ret = LOS_TaskCreate(&taskID, &taskInitParam);
    if (ret == LOS_OK) {
        OS_TCB_FROM_TID(taskID)->taskStatus |= OS_TASK_FLAG_NO_DELETE;
    }
    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
