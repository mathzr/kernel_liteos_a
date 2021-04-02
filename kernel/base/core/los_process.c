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

#include "los_process_pri.h"
#include "los_task_pri.h"
#include "los_hw_pri.h"
#include "los_sem_pri.h"
#include "los_mp.h"
#include "los_exc.h"
#include "asm/page.h"
#ifdef LOSCFG_FS_VFS
#include "fs/fd_table.h"
#endif
#include "time.h"
#include "user_copy.h"
#include "los_signal.h"
#ifdef LOSCFG_KERNEL_CPUP
#include "los_cpup_pri.h"
#endif
#ifdef LOSCFG_SECURITY_VID
#include "vid_api.h"
#endif
#ifdef LOSCFG_SECURITY_CAPABILITY
#include "capability_api.h"
#endif
#include "los_swtmr_pri.h"
#include "los_vm_map.h"
#include "los_vm_phys.h"
#include "los_vm_syscall.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//每个CPU核当前正在运行的进程
LITE_OS_SEC_BSS LosProcessCB *g_runProcess[LOSCFG_KERNEL_CORE_NUM];
//系统中的所有进程
LITE_OS_SEC_BSS LosProcessCB *g_processCBArray = NULL;
//空闲状态的进程列表
LITE_OS_SEC_DATA_INIT STATIC LOS_DL_LIST g_freeProcess;
//等待回收的进程列表
LITE_OS_SEC_DATA_INIT STATIC LOS_DL_LIST g_processRecyleList;
//用户态init进程
LITE_OS_SEC_BSS UINT32 g_userInitProcess = OS_INVALID_VALUE;
//内核态KProcess进程
LITE_OS_SEC_BSS UINT32 g_kernelInitProcess = OS_INVALID_VALUE;
//内核态Idle进程
LITE_OS_SEC_BSS UINT32 g_kernelIdleProcess = OS_INVALID_VALUE;
//总进程数目
LITE_OS_SEC_BSS UINT32 g_processMaxNum;
//进程组列表
LITE_OS_SEC_BSS ProcessGroup *g_processGroup = NULL;

//将任务从就绪队列移除
LITE_OS_SEC_TEXT_INIT VOID OsTaskSchedQueueDequeue(LosTaskCB *taskCB, UINT16 status)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (taskCB->taskStatus & OS_TASK_STATUS_READY) {
		//任务从就绪队列移除
        OS_TASK_PRI_QUEUE_DEQUEUE(processCB, taskCB);
		//不再是就绪状态
        taskCB->taskStatus &= ~OS_TASK_STATUS_READY;
    }

    if (processCB->threadScheduleMap != 0) {
        return; //进程内还有任务是就绪状态
    }

	//进程内所有任务都不是就绪状态
    if (processCB->processStatus & OS_PROCESS_STATUS_READY) {
		//则进程也切换成非就绪状态
        processCB->processStatus &= ~OS_PROCESS_STATUS_READY;
        OS_PROCESS_PRI_QUEUE_DEQUEUE(processCB); //从就绪队列移除进程
    }

#if (LOSCFG_KERNEL_SMP == YES)
	//其它CPU没有运行此进程下的任务
    if (OS_PROCESS_GET_RUNTASK_COUNT(processCB->processStatus) == 1) {
#endif
		//设置进程状态为阻塞状态
        processCB->processStatus |= status;
#if (LOSCFG_KERNEL_SMP == YES)
    }
#endif
}

//将任务放入就绪队列
STATIC INLINE VOID OsSchedTaskEnqueue(LosProcessCB *processCB, LosTaskCB *taskCB)
{
    if (((taskCB->policy == LOS_SCHED_RR) && (taskCB->timeSlice != 0)) || //时间片还未用完的任务
    	//或者正在运行的FIFO任务FIFO任务，存入就绪队列头部，优先被调用
        ((taskCB->taskStatus & OS_TASK_STATUS_RUNNING) && (taskCB->policy == LOS_SCHED_FIFO))) {
        OS_TASK_PRI_QUEUE_ENQUEUE_HEAD(processCB, taskCB);
    } else {
		//其它情况存入就绪队列尾部
        OS_TASK_PRI_QUEUE_ENQUEUE(processCB, taskCB);
    }
    taskCB->taskStatus |= OS_TASK_STATUS_READY; //任务任务就绪状态
}

//将任务存入就绪队列，同时考虑进程的情况
LITE_OS_SEC_TEXT_INIT VOID OsTaskSchedQueueEnqueue(LosTaskCB *taskCB, UINT16 status)
{
    LosProcessCB *processCB = NULL;

	//确保任务当前不在就绪队列
    LOS_ASSERT(!(taskCB->taskStatus & OS_TASK_STATUS_READY));

    processCB = OS_PCB_FROM_PID(taskCB->processID);
    if (!(processCB->processStatus & OS_PROCESS_STATUS_READY)) {
		//如果进程不在就绪队列
        if (((processCB->policy == LOS_SCHED_RR) && (processCB->timeSlice != 0)) || //进程还有剩余时间片
        	//或者正在运行的FIFO进程
            ((processCB->processStatus & OS_PROCESS_STATUS_RUNNING) && (processCB->policy == LOS_SCHED_FIFO))) {
            //直接放入就绪队列头部
            OS_PROCESS_PRI_QUEUE_ENQUEUE_HEAD(processCB);
        } else {
			//其它情况放入就绪队列尾部
            OS_PROCESS_PRI_QUEUE_ENQUEUE(processCB);
        }
		//清除挂起标记以及指定的其它标记
        processCB->processStatus &= ~(status | OS_PROCESS_STATUS_PEND);
		//设置进程为就绪状态
        processCB->processStatus |= OS_PROCESS_STATUS_READY;
    } else {
		//进程已经处于就绪态，则其不应该为阻塞态
        LOS_ASSERT(!(processCB->processStatus & OS_PROCESS_STATUS_PEND));
		//进程应该在就绪队列中
        LOS_ASSERT((UINTPTR)processCB->pendList.pstNext);
        if ((processCB->timeSlice == 0) && (processCB->policy == LOS_SCHED_RR)) {
			//对于时间片已经耗尽的进程，则需要移动到就绪队列尾部
            OS_PROCESS_PRI_QUEUE_DEQUEUE(processCB);
            OS_PROCESS_PRI_QUEUE_ENQUEUE(processCB);
        }
    }

	//将任务放入进程中的就绪队列
    OsSchedTaskEnqueue(processCB, taskCB);
}

//进程放入空闲队列
STATIC INLINE VOID OsInsertPCBToFreeList(LosProcessCB *processCB)
{
    UINT32 pid = processCB->processID;
	//清空数据
    (VOID)memset_s(processCB, sizeof(LosProcessCB), 0, sizeof(LosProcessCB));
    processCB->processID = pid; 
    processCB->processStatus = OS_PROCESS_FLAG_UNUSED;
    processCB->timerID = (timer_t)(UINTPTR)MAX_INVALID_TIMER_VID;
	//放入空闲队列
    LOS_ListTailInsert(&g_freeProcess, &processCB->pendList);
}

//创建进程组
STATIC ProcessGroup *OsCreateProcessGroup(UINT32 pid)
{
    LosProcessCB *processCB = NULL;
	//新建进程组控制块
    ProcessGroup *group = LOS_MemAlloc(m_aucSysMem1, sizeof(ProcessGroup));
    if (group == NULL) {
        return NULL;
    }

    group->groupID = pid;  //设置进程组的ID, 进程组id为进程组中某进程的ID
    LOS_ListInit(&group->processList); //初始化进程组中的进程列表
    LOS_ListInit(&group->exitProcessList); //初始化进程组中已退出进程的列表

    processCB = OS_PCB_FROM_PID(pid);
	//将进程加入进程组
    LOS_ListTailInsert(&group->processList, &processCB->subordinateGroupList);
    processCB->group = group; //记录进程所在的进程组
    processCB->processStatus |= OS_PROCESS_FLAG_GROUP_LEADER; //此进程为本进程组中的首领进程
    if (g_processGroup != NULL) {
		//将进程组存入系统
        LOS_ListTailInsert(&g_processGroup->groupList, &group->groupList);
    }

    return group;
}

//进程退出进程组
STATIC VOID OsExitProcessGroup(LosProcessCB *processCB, ProcessGroup **group)
{
	//获取进程组的首领进程
    LosProcessCB *groupProcessCB = OS_PCB_FROM_PID(processCB->group->groupID);

    LOS_ListDelete(&processCB->subordinateGroupList);  //当前进程退出进程组
    if (LOS_ListEmpty(&processCB->group->processList) && LOS_ListEmpty(&processCB->group->exitProcessList)) {
		//此进程组下已经无进程了，连退出状态的进程都没有了
        LOS_ListDelete(&processCB->group->groupList); //那么这个进程组空了，需要从系统移除进程组
        groupProcessCB->processStatus &= ~OS_PROCESS_FLAG_GROUP_LEADER; //首领进程不再是首领了
        *group = processCB->group; //记录下需要删除的进程组
        if (OsProcessIsUnused(groupProcessCB) && !(groupProcessCB->processStatus & OS_PROCESS_FLAG_EXIT)) {
			//首领进程资源已回收，且其所在的进程组也即将回收
			//那么把这个首领进程的控制块从待回收队列移动到空闲队列
            LOS_ListDelete(&groupProcessCB->pendList);
            OsInsertPCBToFreeList(groupProcessCB);
        }
    }

    processCB->group = NULL;
}

//查询进程组
STATIC ProcessGroup *OsFindProcessGroup(UINT32 gid)
{
    ProcessGroup *group = NULL;
	//进程组列表中第一个进程
    if (g_processGroup->groupID == gid) {
        return g_processGroup;
    }

	//遍历进程组列表
    LOS_DL_LIST_FOR_EACH_ENTRY(group, &g_processGroup->groupList, ProcessGroup, groupList) {
        if (group->groupID == gid) {
            return group;  //成功找到gid对应的进程组
        }
    }

    PRINT_INFO("%s is find group : %u failed!\n", __FUNCTION__, gid);
    return NULL; //查找失败
}

//在进程组中查找处于退出状态的某进程
STATIC LosProcessCB *OsFindGroupExitProcess(ProcessGroup *group, INT32 pid)
{
    LosProcessCB *childCB = NULL;

	//遍历进程组中退出状态进程列表
    LOS_DL_LIST_FOR_EACH_ENTRY(childCB, &(group->exitProcessList), LosProcessCB, subordinateGroupList) {
        if ((childCB->processID == pid) || (pid == OS_INVALID_VALUE)) {
            return childCB; //没有指定pid，则则取第一个退出状态的进程
        }
    }

    PRINT_INFO("%s find exit process : %d failed in group : %u\n", __FUNCTION__, pid, group->groupID);
    return NULL;
}

//在父进程中查询指定的子进程
STATIC UINT32 OsFindChildProcess(const LosProcessCB *processCB, INT32 childPid)
{
    LosProcessCB *childCB = NULL;

    if (childPid < 0) {
        goto ERR;
    }

	//遍历父进程的子进程列表
    LOS_DL_LIST_FOR_EACH_ENTRY(childCB, &(processCB->childrenList), LosProcessCB, siblingList) {
        if (childCB->processID == childPid) {
            return LOS_OK; //子进程存在
        }
    }

ERR:
    PRINT_INFO("%s is find the child : %d failed in parent : %u\n", __FUNCTION__, childPid, processCB->processID);
    return LOS_NOK;  //子进程不存在
}

//在父进程中查询退出状态的子进程
STATIC LosProcessCB *OsFindExitChildProcess(const LosProcessCB *processCB, INT32 childPid)
{
    LosProcessCB *exitChild = NULL;

	//遍历退出状态子进程列表
    LOS_DL_LIST_FOR_EACH_ENTRY(exitChild, &(processCB->exitChildList), LosProcessCB, siblingList) {
        if ((childPid == OS_INVALID_VALUE) || (exitChild->processID == childPid)) {
            return exitChild; //查找到指定的子进程或者取第一个子进程
        }
    }

    PRINT_INFO("%s is find the exit child : %d failed in parent : %u\n", __FUNCTION__, childPid, processCB->processID);
    return NULL;
}

//唤醒指定任务
STATIC INLINE VOID OsWaitWakeTask(LosTaskCB *taskCB, UINT32 wakePID)
{
    taskCB->waitID = wakePID;  //记录需要回收资源的进程ID
    OsTaskWake(taskCB); //唤醒任务
#if (LOSCFG_KERNEL_SMP == YES)
    LOS_MpSchedule(OS_MP_CPU_ALL);
#endif
}

//唤醒等待某具体子进程退出的所有任务
STATIC BOOL OsWaitWakeSpecifiedProcess(LOS_DL_LIST *head, const LosProcessCB *processCB, LOS_DL_LIST **anyList)
{
    LOS_DL_LIST *list = head;
    LosTaskCB *taskCB = NULL;
    UINT32 pid = 0;
    BOOL find = FALSE;

	//遍历正在等待的任务列表
    while (list->pstNext != head) {
        taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(list));
        if ((taskCB->waitFlag == OS_PROCESS_WAIT_PRO) && (taskCB->waitID == processCB->processID)) {
			//如果这个任务就是等待此进程退出的任务
            if (pid == 0) {
				//此进程退出唤醒的第一个任务，这个任务要负责回收此进程的资源
				//所以，我们得把自己的进程ID给他
                pid = processCB->processID;
                find = TRUE;
            } else {
				//此进程退出唤醒的后续任务(不是第一个任务)
				//那么没有必要告诉被唤醒任务，本进程的ID，因为不允许重复回收进程资源，
				//前面if逻辑已经能够引起回收了
                pid = OS_INVALID_VALUE;
            }

            OsWaitWakeTask(taskCB, pid); //唤醒任务
            continue; //继续唤醒后面等待此进程退出的任务
        }

        if (taskCB->waitFlag != OS_PROCESS_WAIT_PRO) {
			// OS_PROCESS_WAIT_PRO 状态的任务排在前面
			//剩余的没有唤醒的任务留着后面再处理
            *anyList = list;
            break;
        }
        list = list->pstNext;
    }

    return find;
}

//唤醒正在父进程上等待的任务，这些任务主要是等待父进程中某些子进程的退出
STATIC VOID OsWaitCheckAndWakeParentProcess(LosProcessCB *parentCB, const LosProcessCB *processCB)
{
    LOS_DL_LIST *head = &parentCB->waitList;
    LOS_DL_LIST *list = NULL;
    LosTaskCB *taskCB = NULL;
    BOOL findSpecified = FALSE;

    if (LOS_ListEmpty(&parentCB->waitList)) {
        return;  //没有任务在父进程下等待，那就不存在需要唤醒的任务
    }

	//由于子进程的退出，唤醒等待此子进程退出的任务列表，可能有多个
	//第1个任务记录此子进程的ID, 其它任务不记录(因为没有必要重复回收子进程资源)
	//剩余还未唤醒的任务记录在list队列中
    findSpecified = OsWaitWakeSpecifiedProcess(head, processCB, &list);
    if (findSpecified == TRUE) {
		//如果子进程退出已经唤醒了具体的等待此子进程的任务
        /* No thread is waiting for any child process to finish */
        if (LOS_ListEmpty(&parentCB->waitList)) {
            return;  //且当前没有任务在等待了，则返回
        } else if (!LOS_ListEmpty(&parentCB->childrenList)) {
            /* Other child processes exist, and other threads that are waiting
             * for the child to finish continue to wait
             */
             //且当前还有任务在等待，但等待的是本父进程下其它子进程的退出
             //那么继续等待即可
            return;
        }
    }

    /* Waiting threads are waiting for a specified child process to finish */
    if (list == NULL) {
        return; //所有等待的任务都是等待某具体子进程的退出
    }
	
    /* No child processes exist and all waiting threads are awakened */
    if (findSpecified == TRUE) {
		//等待某具体子进程退出的那几个任务已经唤醒
		//在父进程上等待的其它任务，由于父进程不再存在子进程，也需要唤醒
		//否则不再存在唤醒机会
        while (list->pstNext != head) {
            taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(list));			
            OsWaitWakeTask(taskCB, OS_INVALID_VALUE); //这个时候，被唤醒的任务没有需要回收的进程资源
        }
        return;
    }

	//等待进程组内的进程退出或者等待任意子进程退出的情况
    while (list->pstNext != head) {
        taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(list));
        if (taskCB->waitFlag == OS_PROCESS_WAIT_GID) {			
			//如果是等待进程组中的进程
            if (taskCB->waitID != processCB->group->groupID) {
                list = list->pstNext; //则需要跳过不匹配的进程组
                continue;
            }
        }
		//找到一个匹配的子进程(进程组内的子进程或者任意子进程)
        if (findSpecified == FALSE) {
			//被唤醒的线程还要负责回收此进程的资源，所以这个时候要传递进程id
            OsWaitWakeTask(taskCB, processCB->processID);
            findSpecified = TRUE; //后续就不需要重复回收了
        } else {
			//后续就不需要重复回收了
            OsWaitWakeTask(taskCB, OS_INVALID_VALUE);  
        }

        if (!LOS_ListEmpty(&parentCB->childrenList)) {
            break; //如果父进程中还有其它子进程，那么其它子进程再来唤醒剩下的任务吧
        }
		//如果父进程空了，那么所有等待的任务都需要唤醒
    }

    return;
}


//释放进程占用的一些资源
LITE_OS_SEC_TEXT VOID OsProcessResourcesToFree(LosProcessCB *processCB)
{
    if (!(processCB->processStatus & (OS_PROCESS_STATUS_INIT | OS_PROCESS_STATUS_RUNNING))) {
		//只允许释放初始状态的进程资源或者正在运行状态的进程资源
		//当释放正在运行的进程状态资源后，进程很快就会执行退出的一些逻辑
        PRINT_ERR("The process(%d) has no permission to release process(%d) resources!\n",
                  OsCurrProcessGet()->processID, processCB->processID);
    }

#ifdef LOSCFG_FS_VFS
    if (OsProcessIsUserMode(processCB)) {
		//用户态进程才需要通过文件形式和内核交互
		//释放打开的文件描述符
        delete_files(processCB, processCB->files);
    }
    processCB->files = NULL;
#endif

#ifdef LOSCFG_SECURITY_CAPABILITY
    if (processCB->user != NULL) {
		//释放进程所属的用户和组信息
        (VOID)LOS_MemFree(m_aucSysMem1, processCB->user);
        processCB->user = NULL;
    }
#endif

	//释放进程下的定时器资源
    OsSwtmrRecycle(processCB->processID);
    processCB->timerID = (timer_t)(UINTPTR)MAX_INVALID_TIMER_VID;

#ifdef LOSCFG_SECURITY_VID
    if (processCB->timerIdMap.bitMap != NULL) {
		//TBD
        VidMapDestroy(processCB);
        processCB->timerIdMap.bitMap = NULL;
    }
#endif

#if (LOSCFG_KERNEL_LITEIPC == YES)
    if (OsProcessIsUserMode(processCB)) {
		//释放用户间进程通信占用的资源
        LiteIpcPoolDelete(&(processCB->ipcInfo));
        (VOID)memset_s(&(processCB->ipcInfo), sizeof(ProcIpcInfo), 0, sizeof(ProcIpcInfo));
    }
#endif
}


//回收僵尸进程
LITE_OS_SEC_TEXT STATIC VOID OsRecycleZombiesProcess(LosProcessCB *childCB, ProcessGroup **group)
{
    OsExitProcessGroup(childCB, group);  //先让僵尸进程退出进程组
    LOS_ListDelete(&childCB->siblingList); //让僵尸进程退出僵尸进程列表
    if (childCB->processStatus & OS_PROCESS_STATUS_ZOMBIES) {
		//修订进程状态为空闲
        childCB->processStatus &= ~OS_PROCESS_STATUS_ZOMBIES;
        childCB->processStatus |= OS_PROCESS_FLAG_UNUSED;
    }

	//先退出待回收队列
    LOS_ListDelete(&childCB->pendList);
    if (childCB->processStatus & OS_PROCESS_FLAG_EXIT) {
		//正在退出的进程，重新加入待回收队列
        LOS_ListHeadInsert(&g_processRecyleList, &childCB->pendList);
    } else if (childCB->processStatus & OS_PROCESS_FLAG_GROUP_LEADER) {
    	//进程组首领进程重新加入待回收队列
        LOS_ListTailInsert(&g_processRecyleList, &childCB->pendList);
    } else {
		//其他状态则加入空闲队列
        OsInsertPCBToFreeList(childCB);
    }
}


//为当前还活着的子进程重新找父进程
STATIC VOID OsDealAliveChildProcess(LosProcessCB *processCB)
{
    UINT32 parentID;
    LosProcessCB *childCB = NULL;
    LosProcessCB *parentCB = NULL;
    LOS_DL_LIST *nextList = NULL;
    LOS_DL_LIST *childHead = NULL;

	//存在活动子进程的情况下
    if (!LOS_ListEmpty(&processCB->childrenList)) {
		//找出第一个子进程
        childHead = processCB->childrenList.pstNext;
		//子进程队列脱离本进程，形成孤儿进程队列
        LOS_ListDelete(&(processCB->childrenList));  
        if (OsProcessIsUserMode(processCB)) {
			//如果是用户态进程，那么这些孤儿进程由init收养
            parentID = g_userInitProcess;
        } else {
        	//如果是内核态进程，那么这些孤儿进程由KProcess收养
            parentID = g_kernelInitProcess;
        }

		//遍历所有的孤儿进程
        for (nextList = childHead; ;) {
            childCB = OS_PCB_FROM_SIBLIST(nextList);
			//为这些孤儿进程重新制定父进程
            childCB->parentProcessID = parentID;
            nextList = nextList->pstNext;
            if (nextList == childHead) {
                break;
            }
        }

		//孤儿进程队列直接纳入新父进程怀抱
        parentCB = OS_PCB_FROM_PID(parentID);
        LOS_ListTailInsertList(&parentCB->childrenList, childHead);
    }

    return;
}

//回收已处于退出状态的子进程的资源
STATIC VOID OsChildProcessResourcesFree(const LosProcessCB *processCB)
{
    LosProcessCB *childCB = NULL;
    ProcessGroup *group = NULL;

	//遍历所有处于退出状态的子进程
    while (!LOS_ListEmpty(&((LosProcessCB *)processCB)->exitChildList)) {
        childCB = LOS_DL_LIST_ENTRY(processCB->exitChildList.pstNext, LosProcessCB, siblingList);
        OsRecycleZombiesProcess(childCB, &group);  //回收它们的资源
        (VOID)LOS_MemFree(m_aucSysMem1, group); //如果产生了空的进程组，则释放
    }
}

//处理一个进程的正常退出
STATIC VOID OsProcessNaturalExit(LosTaskCB *runTask, UINT32 status)
{
    LosProcessCB *processCB = OS_PCB_FROM_PID(runTask->processID);
    LosProcessCB *parentCB = NULL;

    LOS_ASSERT(!(processCB->threadScheduleMap != 0));
    LOS_ASSERT(processCB->processStatus & OS_PROCESS_STATUS_RUNNING);

	//先释放已退出状态的子进程的资源
    OsChildProcessResourcesFree(processCB);

#ifdef LOSCFG_KERNEL_CPUP
    OsCpupClean(processCB->processID); //清空本进程的运行时间统计值
#endif

    /* is a child process */
    if (processCB->parentProcessID != OS_INVALID_VALUE) {
		//本进程有父进程
        parentCB = OS_PCB_FROM_PID(processCB->parentProcessID);
        LOS_ListDelete(&processCB->siblingList);  //离开父进程怀抱
        if (!OsProcessExitCodeSignalIsSet(processCB)) { //没有退出信号需要处理的话
            OsProcessExitCodeSet(processCB, status); //设置进程的退出代码
        }
		//本进程放入父进程的已退出子进程列表
        LOS_ListTailInsert(&parentCB->exitChildList, &processCB->siblingList);
        LOS_ListDelete(&processCB->subordinateGroupList); //本进程退出进程组
        //并加入进程组的已退出进程列表
        LOS_ListTailInsert(&processCB->group->exitProcessList, &processCB->subordinateGroupList);

		//尝试唤醒在父进程上等待的任务(那些任务就是在等待子进程的退出)
        OsWaitCheckAndWakeParentProcess(parentCB, processCB);

		//将剩余未退出的子进程(孤儿进程)重新绑定新的父进程
        OsDealAliveChildProcess(processCB);

		//当前进程设置成僵尸进程
        processCB->processStatus |= OS_PROCESS_STATUS_ZOMBIES;

		//通知父进程我已退出
        (VOID)OsKill(processCB->parentProcessID, SIGCHLD, OS_KERNEL_KILL_PERMISSION);
		//本进程放入待回收队列
        LOS_ListHeadInsert(&g_processRecyleList, &processCB->pendList);
        OsRunTaskToDelete(runTask);  //删除当前运行任务
        return;
    }

    LOS_Panic("pid : %u is the root process exit!\n", processCB->processID);
    return;
}

//初始化进程管理
LITE_OS_SEC_TEXT_INIT UINT32 OsProcessInit(VOID)
{
    UINT32 index;
    UINT32 size;

	//支持的最大进程数
    g_processMaxNum = LOSCFG_BASE_CORE_PROCESS_LIMIT;
	//所有进程控制块占用的尺寸
    size = g_processMaxNum * sizeof(LosProcessCB);

	//新建进程控制块数组
    g_processCBArray = (LosProcessCB *)LOS_MemAlloc(m_aucSysMem1, size);
    if (g_processCBArray == NULL) {
        return LOS_NOK;
    }
    (VOID)memset_s(g_processCBArray, size, 0, size);

	//初始化空闲进程控制块队列
    LOS_ListInit(&g_freeProcess);
	//初始化待回收进程控制块队列
    LOS_ListInit(&g_processRecyleList);

	//初始化每一个进程控制块
    for (index = 0; index < g_processMaxNum; index++) {
		//每一个进程控制块的ID为其在数组中的下标编号
        g_processCBArray[index].processID = index;
		//初始状态每个进程都是空闲的
        g_processCBArray[index].processStatus = OS_PROCESS_FLAG_UNUSED;
		//初始状态每个进程都存入空闲队列
        LOS_ListTailInsert(&g_freeProcess, &g_processCBArray[index].pendList);
    }

	//先保留1号进程做为用户态init进程
    g_userInitProcess = 1; /* 1: The root process ID of the user-mode process is fixed at 1 */
    LOS_ListDelete(&g_processCBArray[g_userInitProcess].pendList); 

	//先保留2号进程做为内核态init进程(KProcess进程)
    g_kernelInitProcess = 2; /* 2: The root process ID of the kernel-mode process is fixed at 2 */
    LOS_ListDelete(&g_processCBArray[g_kernelInitProcess].pendList);

    return LOS_OK;
}

//创建Idle进程
STATIC UINT32 OsCreateIdleProcess(VOID)
{
    UINT32 ret;
    CHAR *idleName = "Idle";
    LosProcessCB *idleProcess = NULL;
    Percpu *perCpu = OsPercpuGet();
	//每个CPU都有一个Idle任务
    UINT32 *idleTaskID = &perCpu->idleTaskID;

	//先创建一个资源回收线程
    ret = OsCreateResourceFreeTask();
    if (ret != LOS_OK) {
        return ret;
    }

	//基于KProcess进程fork一个KIdle进程
    INT32 processId = LOS_Fork(CLONE_FILES, "KIdle", (TSK_ENTRY_FUNC)OsIdleTask, LOSCFG_BASE_CORE_TSK_IDLE_STACK_SIZE);
    if (processId < 0) {
        return LOS_NOK;
    }
	//记录下KIdle进程的ID
    g_kernelIdleProcess = (UINT32)processId;

	//idle进程控制块
    idleProcess = OS_PCB_FROM_PID(g_kernelIdleProcess);
	//记录idle进程中的主线程，即idle线程ID
    *idleTaskID = idleProcess->threadGroupID;
	//标记idle线程为系统线程
    OS_TCB_FROM_TID(*idleTaskID)->taskStatus |= OS_TASK_FLAG_SYSTEM_TASK;
#if (LOSCFG_KERNEL_SMP == YES)
	//本线程绑定在创建此线程的核上运行
    OS_TCB_FROM_TID(*idleTaskID)->cpuAffiMask = CPUID_TO_AFFI_MASK(ArchCurrCpuid());
#endif
	//设置idle线程的名称
    return (UINT32)OsSetTaskName(OS_TCB_FROM_TID(*idleTaskID), idleName, FALSE);
}


//批量处理待回收进程
LITE_OS_SEC_TEXT VOID OsProcessCBRecyleToFree(VOID)
{
    UINT32 intSave;
    LosVmSpace *space = NULL;
    LosProcessCB *processCB = NULL;

    SCHEDULER_LOCK(intSave);
	//遍历待回收进程队列
    while (!LOS_ListEmpty(&g_processRecyleList)) {
        processCB = OS_PCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_processRecyleList));
        if (!(processCB->processStatus & OS_PROCESS_FLAG_EXIT)) {
            break;  //只处理正在退出的进程
        }
        SCHEDULER_UNLOCK(intSave);

        OsTaskCBRecyleToFree();  //释放待回收线程的资源

        SCHEDULER_LOCK(intSave);
		//清除进程正在退出标志
        processCB->processStatus &= ~OS_PROCESS_FLAG_EXIT;
        if (OsProcessIsUserMode(processCB)) {
			//对于用户态进程，备份下内存地址空间信息
            space = processCB->vmSpace;
        }
        processCB->vmSpace = NULL; //内存地址空间置空
        /* OS_PROCESS_FLAG_GROUP_LEADER: The lead process group cannot be recycled without destroying the PCB.
         * !OS_PROCESS_FLAG_UNUSED: Parent process does not reclaim child process resources.
         */
        LOS_ListDelete(&processCB->pendList); //先从待回收队列移除
        if ((processCB->processStatus & OS_PROCESS_FLAG_GROUP_LEADER) || //由其它驱动回收首领进程
            (processCB->processStatus & OS_PROCESS_STATUS_ZOMBIES)) {    //由其它驱动回收僵尸进程
            LOS_ListTailInsert(&g_processRecyleList, &processCB->pendList); //所以，上述进程需要重新放入待回收队列
        } else {
            /* Clear the bottom 4 bits of process status */
            OsInsertPCBToFreeList(processCB);   //回收进程控制块，即放入空闲队列
        }
        SCHEDULER_UNLOCK(intSave);

        (VOID)LOS_VmSpaceFree(space);  //释放进程地址空间

        SCHEDULER_LOCK(intSave);
    }

    SCHEDULER_UNLOCK(intSave);
}

//获取一个空闲的进程
STATIC LosProcessCB *OsGetFreePCB(VOID)
{
    LosProcessCB *processCB = NULL;
    UINT32 intSave;

    SCHEDULER_LOCK(intSave);
    if (LOS_ListEmpty(&g_freeProcess)) {
        SCHEDULER_UNLOCK(intSave); //进程控制块已经耗尽了
        PRINT_ERR("No idle PCB in the system!\n");
        return NULL;
    }

	//取第一个空闲进程
    processCB = OS_PCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&g_freeProcess));
    LOS_ListDelete(&processCB->pendList);
    SCHEDULER_UNLOCK(intSave);

    return processCB;  //返回空闲进程
}

//初始化进程控制块过程中的错误处理
STATIC VOID OsDeInitPCB(LosProcessCB *processCB)
{
    UINT32 intSave;
    ProcessGroup *group = NULL;

    if (processCB == NULL) {
        return;
    }

	//释放进程控制块中的已有资源
    OsProcessResourcesToFree(processCB);

    SCHEDULER_LOCK(intSave);
    if (processCB->parentProcessID != OS_INVALID_VALUE) {
		//从父进程移除
        LOS_ListDelete(&processCB->siblingList);
        processCB->parentProcessID = OS_INVALID_VALUE;
    }

    if (processCB->group != NULL) {
		//从进程组移除
        OsExitProcessGroup(processCB, &group);
    }

	//从初始状态切换到退出状态
    processCB->processStatus &= ~OS_PROCESS_STATUS_INIT;
    processCB->processStatus |= OS_PROCESS_FLAG_EXIT;
	//放入待回收队列
    LOS_ListHeadInsert(&g_processRecyleList, &processCB->pendList);
    SCHEDULER_UNLOCK(intSave);

    (VOID)LOS_MemFree(m_aucSysMem1, group); //释放进程组
    OsWriteResourceEvent(OS_RESOURCE_EVENT_FREE); //通知资源回收线程来回收控制块
    return;
}


//设置进程的名称
UINT32 OsSetProcessName(LosProcessCB *processCB, const CHAR *name)
{
    errno_t errRet;

    if (processCB == NULL) {
        return LOS_EINVAL;
    }

    if (name != NULL) {
		//直接记录下用户设置的进程名称
        errRet = strncpy_s(processCB->processName, OS_PCB_NAME_LEN, name, OS_PCB_NAME_LEN - 1);
        if (errRet == EOK) {
            return LOS_OK;
        }
    }

	//用户没有设置进程名称，则我们根据ID生成进程名称
    switch (processCB->processMode) {
        case OS_KERNEL_MODE:
			//内核态进程生成进程名称的方法
            errRet = snprintf_s(processCB->processName, OS_PCB_NAME_LEN, OS_PCB_NAME_LEN - 1,
                                "KerProcess%u", processCB->processID);
            break;
        default:
			//用户态进程生成进程名称的方法
            errRet = snprintf_s(processCB->processName, OS_PCB_NAME_LEN, OS_PCB_NAME_LEN - 1,
                                "UserProcess%u", processCB->processID);
            break;
    }

    if (errRet < 0) {
        return LOS_NOK;
    }
    return LOS_OK;
}

//初始化进程控制块
STATIC UINT32 OsInitPCB(LosProcessCB *processCB, UINT32 mode, UINT16 priority, UINT16 policy, const CHAR *name)
{
    UINT32 count;
    LosVmSpace *space = NULL;
    LosVmPage *vmPage = NULL;
    status_t status;
    BOOL retVal = FALSE;

    processCB->processMode = mode;  //内核or用户空间进程
    processCB->processStatus = OS_PROCESS_STATUS_INIT; //初始化状态
    processCB->parentProcessID = OS_INVALID_VALUE; //现在还无父进程
    processCB->threadGroupID = OS_INVALID_VALUE; //现在还无主线程
    processCB->priority = priority;  //进程的优先级
    processCB->policy = policy; //进程的调度策略
    processCB->umask = OS_PROCESS_DEFAULT_UMASK;  //TBD
    processCB->timerID = (timer_t)(UINTPTR)MAX_INVALID_TIMER_VID; //TBD

    LOS_ListInit(&processCB->threadSiblingList); //进程下的线程列表
    LOS_ListInit(&processCB->childrenList); //子进程列表
    LOS_ListInit(&processCB->exitChildList); //退出状态的子进程列表
    LOS_ListInit(&(processCB->waitList));  //等待本进程的子进程退出的任务列表

    for (count = 0; count < OS_PRIORITY_QUEUE_NUM; ++count) {
		//进程中的任务调度队列
        LOS_ListInit(&processCB->threadPriQueueList[count]);
    }

    if (OsProcessIsUserMode(processCB)) {
		//用户空间进程
		//地址空间描述符
        space = LOS_MemAlloc(m_aucSysMem0, sizeof(LosVmSpace));
        if (space == NULL) {
            PRINT_ERR("Init process struct, alloc space memory failed!\n");
            return LOS_ENOMEM;
        }
		//申请一个物理页来存储地址转换表
        VADDR_T *ttb = LOS_PhysPagesAllocContiguous(1);
        if (ttb == NULL) {
            PRINT_ERR("Init process struct, alloc ttb failed!\n");
            (VOID)LOS_MemFree(m_aucSysMem0, space);
            return LOS_ENOMEM;
        }
        (VOID)memset_s(ttb, PAGE_SIZE, 0, PAGE_SIZE);
		//初始化地址转换表
        retVal = OsUserVmSpaceInit(space, ttb);
		//物理页描述符
        vmPage = OsVmVaddrToPage(ttb);
        if ((retVal == FALSE) || (vmPage == NULL)) {
            PRINT_ERR("Init process struct, create space failed!\n");
            processCB->processStatus = OS_PROCESS_FLAG_UNUSED;
            (VOID)LOS_MemFree(m_aucSysMem0, space);
            LOS_PhysPagesFreeContiguous(ttb, 1);
            return LOS_EAGAIN;
        }
		//在进程记录下地址空间
        processCB->vmSpace = space;
		//内存页加入地址空间
        LOS_ListAdd(&processCB->vmSpace->archMmu.ptList, &(vmPage->node));
    } else {
		//内核态进程共享内核地址空间
        processCB->vmSpace = LOS_GetKVmSpace();
    }

#ifdef LOSCFG_SECURITY_VID
    status = VidMapListInit(processCB);
    if (status != LOS_OK) {
        return LOS_ENOMEM;
    }
#endif
#ifdef LOSCFG_SECURITY_CAPABILITY
    OsInitCapability(processCB);
#endif

	//设置进程名称
    if (OsSetProcessName(processCB, name) != LOS_OK) {
        return LOS_ENOMEM;
    }

    return LOS_OK;
}

#ifdef LOSCFG_SECURITY_CAPABILITY
//创建用户
STATIC User *OsCreateUser(UINT32 userID, UINT32 gid, UINT32 size)
{
	//新建用户
    User *user = LOS_MemAlloc(m_aucSysMem1, sizeof(User) + (size - 1) * sizeof(UINT32));
    if (user == NULL) {
        return NULL;
    }

	//记录用户ID和组ID
    user->userID = userID;
    user->effUserID = userID;
    user->gid = gid;
    user->effGid = gid;
    user->groupNumber = size;
    user->groups[0] = gid;
    return user;
}

//判断用户是否在组内
LITE_OS_SEC_TEXT BOOL LOS_CheckInGroups(UINT32 gid)
{
    UINT32 intSave;
    UINT32 count;
    User *user = NULL;

    SCHEDULER_LOCK(intSave);
    user = OsCurrUserGet();  //获取当前用户
    //遍历当前用户的所在的组
    for (count = 0; count < user->groupNumber; count++) {
        if (user->groups[count] == gid) { //判断组ID是否匹配
            SCHEDULER_UNLOCK(intSave);
            return TRUE;  //用户在指定组中
        }
    }

    SCHEDULER_UNLOCK(intSave);
    return FALSE; //用户不在组中
}
#endif

//获取当前用户的ID
LITE_OS_SEC_TEXT INT32 LOS_GetUserID(VOID)
{
#ifdef LOSCFG_SECURITY_CAPABILITY
    UINT32 intSave;
    INT32 uid;

    SCHEDULER_LOCK(intSave);
    uid = (INT32)OsCurrUserGet()->userID;
    SCHEDULER_UNLOCK(intSave);
    return uid;
#else
    return 0;
#endif
}

//获取当前用户的组ID
LITE_OS_SEC_TEXT INT32 LOS_GetGroupID(VOID)
{
#ifdef LOSCFG_SECURITY_CAPABILITY
    UINT32 intSave;
    INT32 gid;

    SCHEDULER_LOCK(intSave);
    gid = (INT32)OsCurrUserGet()->gid;
    SCHEDULER_UNLOCK(intSave);

    return gid;
#else
    return 0;
#endif
}

//内核和用户态init进程的初始化
STATIC UINT32 OsProcessCreateInit(LosProcessCB *processCB, UINT32 flags, const CHAR *name, UINT16 priority)
{
    ProcessGroup *group = NULL;
    UINT32 ret = OsInitPCB(processCB, flags, priority, LOS_SCHED_RR, name);
    if (ret != LOS_OK) {
        goto EXIT;
    }

#if (LOSCFG_KERNEL_LITEIPC == YES)
    if (OsProcessIsUserMode(processCB)) {
		//用户态进程创建进程间通信需要的内核资源
        ret = LiteIpcPoolInit(&(processCB->ipcInfo));
        if (ret != LOS_OK) {
            ret = LOS_ENOMEM;
            goto EXIT;
        }
    }
#endif

#ifdef LOSCFG_FS_VFS
	//分配打开的文件描述符表
    processCB->files = alloc_files();
    if (processCB->files == NULL) {
        ret = LOS_ENOMEM;
        goto EXIT;
    }
#endif

	//创建进程组，传入首领进程编号
    group = OsCreateProcessGroup(processCB->processID);
    if (group == NULL) {
        ret = LOS_ENOMEM;
        goto EXIT;
    }

#ifdef LOSCFG_SECURITY_CAPABILITY
	//创建进程的用户和组ID都为0
    processCB->user = OsCreateUser(0, 0, 1);
    if (processCB->user == NULL) {
        ret = LOS_ENOMEM;
        goto EXIT;
    }
#endif

#ifdef LOSCFG_KERNEL_CPUP
	//进程运行统计
    OsCpupSet(processCB->processID);
#endif

    return LOS_OK;

EXIT:
    OsDeInitPCB(processCB);
    return ret;
}

//内核态Init进程的初始化
LITE_OS_SEC_TEXT_INIT UINT32 OsKernelInitProcess(VOID)
{
    LosProcessCB *processCB = NULL;
    UINT32 ret;

	//初始化进程管理模块
    ret = OsProcessInit();
    if (ret != LOS_OK) {
        return ret;
    }

	//获取init进程控制块
    processCB = OS_PCB_FROM_PID(g_kernelInitProcess);
	//初始化init进程，并取名KProcess,最高调度优先级
    ret = OsProcessCreateInit(processCB, OS_KERNEL_MODE, "KProcess", 0);
    if (ret != LOS_OK) {
        return ret;
    }

	//进程已初始化完成
    processCB->processStatus &= ~OS_PROCESS_STATUS_INIT;
	//将本进程的进程组加入系统
    g_processGroup = processCB->group;
    LOS_ListInit(&g_processGroup->groupList);
    OsCurrProcessSet(processCB); //设置成当前正运行的进程

    return OsCreateIdleProcess(); //再创建Idle进程
}

//进程让出CPU资源
LITE_OS_SEC_TEXT UINT32 LOS_ProcessYield(VOID)
{
    UINT32 count;
    UINT32 intSave;
    LosProcessCB *runProcessCB = NULL;

    if (OS_INT_ACTIVE) {
		//中断上下文不允许切换进程和任务
        return LOS_ERRNO_TSK_YIELD_IN_INT;
    }

    if (!OsPreemptable()) {
		//任务调度临时关闭的情况
        return LOS_ERRNO_TSK_YIELD_IN_LOCK;
    }

    SCHEDULER_LOCK(intSave);
	//本进程
    runProcessCB = OsCurrProcessGet();

    /* reset timeslice of yeilded task */
    runProcessCB->timeSlice = 0;  //清空本进程的剩余时间片

	//当前进程同优先级就绪队列长度
    count = OS_PROCESS_PRI_QUEUE_SIZE(runProcessCB);
    if (count > 0) {
		//如果还有同优先级就绪进程，那么应该让它来运行
        if (runProcessCB->processStatus & OS_PROCESS_STATUS_READY) {
			//如果本进程在就绪队列，那么先移出就绪队列
            OS_PROCESS_PRI_QUEUE_DEQUEUE(runProcessCB);
        }
		//本进程再次进入就绪队列尾部
        OS_PROCESS_PRI_QUEUE_ENQUEUE(runProcessCB);
		//设置成就绪状态
        runProcessCB->processStatus |= OS_PROCESS_STATUS_READY;
		//当前任务也进入进程的就绪队列
        OsSchedTaskEnqueue(runProcessCB, OsCurrTaskGet());
    } else {
		//没有就绪的进程可以调度，那么补放弃CPU资源
        SCHEDULER_UNLOCK(intSave);
        return LOS_OK;
    }

	//放弃CPU资源，让另外的进程来执行
    OsSchedResched();
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

//进程调度参数检查
STATIC INLINE INT32 OsProcessSchedlerParamCheck(INT32 which, INT32 pid, UINT16 prio, UINT16 policy)
{
    if (OS_PID_CHECK_INVALID(pid)) {
        return LOS_EINVAL;
    }

    if (which != LOS_PRIO_PROCESS) {
        return LOS_EOPNOTSUPP;  
    }

    if (prio > OS_PROCESS_PRIORITY_LOWEST) {
        return LOS_EINVAL;
    }

    if ((policy != LOS_SCHED_FIFO) && (policy != LOS_SCHED_RR)) {
        return LOS_EOPNOTSUPP;
    }

    return LOS_OK;
}

#ifdef LOSCFG_SECURITY_CAPABILITY
//优先级修订权限检查
STATIC BOOL OsProcessCapPermitCheck(const LosProcessCB *processCB, UINT16 prio)
{
    LosProcessCB *runProcess = OsCurrProcessGet();

    /* always trust kernel process */
    if (!OsProcessIsUserMode(runProcess)) {
        return TRUE; //内核态进程拥有充分的权限
    }

    /* user mode process can reduce the priority of itself */
    if ((runProcess->processID == processCB->processID) && (prio > processCB->priority)) {
        return TRUE;  //进程可以调低自己的优先级
    }

    /* user mode process with privilege of CAP_SCHED_SETPRIORITY can change the priority */
    if (IsCapPermit(CAP_SCHED_SETPRIORITY)) {
        return TRUE;  //具有调整优先级的权限
    }
    return FALSE;
}
#endif


//设置进程调度策略和优先级
LITE_OS_SEC_TEXT INT32 OsSetProcessScheduler(INT32 which, INT32 pid, UINT16 prio, UINT16 policy, BOOL policyFlag)
{
    LosProcessCB *processCB = NULL;
    UINT32 intSave;
    INT32 ret;

	//检查参数
    ret = OsProcessSchedlerParamCheck(which, pid, prio, policy);
    if (ret != LOS_OK) {
        return -ret;
    }

    SCHEDULER_LOCK(intSave);
    processCB = OS_PCB_FROM_PID(pid);
    if (OsProcessIsInactive(processCB)) {
        ret = LOS_ESRCH;  //退出状态的进程
        goto EXIT;
    }

#ifdef LOSCFG_SECURITY_CAPABILITY
    if (!OsProcessCapPermitCheck(processCB, prio)) {
        ret = LOS_EPERM; //无权限配置的情况
        goto EXIT;
    }
#endif

    if (policyFlag == TRUE) {
		//调整调度策略
        if (policy == LOS_SCHED_FIFO) {
			//只有轮转策略才需要时间片
            processCB->timeSlice = 0;
        }
        processCB->policy = policy;  //调整策略
    }

    if (processCB->processStatus & OS_PROCESS_STATUS_READY) {
		//就绪进程，需要根据优先级重新入队
        OS_PROCESS_PRI_QUEUE_DEQUEUE(processCB);
        processCB->priority = prio;
        OS_PROCESS_PRI_QUEUE_ENQUEUE(processCB);
    } else {
		//不在就绪队列中，可以直接改优先级
        processCB->priority = prio;
        if (!(processCB->processStatus & OS_PROCESS_STATUS_RUNNING)) {
            ret = LOS_OK; //不是当前正在运行的进程，则直接返回
            goto EXIT;
        }
    }

    SCHEDULER_UNLOCK(intSave);

    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (OS_SCHEDULER_ACTIVE) {
        LOS_Schedule(); //当前运行的进程优先级或者调度策略调整后
        //可能需要重新选择进程来运行
    }
    return LOS_OK;

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return -ret;
}

//调整进程的调度策略和优先级
LITE_OS_SEC_TEXT INT32 LOS_SetProcessScheduler(INT32 pid, UINT16 policy, UINT16 prio)
{
    return OsSetProcessScheduler(LOS_PRIO_PROCESS, pid, prio, policy, TRUE);
}

//获取进程的调度策略
LITE_OS_SEC_TEXT INT32 LOS_GetProcessScheduler(INT32 pid)
{
    LosProcessCB *processCB = NULL;
    UINT32 intSave;
    INT32 policy;

    if (OS_PID_CHECK_INVALID(pid)) {
        return -LOS_EINVAL;
    }

    SCHEDULER_LOCK(intSave);
    processCB = OS_PCB_FROM_PID(pid);
    if (OsProcessIsUnused(processCB)) {
        policy = -LOS_ESRCH;
        goto OUT;
    }

    policy = processCB->policy; //调度策略

OUT:
    SCHEDULER_UNLOCK(intSave);
    return policy;
}

//设置优先级
LITE_OS_SEC_TEXT INT32 LOS_SetProcessPriority(INT32 pid, UINT16 prio)
{
	//只设置优先级
    return OsSetProcessScheduler(LOS_PRIO_PROCESS, pid, prio, LOS_SCHED_RR, FALSE);
}

//获取进程的优先级
LITE_OS_SEC_TEXT INT32 OsGetProcessPriority(INT32 which, INT32 pid)
{
    LosProcessCB *processCB = NULL;
    INT32 prio;
    UINT32 intSave;
    (VOID)which;

    if (OS_PID_CHECK_INVALID(pid)) {
        return -LOS_EINVAL;
    }

    if (which != LOS_PRIO_PROCESS) {
        return -LOS_EOPNOTSUPP;
    }

    SCHEDULER_LOCK(intSave);
    processCB = OS_PCB_FROM_PID(pid);
    if (OsProcessIsUnused(processCB)) {
        prio = -LOS_ESRCH;
        goto OUT;
    }

    prio = (INT32)processCB->priority; //优先级

OUT:
    SCHEDULER_UNLOCK(intSave);
    return prio;
}

//获取进程优先级
LITE_OS_SEC_TEXT INT32 LOS_GetProcessPriority(INT32 pid)
{
    return OsGetProcessPriority(LOS_PRIO_PROCESS, pid);
}

//唤醒指定进程下等待的一个任务
LITE_OS_SEC_TEXT VOID OsWaitSignalToWakeProcess(LosProcessCB *processCB)
{
    LosTaskCB *taskCB = NULL;

    if (processCB == NULL) {
        return;
    }

    /* only suspend process can continue */
    if (!(processCB->processStatus & OS_PROCESS_STATUS_PEND)) {
        return;  //进程不在挂起状态，不用唤醒
    }

    if (!LOS_ListEmpty(&processCB->waitList)) {
		//唤醒在等待此进程的第一个任务
        taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(&processCB->waitList));
        OsWaitWakeTask(taskCB, OS_INVALID_VALUE);
    }

    return;
}


//当前任务等待processCB的子进程的结束
STATIC VOID OsWaitInsertWaitListInOrder(LosTaskCB *runTask, LosProcessCB *processCB)
{
    LOS_DL_LIST *head = &processCB->waitList;
    LOS_DL_LIST *list = head;
    LosTaskCB *taskCB = NULL;

	//放入等待队列末尾，不实际等待，不超时，并设置合适的状态
    (VOID)OsTaskWait(&processCB->waitList, LOS_WAIT_FOREVER, FALSE);
	//然后从队列取下来，重新按序放入
    LOS_ListDelete(&runTask->pendList);
    if (runTask->waitFlag == OS_PROCESS_WAIT_PRO) {
		//如果是等待一个具体的子进程，放入队列头部
        LOS_ListHeadInsert(&processCB->waitList, &runTask->pendList);
        return;
    } else if (runTask->waitFlag == OS_PROCESS_WAIT_GID) {
		//如果是等待进程组中的进程
        while (list->pstNext != head) {
            taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(list));
            if (taskCB->waitFlag == OS_PROCESS_WAIT_PRO) {
                list = list->pstNext;
                continue;  //则跳过所有的OS_PROCESS_WAIT_PRO任务
            }
            break;
        }
		//此任务插入到所有OS_PROCESS_WAIT_PRO的末尾
        LOS_ListHeadInsert(list, &runTask->pendList);
        return;
    }

	//如果是等待任何一个子进程退出
    while (list->pstNext != head) {
        taskCB = OS_TCB_FROM_PENDLIST(LOS_DL_LIST_FIRST(list));
        if (taskCB->waitFlag != OS_PROCESS_WAIT_ANY) {
            list = list->pstNext; //跳过所有不是OS_PROCESS_WAIT_ANY的任务
            continue;
        }
        break;
    }

	//最终所有等待任务按waitflag进行了排序，PRO最前面，GROUP中间，ANY最后
    LOS_ListHeadInsert(list, &runTask->pendList);
    return;
}

//当前进程等待pid标识的子进程退出，返回成功等到的子进程*child
STATIC UINT32 OsWaitSetFlag(const LosProcessCB *processCB, INT32 pid, LosProcessCB **child)
{
    LosProcessCB *childCB = NULL;
    ProcessGroup *group = NULL;
    LosTaskCB *runTask = OsCurrTaskGet();
    UINT32 ret;

    if (pid > 0) {
        /* Wait for the child process whose process number is pid. */
		//对应的子进程存在，且处于退出状态
        childCB = OsFindExitChildProcess(processCB, pid);
        if (childCB != NULL) {
            goto WAIT_BACK;  //成功等到
        }

		//查找正常状态子进程
        ret = OsFindChildProcess(processCB, pid);
        if (ret != LOS_OK) {
            return LOS_ECHILD;  //不存在这个子进程
        }
		//存在这个子进程，但我们需要等待它结束
        runTask->waitFlag = OS_PROCESS_WAIT_PRO; //记录等待标记
        runTask->waitID = pid; //以及等待的进程号
    } else if (pid == 0) {
		//等待同进程组下的子进程的结束
        /* Wait for any child process in the same process group */
		//查找本进程组中是否存在退出状态的进程
        childCB = OsFindGroupExitProcess(processCB->group, OS_INVALID_VALUE);
        if (childCB != NULL) {
            goto WAIT_BACK;  //同进程组中，有进程退出，那么补用等待
        }
		//否则等待这个进程组内的进程退出
        runTask->waitID = processCB->group->groupID;  //等待这个进程组
        runTask->waitFlag = OS_PROCESS_WAIT_GID;  //等待进程组的标记
    } else if (pid == -1) {
		//等待任意的子进程退出
        /* Wait for any child process */
		//查询是否有退出状态的子进程
        childCB = OsFindExitChildProcess(processCB, OS_INVALID_VALUE);
        if (childCB != NULL) {
            goto WAIT_BACK;  //确实有，不用再等待
        }
		//否则等待任意子进程的退出
        runTask->waitID = pid;
        runTask->waitFlag = OS_PROCESS_WAIT_ANY;  //等待任意子进程的标记
    } else { /* pid < -1 */
        /* Wait for any child process whose group number is the pid absolute value. */
		//等待-pid进程组内的进程的退出
        group = OsFindProcessGroup(-pid);
        if (group == NULL) {
            return LOS_ECHILD; //没有这样的进程组
        }

		//在进程组内寻找处于退出状态的进程
        childCB = OsFindGroupExitProcess(group, OS_INVALID_VALUE);
        if (childCB != NULL) {
            goto WAIT_BACK;  //有进程退出，则不再等待
        }

		//否则等待这个进程组下的进程退出
        runTask->waitID = -pid;  //记录进程组号
        runTask->waitFlag = OS_PROCESS_WAIT_GID;  //等待进程组
    }

WAIT_BACK:
    *child = childCB;
    return LOS_OK;
}

STATIC UINT32 OsWaitRecycleChildPorcess(const LosProcessCB *childCB, UINT32 intSave, INT32 *status)
{
    ProcessGroup *group = NULL;
    UINT32 pid = childCB->processID;
    UINT16 mode = childCB->processMode;
    INT32 exitCode = childCB->exitCode;

	//回收僵尸进程的资源
    OsRecycleZombiesProcess((LosProcessCB *)childCB, &group);
    SCHEDULER_UNLOCK(intSave);

    if (status != NULL) {
		//记录进程的返回值
        if (mode == OS_USER_MODE) {
			//返回值拷贝到用户空间
            (VOID)LOS_ArchCopyToUser((VOID *)status, (const VOID *)(&(exitCode)), sizeof(INT32));
        } else {
        	//内核中记录返回值
            *status = exitCode;
        }
    }

	//释放由于僵尸进程回收而需要删除的进程组
    (VOID)LOS_MemFree(m_aucSysMem1, group);
    return pid; //返回已回收的僵尸进程ID
}

STATIC UINT32 OsWaitChildProcessCheck(LosProcessCB *processCB, INT32 pid, LosProcessCB **childCB)
{
    if (LOS_ListEmpty(&(processCB->childrenList)) && LOS_ListEmpty(&(processCB->exitChildList))) {
        return LOS_ECHILD;  //当前进程没有子进程，那么不用等
    }

	//继续等待，并设置等待标记
    return OsWaitSetFlag(processCB, pid, childCB);
}

STATIC UINT32 OsWaitOptionsCheck(UINT32 options)
{
    UINT32 flag = LOS_WAIT_WNOHANG | LOS_WAIT_WUNTRACED | LOS_WAIT_WCONTINUED;

    flag = ~flag & options;
    if (flag != 0) {
        return LOS_EINVAL;  //只允许flag对应的3个参数
    }

    if ((options & (LOS_WAIT_WCONTINUED | LOS_WAIT_WUNTRACED)) != 0) {
        return LOS_EOPNOTSUPP;  //目前暂时只支持 LOS_WAIT_WNOHANG 参数
    }

    if (OS_INT_ACTIVE) {
        return LOS_EINTR;  //中断上下文不能调用这个函数
    }

    return LOS_OK;
}

LITE_OS_SEC_TEXT INT32 LOS_Wait(INT32 pid, USER INT32 *status, UINT32 options, VOID *rusage)
{
    (VOID)rusage;
    UINT32 ret;
    UINT32 intSave;
    LosProcessCB *childCB = NULL;
    LosProcessCB *processCB = NULL;
    LosTaskCB *runTask = NULL;

	//检查options参数
    ret = OsWaitOptionsCheck(options);
    if (ret != LOS_OK) {
        return -ret;
    }

    SCHEDULER_LOCK(intSave);
    processCB = OsCurrProcessGet();
    runTask = OsCurrTaskGet();

	//当前进程等pid标识的子进程退出，返回已经成功等到的子进程
    ret = OsWaitChildProcessCheck(processCB, pid, &childCB);
    if (ret != LOS_OK) {
        pid = -ret;
        goto ERROR;
    }

    if (childCB != NULL) {
		//如果已经存在处于退出状态的进程，则回收其资源
        return (INT32)OsWaitRecycleChildPorcess(childCB, intSave, status);
    }

	//当前无满足要求的子进程处于退出状态，则需要等待
    if ((options & LOS_WAIT_WNOHANG) != 0) {
		//不允许挂起等待的情况下，只能返回错误
        runTask->waitFlag = 0;
        pid = 0;
        goto ERROR;
    }

	//老实等待当前进程的子进程的退出吧，排队等待
    OsWaitInsertWaitListInOrder(runTask, processCB);

    OsSchedResched();  //切换其他任务运行

	//等待的子进程退出了
    runTask->waitFlag = 0;
    if (runTask->waitID == OS_INVALID_VALUE) {
        pid = -LOS_ECHILD;   //这个子进程被其它任务抢先回收了
        goto ERROR;
    }

	//需要回收资源的子进程
    childCB = OS_PCB_FROM_PID(runTask->waitID);
    if (!(childCB->processStatus & OS_PROCESS_STATUS_ZOMBIES)) {
        pid = -LOS_ESRCH;  //这个时候这个进程理论上应该是僵尸状态
        goto ERROR;
    }

	//回收这个僵尸进程的资源吧
    return (INT32)OsWaitRecycleChildPorcess(childCB, intSave, status);

ERROR:
    SCHEDULER_UNLOCK(intSave);
    return pid;
}

//设置进程组合法性检查
STATIC UINT32 OsSetProcessGroupCheck(const LosProcessCB *processCB, UINT32 gid)
{
    LosProcessCB *runProcessCB = OsCurrProcessGet();
    LosProcessCB *groupProcessCB = OS_PCB_FROM_PID(gid);

    if (OsProcessIsInactive(processCB)) {
        return LOS_ESRCH;  //退出状态的进程不能设置进程组
    }

    if (!OsProcessIsUserMode(processCB) || !OsProcessIsUserMode(groupProcessCB)) {
        return LOS_EPERM; //用户态进程才能加入进程组
    }

    if (runProcessCB->processID == processCB->parentProcessID) {
		//当前进程的子进程
        if (processCB->processStatus & OS_PROCESS_FLAG_ALREADY_EXEC) {  
            return LOS_EACCES;   //已经执行了exec调用后，不能再修订进程组，应该由子进程自己修订
        }
    } else if (processCB->processID != runProcessCB->processID) {
        return LOS_ESRCH;  //除了自己，没有权限修订其它进程的进程组属性
    }

    /* Add the process to another existing process group */
    if (processCB->processID != gid) {
		//我不是首领进程
        if (!(groupProcessCB->processStatus & OS_PROCESS_FLAG_GROUP_LEADER)) {
            return LOS_EPERM; //对方也不是首领进程
        }

		//首领进程不是我的兄弟进程，也不是我的父进程
        if ((groupProcessCB->parentProcessID != processCB->parentProcessID) && (gid != processCB->parentProcessID)) {
            return LOS_EPERM;
        }
    }

    return LOS_OK;
}

//设置进程的进程组
STATIC UINT32 OsSetProcessGroupIDUnsafe(UINT32 pid, UINT32 gid, ProcessGroup **group)
{
    ProcessGroup *oldGroup = NULL;
    ProcessGroup *newGroup = NULL;
    LosProcessCB *processCB = OS_PCB_FROM_PID(pid);
	//先检查参数
    UINT32 ret = OsSetProcessGroupCheck(processCB, gid);
    if (ret != LOS_OK) {
        return ret;
    }

    if (processCB->group->groupID == gid) {
        return LOS_OK;  //进程已经属于这个进程组
    }

    oldGroup = processCB->group; //记录进程原来的进程组
    OsExitProcessGroup(processCB, group); //先退出原来的进程组

    newGroup = OsFindProcessGroup(gid);  //查找需要加入的进程组
    if (newGroup != NULL) {
		//新进程组存在
		//将本进程加入新进程组
        LOS_ListTailInsert(&newGroup->processList, &processCB->subordinateGroupList);
        processCB->group = newGroup; //刷新进程的进程组
        return LOS_OK;
    }

	//创建新进程组并加入首个进程
    newGroup = OsCreateProcessGroup(gid);
    if (newGroup == NULL) {
		//新进程组创建失败，则进程回到旧的进程组
        LOS_ListTailInsert(&oldGroup->processList, &processCB->subordinateGroupList);
        processCB->group = oldGroup; //恢复旧进程组
        if (*group != NULL) {
			//旧进程组回到系统进程组列表中
            LOS_ListTailInsert(&g_processGroup->groupList, &oldGroup->groupList);
            processCB = OS_PCB_FROM_PID(oldGroup->groupID);
            processCB->processStatus |= OS_PROCESS_FLAG_GROUP_LEADER; //恢复旧进程组的首领进程
            *group = NULL;
        }
        return LOS_EPERM;
    }
    return LOS_OK;
}

//设置进程的进程组
LITE_OS_SEC_TEXT INT32 OsSetProcessGroupID(UINT32 pid, UINT32 gid)
{
    ProcessGroup *group = NULL;
    UINT32 ret;
    UINT32 intSave;

    if ((OS_PID_CHECK_INVALID(pid)) || (OS_PID_CHECK_INVALID(gid))) {
        return -LOS_EINVAL;
    }

    SCHEDULER_LOCK(intSave);
    ret = OsSetProcessGroupIDUnsafe(pid, gid, &group);
    SCHEDULER_UNLOCK(intSave);
    (VOID)LOS_MemFree(m_aucSysMem1, group); //删除原来的进程组
    return -ret;
}

//设置当前进程的进程组
LITE_OS_SEC_TEXT INT32 OsSetCurrProcessGroupID(UINT32 gid)
{
    return OsSetProcessGroupID(OsCurrProcessGet()->processID, gid);
}

//获取指定进程的进程组
LITE_OS_SEC_TEXT INT32 LOS_GetProcessGroupID(UINT32 pid)
{
    INT32 gid;
    UINT32 intSave;
    LosProcessCB *processCB = NULL;

    if (OS_PID_CHECK_INVALID(pid)) {
        return -LOS_EINVAL;
    }

    SCHEDULER_LOCK(intSave);
    processCB = OS_PCB_FROM_PID(pid);
    if (OsProcessIsUnused(processCB)) {
        gid = -LOS_ESRCH;
        goto EXIT;
    }

    gid = (INT32)processCB->group->groupID;

EXIT:
    SCHEDULER_UNLOCK(intSave);
    return gid;
}

//获取当前进程的进程组
LITE_OS_SEC_TEXT INT32 LOS_GetCurrProcessGroupID(VOID)
{
    return LOS_GetProcessGroupID(OsCurrProcessGet()->processID);
}

//初始化init进程的用户态栈空间
STATIC VOID *OsUserInitStackAlloc(LosProcessCB *processCB, UINT32 *size)
{
    LosVmMapRegion *region = NULL;
    UINT32 stackSize = ALIGN(OS_USER_TASK_STACK_SIZE, PAGE_SIZE);

	//分配栈内存区，虚拟地址空间
    region = LOS_RegionAlloc(processCB->vmSpace, 0, stackSize,
                             VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ |
                             VM_MAP_REGION_FLAG_PERM_WRITE, 0);
    if (region == NULL) {
        return NULL;
    }

    LOS_SetRegionTypeAnon(region); //本段内存区不涉及文件
    region->regionFlags |= VM_MAP_REGION_FLAG_STACK; //用于栈空间

    *size = stackSize;  //使用的内存空间的尺寸

    return (VOID *)(UINTPTR)region->range.base; //返回内存空间首地址(虚拟地址)
}

LITE_OS_SEC_TEXT UINT32 OsExecRecycleAndInit(LosProcessCB *processCB, const CHAR *name,
                                             LosVmSpace *oldSpace, UINTPTR oldFiles)
{
    UINT32 ret;
    const CHAR *processName = NULL;

    if ((processCB == NULL) || (name == NULL)) {
        return LOS_NOK;
    }

	//从name中分离出/后面的字符串，如果没有/字符，那么就是整个name
	//做为进程的名称
    processName = strrchr(name, '/');
    processName = (processName == NULL) ? name : (processName + 1); /* 1: Do not include '/' */

	//同时做为主任务和进程的名称
    ret = (UINT32)OsSetTaskName(OsCurrTaskGet(), processName, TRUE);
    if (ret != LOS_OK) {
        return ret;
    }

#if (LOSCFG_KERNEL_LITEIPC == YES)
	//为进程申请IPC通信需要用到的资源
    ret = LiteIpcPoolInit(&(processCB->ipcInfo));
    if (ret != LOS_OK) {
        return LOS_NOK;
    }
#endif

    processCB->sigHandler = 0; //暂时不指定信号处理函数
    OsCurrTaskGet()->sig.sigprocmask = 0; //暂时不设置信号处理

#ifdef LOSCFG_FS_VFS
	//删除旧的文件描述符表
    delete_files(OsCurrProcessGet(), (struct files_struct *)oldFiles);
#endif

	//回收之前进程遗留的软件定时器
    OsSwtmrRecycle(processCB->processID);
    processCB->timerID = (timer_t)(UINTPTR)MAX_INVALID_TIMER_VID;

#ifdef LOSCFG_SECURITY_VID
    VidMapDestroy(processCB);
    ret = VidMapListInit(processCB);
    if (ret != LOS_OK) {
        return LOS_NOK;
    }
#endif

	//清除进程退出状态
    processCB->processStatus &= ~OS_PROCESS_FLAG_EXIT;
	//并设置进程已运行exec()状态
    processCB->processStatus |= OS_PROCESS_FLAG_ALREADY_EXEC;

    LOS_VmSpaceFree(oldSpace); //释放从父进程克隆的地址空间
    return LOS_OK;
}

//在新进程中执行新的程序
LITE_OS_SEC_TEXT UINT32 OsExecStart(const TSK_ENTRY_FUNC entry, UINTPTR sp, UINTPTR mapBase, UINT32 mapSize)
{
    LosTaskCB *taskCB = NULL;
    TaskContext *taskContext = NULL;
    UINT32 intSave;

    if (entry == NULL) {
        return LOS_NOK;
    }

    if ((sp == 0) || (LOS_Align(sp, LOSCFG_STACK_POINT_ALIGN_SIZE) != sp)) {
        return LOS_NOK;
    }

    if ((mapBase == 0) || (mapSize == 0) || (sp <= mapBase) || (sp > (mapBase + mapSize))) {
        return LOS_NOK;
    }

    SCHEDULER_LOCK(intSave);
    taskCB = OsCurrTaskGet();

    taskCB->userMapBase = mapBase;  //当前任务的栈空间
    taskCB->userMapSize = mapSize;  //当前任务的栈尺寸
    taskCB->taskEntry = (TSK_ENTRY_FUNC)entry; //当前任务的入口函数

	//初始化栈中的任务上下文
    taskContext = (TaskContext *)OsTaskStackInit(taskCB->taskID, taskCB->stackSize, (VOID *)taskCB->topOfStack, FALSE);
    OsUserTaskStackInit(taskContext, taskCB->taskEntry, sp);
    SCHEDULER_UNLOCK(intSave);
    return LOS_OK;
}

//启动init进程
STATIC UINT32 OsUserInitProcessStart(UINT32 processID, TSK_INIT_PARAM_S *param)
{
    UINT32 intSave;
    UINT32 taskID;
    INT32 ret;

	//创建用户态任务
    taskID = OsCreateUserTask(processID, param);
    if (taskID == OS_INVALID_VALUE) {
        return LOS_NOK;
    }

	//设置调度策略和优先级
    ret = LOS_SetTaskScheduler(taskID, LOS_SCHED_RR, OS_TASK_PRIORITY_LOWEST);
    if (ret != LOS_OK) {
        PRINT_ERR("User init process set scheduler failed! ERROR:%d \n", ret);
        SCHEDULER_LOCK(intSave);
        (VOID)OsTaskDeleteUnsafe(OS_TCB_FROM_TID(taskID), OS_PRO_EXIT_OK, intSave);
        return LOS_NOK;
    }

    return LOS_OK;
}

//加载用户态init代码
STATIC UINT32 OsLoadUserInit(LosProcessCB *processCB)
{
    /*              userInitTextStart               -----
     * | user text |
     *
     * | user data |                                initSize
     *              userInitBssStart  ---
     * | user bss  |                  initBssSize
     *              userInitEnd       ---           -----
     */
    errno_t errRet;
    INT32 ret;
    CHAR *userInitTextStart = (CHAR *)&__user_init_entry;
    CHAR *userInitBssStart = (CHAR *)&__user_init_bss;
    CHAR *userInitEnd = (CHAR *)&__user_init_end;
    UINT32 initBssSize = userInitEnd - userInitBssStart;
    UINT32 initSize = userInitEnd - userInitTextStart;
    VOID *userBss = NULL;
    VOID *userText = NULL;

    if ((LOS_Align((UINTPTR)userInitTextStart, PAGE_SIZE) != (UINTPTR)userInitTextStart) ||
        (LOS_Align((UINTPTR)userInitEnd, PAGE_SIZE) != (UINTPTR)userInitEnd)) {
        return LOS_EINVAL;
    }

    if ((initSize == 0) || (initSize <= initBssSize)) {
        return LOS_EINVAL;
    }

	//先申请足够的物理内存用来存放代码和数据
    userText = LOS_PhysPagesAllocContiguous(initSize >> PAGE_SHIFT);
    if (userText == NULL) {
        return LOS_NOK;
    }

	//先将代码和全局数据加载到内存
    errRet = memcpy_s(userText, initSize, (VOID *)&__user_init_load_addr, initSize - initBssSize);
    if (errRet != EOK) {
        PRINT_ERR("Load user init text, data and bss failed! err : %d\n", errRet);
        goto ERROR;
    }
	//将本端物理内存映射到进程的用户空间内存区中
    ret = LOS_VaddrToPaddrMmap(processCB->vmSpace, (VADDR_T)(UINTPTR)userInitTextStart, LOS_PaddrQuery(userText),
                               initSize, VM_MAP_REGION_FLAG_PERM_READ | VM_MAP_REGION_FLAG_PERM_WRITE |
                               VM_MAP_REGION_FLAG_PERM_EXECUTE | VM_MAP_REGION_FLAG_PERM_USER);
    if (ret < 0) {
        PRINT_ERR("Mmap user init text, data and bss failed! err : %d\n", ret);
        goto ERROR;
    }

    /* The User init boot segment may not actually exist */
    if (initBssSize != 0) {
		//将bss数据段清0
        userBss = (VOID *)((UINTPTR)userText + userInitBssStart - userInitTextStart);
        errRet = memset_s(userBss, initBssSize, 0, initBssSize);
        if (errRet != EOK) {
            PRINT_ERR("memset user init bss failed! err : %d\n", errRet);
            goto ERROR;
        }
    }

    return LOS_OK;

ERROR:
    (VOID)LOS_PhysPagesFreeContiguous(userText, initSize >> PAGE_SHIFT);
    return LOS_NOK;
}

//创建用户态init进程
LITE_OS_SEC_TEXT_INIT UINT32 OsUserInitProcess(VOID)
{
    UINT32 ret;
    UINT32 size;
    TSK_INIT_PARAM_S param = { 0 };
    VOID *stack = NULL;

	//用户态init进程描述符
    LosProcessCB *processCB = OS_PCB_FROM_PID(g_userInitProcess);
	//用户态，init进程，优先级28
    ret = OsProcessCreateInit(processCB, OS_USER_MODE, "Init", OS_PROCESS_USERINIT_PRIORITY);
    if (ret != LOS_OK) {
        return ret;
    }

	//加载init进程代码和数据
    ret = OsLoadUserInit(processCB);
    if (ret != LOS_OK) {
        goto ERROR;
    }

	//初始化init进程中的任务栈
    stack = OsUserInitStackAlloc(processCB, &size);
    if (stack == NULL) {
        PRINT_ERR("Alloc user init process user stack failed!\n");
        goto ERROR;
    }

    param.pfnTaskEntry = (TSK_ENTRY_FUNC)(CHAR *)&__user_init_entry;
    param.userParam.userSP = (UINTPTR)stack + size;
    param.userParam.userMapBase = (UINTPTR)stack;
    param.userParam.userMapSize = size;
    param.uwResved = OS_TASK_FLAG_PTHREAD_JOIN;
	//启动init进程
    ret = OsUserInitProcessStart(g_userInitProcess, &param);
    if (ret != LOS_OK) {
        (VOID)OsUnMMap(processCB->vmSpace, param.userParam.userMapBase, param.userParam.userMapSize);
        goto ERROR;
    }

    return LOS_OK;

ERROR:
    OsDeInitPCB(processCB);
    return ret;
}

//拷贝父进程的用户信息到子进程
STATIC UINT32 OsCopyUser(LosProcessCB *childCB, LosProcessCB *parentCB)
{
#ifdef LOSCFG_SECURITY_CAPABILITY
	//计算用户信息尺寸
    UINT32 size = sizeof(User) + sizeof(UINT32) * (parentCB->user->groupNumber - 1);
    childCB->user = LOS_MemAlloc(m_aucSysMem1, size); //新建用户信息结构
    if (childCB->user == NULL) {
        return LOS_ENOMEM;
    }

	//拷贝用户信息数据
    (VOID)memcpy_s(childCB->user, size, parentCB->user, size);
#endif
    return LOS_OK;
}

//初始化拷贝任务时的参数
STATIC VOID OsInitCopyTaskParam(LosProcessCB *childProcessCB, const CHAR *name, UINTPTR entry, UINT32 size,
                                TSK_INIT_PARAM_S *childPara)
{
    LosTaskCB *mainThread = NULL;
    UINT32 intSave;

    SCHEDULER_LOCK(intSave);
    mainThread = OsCurrTaskGet(); //当前线程为主线程，进程拷贝时只拷贝主线程

    if (OsProcessIsUserMode(childProcessCB)) {
		//用户态进程拷贝
		//任务入口拷贝
		//参考主线程
        childPara->pfnTaskEntry = mainThread->taskEntry;
        childPara->uwStackSize = mainThread->stackSize; //栈尺寸
        childPara->userParam.userArea = mainThread->userArea; //TLS, 线程本地存储
        childPara->userParam.userMapBase = mainThread->userMapBase; //用户栈起始地址
        childPara->userParam.userMapSize = mainThread->userMapSize; //用户栈尺寸
    } else {
        childPara->pfnTaskEntry = (TSK_ENTRY_FUNC)entry; //内核线程入口
        childPara->uwStackSize = size; //内核栈尺寸
    }
    childPara->pcName = (CHAR *)name;  //线程名
    childPara->policy = mainThread->policy;  //同主线程调度策略
    childPara->usTaskPrio = mainThread->priority;  //同主线程优先级
    childPara->processID = childProcessCB->processID; //子进程ID
    if (mainThread->taskStatus & OS_TASK_FLAG_PTHREAD_JOIN) {
        childPara->uwResved = OS_TASK_FLAG_PTHREAD_JOIN; //由其它线程来回收资源
    } else if (mainThread->taskStatus & OS_TASK_FLAG_DETACHED) {
        childPara->uwResved = OS_TASK_FLAG_DETACHED; //自己触发后台线程回收资源
    }

    SCHEDULER_UNLOCK(intSave);
}


//fork调用时的任务拷贝
STATIC UINT32 OsCopyTask(UINT32 flags, LosProcessCB *childProcessCB, const CHAR *name, UINTPTR entry, UINT32 size)
{
    LosTaskCB *childTaskCB = NULL;
    TSK_INIT_PARAM_S childPara = { 0 };
    UINT32 ret;
    UINT32 intSave;
    UINT32 taskID;

	//先初始化任务拷贝需要的参数
    OsInitCopyTaskParam(childProcessCB, name, entry, size, &childPara);

	//根据参数创建任务
    ret = LOS_TaskCreateOnly(&taskID, &childPara);
    if (ret != LOS_OK) {
        if (ret == LOS_ERRNO_TSK_TCB_UNAVAILABLE) {
            return LOS_EAGAIN;
        }
        return LOS_ENOMEM;
    }

	//子进程中的主线程
    childTaskCB = OS_TCB_FROM_TID(taskID);
    childTaskCB->taskStatus = OsCurrTaskGet()->taskStatus;  //复制当前线程状态
    if (childTaskCB->taskStatus & OS_TASK_STATUS_RUNNING) {
		//需要清除正在运行状态，因为现在还没有调度它执行
        childTaskCB->taskStatus &= ~OS_TASK_STATUS_RUNNING;
    } else {
    	//正常情况不应该走到这个逻辑
        if (OS_SCHEDULER_ACTIVE) {
            LOS_Panic("Clone thread status not running error status: 0x%x\n", childTaskCB->taskStatus);
        }
        childTaskCB->taskStatus &= ~OS_TASK_STATUS_UNUSED;
		//嫌疑进程，将其优先级降低到最低，方便诊断问题，同时不让其影响其它进程的运行
        childProcessCB->priority = OS_PROCESS_PRIORITY_LOWEST;
    }

    if (OsProcessIsUserMode(childProcessCB)) {
        SCHEDULER_LOCK(intSave);
		//用户态任务还要克隆任务栈
        OsUserCloneParentStack(childTaskCB, OsCurrTaskGet());
        SCHEDULER_UNLOCK(intSave);
    }
	//将子进程中的主任务放入子进程的就绪队列中
    OS_TASK_PRI_QUEUE_ENQUEUE(childProcessCB, childTaskCB);
    childTaskCB->taskStatus |= OS_TASK_STATUS_READY; //并设置子进程的主任务为就绪状态
    return LOS_OK;
}

//拷贝父进程的信息到子进程中
STATIC UINT32 OsCopyParent(UINT32 flags, LosProcessCB *childProcessCB, LosProcessCB *runProcessCB)
{
    UINT32 ret;
    UINT32 intSave;
    LosProcessCB *parentProcessCB = NULL;

    SCHEDULER_LOCK(intSave);
	//拷贝优先级和调度策略
    childProcessCB->priority = runProcessCB->priority;
    childProcessCB->policy = runProcessCB->policy;

    if (flags & CLONE_PARENT) {
		//本进程与新进程为兄弟关系
        parentProcessCB = OS_PCB_FROM_PID(runProcessCB->parentProcessID);		
        childProcessCB->parentProcessID = parentProcessCB->processID;
	
        LOS_ListTailInsert(&parentProcessCB->childrenList, &childProcessCB->siblingList);
        childProcessCB->group = parentProcessCB->group;
        LOS_ListTailInsert(&parentProcessCB->group->processList, &childProcessCB->subordinateGroupList);
        ret = OsCopyUser(childProcessCB, parentProcessCB);
    } else {
		//本进程与新进程为父子关系
		
        childProcessCB->parentProcessID = runProcessCB->processID;
		
        LOS_ListTailInsert(&runProcessCB->childrenList, &childProcessCB->siblingList);
        childProcessCB->group = runProcessCB->group;
        LOS_ListTailInsert(&runProcessCB->group->processList, &childProcessCB->subordinateGroupList);
        ret = OsCopyUser(childProcessCB, runProcessCB);
    }
    SCHEDULER_UNLOCK(intSave);
    return ret;
}

//从父进程拷贝内存空间
STATIC UINT32 OsCopyMM(UINT32 flags, LosProcessCB *childProcessCB, LosProcessCB *runProcessCB)
{
    status_t status;
    UINT32 intSave;

    if (!OsProcessIsUserMode(childProcessCB)) {
        return LOS_OK;  //内核态进程大家共享同一个内存空间
    }

    if (flags & CLONE_VM) {
        SCHEDULER_LOCK(intSave);
		//与父进程共享内存空间的情况
		//共享地址映射表
        childProcessCB->vmSpace->archMmu.virtTtb = runProcessCB->vmSpace->archMmu.virtTtb;
        childProcessCB->vmSpace->archMmu.physTtb = runProcessCB->vmSpace->archMmu.physTtb;
        SCHEDULER_UNLOCK(intSave);
        return LOS_OK;
    }

	//父子进程独立的地址空间，则克隆父进程的地址空间到子进程
    status = LOS_VmSpaceClone(runProcessCB->vmSpace, childProcessCB->vmSpace);
    if (status != LOS_OK) {
        return LOS_ENOMEM;
    }
    return LOS_OK;
}

//拷贝父进程文件描述符表
STATIC UINT32 OsCopyFile(UINT32 flags, LosProcessCB *childProcessCB, LosProcessCB *runProcessCB)
{
#ifdef LOSCFG_FS_VFS
    if (flags & CLONE_FILES) {
		//与父进程共享文件描述符表
        childProcessCB->files = runProcessCB->files;
    } else {
    	//克隆父进程的文件描述符表
        childProcessCB->files = dup_fd(runProcessCB->files);
    }
    if (childProcessCB->files == NULL) {
        return LOS_ENOMEM;
    }
#endif

    childProcessCB->consoleID = runProcessCB->consoleID;
    childProcessCB->umask = runProcessCB->umask;
    return LOS_OK;
}

//克隆父进程时初始化子进程描述符
STATIC UINT32 OsForkInitPCB(UINT32 flags, LosProcessCB *child, const CHAR *name, UINTPTR sp, UINT32 size)
{
    UINT32 ret;
    LosProcessCB *run = OsCurrProcessGet();

	//初始化子进程描述符
    ret = OsInitPCB(child, run->processMode, OS_PROCESS_PRIORITY_LOWEST, LOS_SCHED_RR, name);
    if (ret != LOS_OK) {
        return ret;
    }

	//拷贝父进程描述符
    ret = OsCopyParent(flags, child, run);
    if (ret != LOS_OK) {
        return ret;
    }

	//创建子进程的主线程
    return OsCopyTask(flags, child, name, sp, size);
}

//子进程设置进程组并参与调度
STATIC UINT32 OsChildSetProcessGroupAndSched(LosProcessCB *child, LosProcessCB *run)
{
    UINT32 intSave;
    UINT32 ret;
    ProcessGroup *group = NULL;

    SCHEDULER_LOCK(intSave);
    if (run->group->groupID == OS_USER_PRIVILEGE_PROCESS_GROUP) {
		//通过子进程创建新的进程组以及首领进程
        ret = OsSetProcessGroupIDUnsafe(child->processID, child->processID, &group);
        if (ret != LOS_OK) {
            SCHEDULER_UNLOCK(intSave);
            return LOS_ENOMEM;
        }
    }

	//子进程加入调度队列
    OS_PROCESS_PRI_QUEUE_ENQUEUE(child);
    child->processStatus &= ~OS_PROCESS_STATUS_INIT;
    child->processStatus |= OS_PROCESS_STATUS_READY;

#ifdef LOSCFG_KERNEL_CPUP
    OsCpupSet(child->processID); //子进程运行时间监控使能
#endif
    SCHEDULER_UNLOCK(intSave);

    (VOID)LOS_MemFree(m_aucSysMem1, group); //释放子进程原来所在的进程组
    return LOS_OK;
}

//拷贝父进程资源
STATIC UINT32 OsCopyProcessResources(UINT32 flags, LosProcessCB *child, LosProcessCB *run)
{
    UINT32 ret;

	//拷贝内存地址空间
    ret = OsCopyMM(flags, child, run);
    if (ret != LOS_OK) {
        return ret;
    }

	//拷贝文件描述符表
    ret = OsCopyFile(flags, child, run);
    if (ret != LOS_OK) {
        return ret;
    }

#if (LOSCFG_KERNEL_LITEIPC == YES)
	//重新初始化子进程的IPC通信资源
    if (OsProcessIsUserMode(child)) {
        ret = LiteIpcPoolReInit(&child->ipcInfo, (const ProcIpcInfo *)(&run->ipcInfo));
        if (ret != LOS_OK) {
            return LOS_ENOMEM;
        }
    }
#endif

#ifdef LOSCFG_SECURITY_CAPABILITY
    OsCopyCapability(run, child);
#endif

    return LOS_OK;
}

//拷贝进程生成新进程
STATIC INT32 OsCopyProcess(UINT32 flags, const CHAR *name, UINTPTR sp, UINT32 size)
{
    UINT32 intSave, ret, processID;
    LosProcessCB *run = OsCurrProcessGet();

	//新建子进程
    LosProcessCB *child = OsGetFreePCB();
    if (child == NULL) {
        return -LOS_EAGAIN;
    }
    processID = child->processID;

	//初始化新建的子进程
    ret = OsForkInitPCB(flags, child, name, sp, size);
    if (ret != LOS_OK) {
        goto ERROR_INIT;
    }

	//从父进程处拷贝资源
    ret = OsCopyProcessResources(flags, child, run);
    if (ret != LOS_OK) {
        goto ERROR_TASK;
    }

	//设置子进程的进程组并运行子进程调度
    ret = OsChildSetProcessGroupAndSched(child, run);
    if (ret != LOS_OK) {
        goto ERROR_TASK;
    }

    LOS_MpSchedule(OS_MP_CPU_ALL);
    if (OS_SCHEDULER_ACTIVE) {
        LOS_Schedule();  //调度，让子进程有机会运行
    }

    return processID;

ERROR_TASK:
    SCHEDULER_LOCK(intSave);
    (VOID)OsTaskDeleteUnsafe(OS_TCB_FROM_TID(child->threadGroupID), OS_PRO_EXIT_OK, intSave);
ERROR_INIT:
    OsDeInitPCB(child);
    return -ret;
}

//克隆子进程
LITE_OS_SEC_TEXT INT32 OsClone(UINT32 flags, UINTPTR sp, UINT32 size)
{
    UINT32 cloneFlag = CLONE_PARENT | CLONE_THREAD | CLONE_VFORK | CLONE_VM;

    if (flags & (~cloneFlag)) {
        PRINT_WARN("Clone dont support some flags!\n");
    }

    return OsCopyProcess(cloneFlag & flags, NULL, sp, size);
}

//fork子进程
LITE_OS_SEC_TEXT INT32 LOS_Fork(UINT32 flags, const CHAR *name, const TSK_ENTRY_FUNC entry, UINT32 stackSize)
{
    UINT32 cloneFlag = CLONE_PARENT | CLONE_THREAD | CLONE_VFORK | CLONE_FILES;

    if (flags & (~cloneFlag)) {
        PRINT_WARN("Clone dont support some flags!\n");
    }

    flags |= CLONE_FILES;
    return OsCopyProcess(cloneFlag & flags, name, (UINTPTR)entry, stackSize);
}

//获取当前进程ID
LITE_OS_SEC_TEXT UINT32 LOS_GetCurrProcessID(VOID)
{
    return OsCurrProcessGet()->processID;
}

//进程退出的处理
LITE_OS_SEC_TEXT VOID OsProcessExit(LosTaskCB *runTask, INT32 status)
{
    UINT32 intSave;
    LOS_ASSERT(runTask == OsCurrTaskGet());

	//释放当前任务的资源
    OsTaskResourcesToFree(runTask);
	//释放当前进程的资源
    OsProcessResourcesToFree(OsCurrProcessGet());

    SCHEDULER_LOCK(intSave);
	//处理进程的退出逻辑
    OsProcessNaturalExit(runTask, status);
    SCHEDULER_UNLOCK(intSave);
}

//进程退出
LITE_OS_SEC_TEXT VOID LOS_Exit(INT32 status)
{
	//先退出进程组
    OsTaskExitGroup((UINT32)status);
	//然后处理进程退出
    OsProcessExit(OsCurrTaskGet(), (UINT32)status);
}

//获取init进程的ID
LITE_OS_SEC_TEXT UINT32 OsGetUserInitProcessID(VOID)
{
    return g_userInitProcess;
}

//获取idle进程的ID
LITE_OS_SEC_TEXT UINT32 OsGetIdleProcessID(VOID)
{
    return g_kernelIdleProcess;
}

//获取KProcess进程的ID
LITE_OS_SEC_TEXT UINT32 OsGetKernelInitProcessID(VOID)
{
    return g_kernelInitProcess;
}

//设置信号处理函数
LITE_OS_SEC_TEXT VOID OsSetSigHandler(UINTPTR addr)
{
    OsCurrProcessGet()->sigHandler = addr;
}

//获取信号处理函数
LITE_OS_SEC_TEXT UINTPTR OsGetSigHandler(VOID)
{
    return OsCurrProcessGet()->sigHandler;
}
#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif
