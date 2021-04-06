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

#include "los_swtmr_pri.h"
#include "los_sortlink_pri.h"
#include "los_queue_pri.h"
#include "los_task_pri.h"
#include "los_process_pri.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#if (LOSCFG_BASE_CORE_SWTMR == YES)
#if (LOSCFG_BASE_CORE_SWTMR_LIMIT <= 0)
#error "swtmr maxnum cannot be zero"
#endif /* LOSCFG_BASE_CORE_SWTMR_LIMIT <= 0 */

//软件定时器控制块数组
LITE_OS_SEC_BSS SWTMR_CTRL_S    *g_swtmrCBArray = NULL;     /* First address in Timer memory space */
//软件定时器handler池
LITE_OS_SEC_BSS UINT8           *g_swtmrHandlerPool = NULL; /* Pool of Swtmr Handler */
//空闲的软件定时器队列
LITE_OS_SEC_BSS LOS_DL_LIST     g_swtmrFreeList;            /* Free list of Software Timer */

/* spinlock for swtmr module, only available on SMP mode */
LITE_OS_SEC_BSS  SPIN_LOCK_INIT(g_swtmrSpin);
#define SWTMR_LOCK(state)       LOS_SpinLockSave(&g_swtmrSpin, &(state))
#define SWTMR_UNLOCK(state)     LOS_SpinUnlockRestore(&g_swtmrSpin, (state))

//软件定时器任务，用于读取超时事件，并具体处理超时对应的业务逻辑
//因为中断上下文不能阻塞，所以只能放到任务中来处理
LITE_OS_SEC_TEXT VOID OsSwtmrTask(VOID)
{
    SwtmrHandlerItemPtr swtmrHandlePtr = NULL;
    SwtmrHandlerItem swtmrHandle;
    UINT32 ret, swtmrHandlerQueue;

	//每个CPU有这样一个任务
	//只读属于我这个CPU的超时事件
    swtmrHandlerQueue = OsPercpuGet()->swtmrHandlerQueue;
    for (;;) {
		//从事件队列中读取超时消息
        ret = LOS_QueueRead(swtmrHandlerQueue, &swtmrHandlePtr, sizeof(CHAR *), LOS_WAIT_FOREVER);
        if ((ret == LOS_OK) && (swtmrHandlePtr != NULL)) {
			//取出超时处理函数和参数
            swtmrHandle.handler = swtmrHandlePtr->handler;
            swtmrHandle.arg = swtmrHandlePtr->arg;
			//释放超时消息
            (VOID)LOS_MemboxFree(g_swtmrHandlerPool, swtmrHandlePtr);
            if (swtmrHandle.handler != NULL) {
				//在任务中处理这个超时事件
                swtmrHandle.handler(swtmrHandle.arg);
            }
        }
    }
}


//创建软件定时器任务
LITE_OS_SEC_TEXT_INIT UINT32 OsSwtmrTaskCreate(VOID)
{
    UINT32 ret, swtmrTaskID;
    TSK_INIT_PARAM_S swtmrTask;
    UINT32 cpuid = ArchCurrCpuid();

    (VOID)memset_s(&swtmrTask, sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    swtmrTask.pfnTaskEntry = (TSK_ENTRY_FUNC)OsSwtmrTask;
    swtmrTask.uwStackSize = LOSCFG_BASE_CORE_TSK_DEFAULT_STACK_SIZE;
    swtmrTask.pcName = "Swt_Task";
    swtmrTask.usTaskPrio = 0;  //为了加强实时性，将任务优先级设置成最高
    swtmrTask.uwResved = LOS_TASK_STATUS_DETACHED;  //本任务为后台任务，会一直运行，不需要其它线程来等待它删除
#if (LOSCFG_KERNEL_SMP == YES)
    swtmrTask.usCpuAffiMask   = CPUID_TO_AFFI_MASK(cpuid);  //本任务只在当前CPU调度
#endif
    ret = LOS_TaskCreate(&swtmrTaskID, &swtmrTask);
    if (ret == LOS_OK) {
        g_percpu[cpuid].swtmrTaskID = swtmrTaskID;  //记录下本CPU创建好的软件定时器任务ID
        OS_TCB_FROM_TID(swtmrTaskID)->taskStatus |= OS_TASK_FLAG_SYSTEM_TASK; //标记为系统任务
    }

    return ret;
}


//回收某进程内的软件定时器资源
LITE_OS_SEC_TEXT_INIT VOID OsSwtmrRecycle(UINT32 processID)
{
	//遍历所有定时器
    for (UINT16 index = 0; index < LOSCFG_BASE_CORE_SWTMR_LIMIT; index++) {
        if (g_swtmrCBArray[index].uwOwnerPid == processID) {
			//回收指定进程内的定时器
            LOS_SwtmrDelete(index);
        }
    }
}

//软件定时器模块初始化
LITE_OS_SEC_TEXT_INIT UINT32 OsSwtmrInit(VOID)
{
    UINT32 size;
    UINT16 index;
    UINT32 ret;
    SWTMR_CTRL_S *swtmr = NULL;
    UINT32 swtmrHandlePoolSize;
    UINT32 cpuid = ArchCurrCpuid();
    if (cpuid == 0) {
		//0核，主核才会执行这里面的代码
		
		//创建所有的软件定时器资源
        size = sizeof(SWTMR_CTRL_S) * LOSCFG_BASE_CORE_SWTMR_LIMIT;
        swtmr = (SWTMR_CTRL_S *)LOS_MemAlloc(m_aucSysMem0, size); /* system resident resource */
        if (swtmr == NULL) {
            return LOS_ERRNO_SWTMR_NO_MEMORY;
        }

        (VOID)memset_s(swtmr, size, 0, size);
        g_swtmrCBArray = swtmr;
        LOS_ListInit(&g_swtmrFreeList); //初始化空闲定时器队列
        for (index = 0; index < LOSCFG_BASE_CORE_SWTMR_LIMIT; index++, swtmr++) {
            swtmr->usTimerID = index;  //记录每个定时器的ID
            //并将每个定时器放入空闲队列
            LOS_ListTailInsert(&g_swtmrFreeList, &swtmr->stSortList.sortLinkNode);
        }

		//创建定时器handler池
        swtmrHandlePoolSize = LOS_MEMBOX_SIZE(sizeof(SwtmrHandlerItem), OS_SWTMR_HANDLE_QUEUE_SIZE);

        g_swtmrHandlerPool = (UINT8 *)LOS_MemAlloc(m_aucSysMem1, swtmrHandlePoolSize); /* system resident resource */
        if (g_swtmrHandlerPool == NULL) {
            return LOS_ERRNO_SWTMR_NO_MEMORY;
        }

		//初始化定时器handler池
        ret = LOS_MemboxInit(g_swtmrHandlerPool, swtmrHandlePoolSize, sizeof(SwtmrHandlerItem));
        if (ret != LOS_OK) {
            return LOS_ERRNO_SWTMR_HANDLER_POOL_NO_MEM;
        }
    }

	//每个CPU核都会执行下面的代码

	//创建消息队列，用于定时器对象向定时器任务发送超时处理消息，每cpu一个
    ret = LOS_QueueCreate(NULL, OS_SWTMR_HANDLE_QUEUE_SIZE, &g_percpu[cpuid].swtmrHandlerQueue, 0, sizeof(CHAR *));
    if (ret != LOS_OK) {
        return LOS_ERRNO_SWTMR_QUEUE_CREATE_FAILED;
    }

	//创建定时器任务，用于接收上述消息并处理，每cpu一个
    ret = OsSwtmrTaskCreate();
    if (ret != LOS_OK) {
        return LOS_ERRNO_SWTMR_TASK_CREATE_FAILED;
    }

	//创建定时器超时监测数据结构，用于把定时器对象放入此数据结构中，每cpu一个
    ret = OsSortLinkInit(&g_percpu[cpuid].swtmrSortLink);
    if (ret != LOS_OK) {
        return LOS_ERRNO_SWTMR_SORTLINK_CREATE_FAILED;
    }

    return LOS_OK;
}

/*
 * Description: Start Software Timer
 * Input      : swtmr --- Need to start software timer
 */
 //启动定时器，即放入队列中
LITE_OS_SEC_TEXT VOID OsSwtmrStart(SWTMR_CTRL_S *swtmr)
{
    if ((swtmr->ucOverrun == 0) && ((swtmr->ucMode == LOS_SWTMR_MODE_ONCE) ||
        (swtmr->ucMode == LOS_SWTMR_MODE_OPP) ||
        (swtmr->ucMode == LOS_SWTMR_MODE_NO_SELFDELETE))) {
        //普通定时器使用uwExpiry记录超时时间
        SET_SORTLIST_VALUE(&(swtmr->stSortList), swtmr->uwExpiry);
    } else {
		//周期定时器使用uwInterval记录超时时间
        SET_SORTLIST_VALUE(&(swtmr->stSortList), swtmr->uwInterval);
    }

	//超时时间设置好以后，将定时器放入队列
    OsAdd2SortLink(&OsPercpuGet()->swtmrSortLink, &swtmr->stSortList);

    swtmr->ucState = OS_SWTMR_STATUS_TICKING;  //状态修订为正在计时(等待超时)状态

#if (LOSCFG_KERNEL_SMP == YES)
    swtmr->uwCpuid = ArchCurrCpuid();   //当前定时器所在的CPU
#endif

    return;
}

/*
 * Description: Delete Software Timer
 * Input      : swtmr --- Need to delete software timer, When using, Ensure that it can't be NULL.
 */
 //将定时器删除
STATIC INLINE VOID OsSwtmrDelete(SWTMR_CTRL_S *swtmr)
{
    /* insert to free list */
	//定时器放入空闲队列
    LOS_ListTailInsert(&g_swtmrFreeList, &swtmr->stSortList.sortLinkNode);
    swtmr->ucState = OS_SWTMR_STATUS_UNUSED; //并置空闲状态
    swtmr->uwOwnerPid = 0; //本定时器此时不属于任何进程
}

/*
 * Description: Tick interrupt interface module of software timer
 * Return     : LOS_OK on success or error code on failure
 */
 //每tick扫描一次定时器，看哪些定时器超时了，然后通知定时器任务处理超时逻辑
LITE_OS_SEC_TEXT VOID OsSwtmrScan(VOID)
{
    SortLinkList *sortList = NULL;
    SWTMR_CTRL_S *swtmr = NULL;
    SwtmrHandlerItemPtr swtmrHandler = NULL;
    LOS_DL_LIST *listObject = NULL;
	//只扫描本cpu下的定时器
    SortLinkAttribute* swtmrSortLink = &OsPercpuGet()->swtmrSortLink;

	//时间往前走了一个tick
    swtmrSortLink->cursor = (swtmrSortLink->cursor + 1) & OS_TSK_SORTLINK_MASK;
    listObject = swtmrSortLink->sortLink + swtmrSortLink->cursor;

    /*
     * it needs to be carefully coped with, since the swtmr is in specific sortlink
     * while other cores still has the chance to process it, like stop the timer.
     */
     //由于其他CPU可能会停止某个定时器，也会访问到这个队列，所以需要用自旋锁来保护队列操作
    LOS_SpinLock(&g_swtmrSpin);

    if (LOS_ListEmpty(listObject)) {
        LOS_SpinUnlock(&g_swtmrSpin);  //此刻度没有软件定时器
        return;
    }
    sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
    ROLLNUM_DEC(sortList->idxRollNum);  //此刻度的定时器从上一轮检测到目前间隔了1圈(roll)

    while (ROLLNUM(sortList->idxRollNum) == 0) {
		//对于已经处于超时状态的定时器
        sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
		//移除队列
        LOS_ListDelete(&sortList->sortLinkNode);
        swtmr = LOS_DL_LIST_ENTRY(sortList, SWTMR_CTRL_S, stSortList);

		//并从handler池中申请处理函数与参数信息记录的结构
        swtmrHandler = (SwtmrHandlerItemPtr)LOS_MemboxAlloc(g_swtmrHandlerPool);
        if (swtmrHandler != NULL) {
            swtmrHandler->handler = swtmr->pfnHandler;
            swtmrHandler->arg = swtmr->uwArg;

			//通过消息队列发送给定时器任务，不能阻塞
            if (LOS_QueueWrite(OsPercpuGet()->swtmrHandlerQueue, swtmrHandler, sizeof(CHAR *), LOS_NO_WAIT)) {
				//消息发送失败，则释放刚申请的内存
                (VOID)LOS_MemboxFree(g_swtmrHandlerPool, swtmrHandler);
            }
        }

        if (swtmr->ucMode == LOS_SWTMR_MODE_ONCE) {
			//只超时1次的定时器
            OsSwtmrDelete(swtmr); //那么这个时候得把定时器删除

			//对定时器ID做一个小调整，调整后的ID也能反映其在数组中的位置
			//用于辅助在平时读写定时器时，判断一下是不是之前创建的那个定时器
            if (swtmr->usTimerID < (OS_SWTMR_MAX_TIMERID - LOSCFG_BASE_CORE_SWTMR_LIMIT)) {
				//ID不回绕
                swtmr->usTimerID += LOSCFG_BASE_CORE_SWTMR_LIMIT;
            } else {
            	//ID不回绕
                swtmr->usTimerID %= LOSCFG_BASE_CORE_SWTMR_LIMIT;
            }
        } else if (swtmr->ucMode == LOS_SWTMR_MODE_NO_SELFDELETE) {
        	//不删除的定时器，又切换成已创建状态，等待下一次启动
            swtmr->ucState = OS_SWTMR_STATUS_CREATED;
        } else {
			//周期定时器，增加已超时的次数
            swtmr->ucOverrun++;
            OsSwtmrStart(swtmr); //再次启动定时器
        }

        if (LOS_ListEmpty(listObject)) {
            break;  //本队列定时器处理完
        }

		//处理队列中的下一个定时器
        sortList = LOS_DL_LIST_ENTRY(listObject->pstNext, SortLinkList, sortLinkNode);
    }

    LOS_SpinUnlock(&g_swtmrSpin);
}

/*
 * Description: Get next timeout
 * Return     : Count of the Timer list
 */
 //获取下一次的超时时间
LITE_OS_SEC_TEXT UINT32 OsSwtmrGetNextTimeout(VOID)
{
    return OsSortLinkGetNextExpireTime(&OsPercpuGet()->swtmrSortLink);
}

/*
 * Description: Stop of Software Timer interface
 * Input      : swtmr --- the software timer contrl handler
 */
 //停止某软件定时器
LITE_OS_SEC_TEXT STATIC VOID OsSwtmrStop(SWTMR_CTRL_S *swtmr)
{
    SortLinkAttribute *sortLinkHeader = NULL;

#if (LOSCFG_KERNEL_SMP == YES)
    /*
     * the timer is running on the specific processor,
     * we need delete the timer from that processor's sortlink.
     */
     //对于多核CPU，每个定时器都归属于不同的CPU
    sortLinkHeader = &g_percpu[swtmr->uwCpuid].swtmrSortLink;
#else
    sortLinkHeader = &g_percpu[0].swtmrSortLink;
#endif
	//从超时监控队列中移除
    OsDeleteSortLink(sortLinkHeader, &swtmr->stSortList);

    swtmr->ucState = OS_SWTMR_STATUS_CREATED;  //重新回到刚创建状态
    swtmr->ucOverrun = 0;  //超时次数清0
}

/*
 * Description: Get next software timer expiretime
 * Input      : swtmr --- the software timer contrl handler
 */
 //获取本定时器还有多久超时
LITE_OS_SEC_TEXT STATIC UINT32 OsSwtmrTimeGet(const SWTMR_CTRL_S *swtmr)
{
    SortLinkAttribute *sortLinkHeader = NULL;

#if (LOSCFG_KERNEL_SMP == YES)
    /*
     * the timer is running on the specific processor,
     * we need search the timer from that processor's sortlink.
     */
    sortLinkHeader = &g_percpu[swtmr->uwCpuid].swtmrSortLink;
#else
    sortLinkHeader = &g_percpu[0].swtmrSortLink;
#endif

    return OsSortLinkGetTargetExpireTime(sortLinkHeader, &swtmr->stSortList);
}

//创建软件定时器
LITE_OS_SEC_TEXT_INIT UINT32 LOS_SwtmrCreate(UINT32 interval,
                                             UINT8 mode,
                                             SWTMR_PROC_FUNC handler,
                                             UINT16 *swtmrID,
                                             UINTPTR arg)
{
    SWTMR_CTRL_S *swtmr = NULL;
    UINT32 intSave;
    SortLinkList *sortList = NULL;

    if (interval == 0) {
		//超时时间参数不合法
        return LOS_ERRNO_SWTMR_INTERVAL_NOT_SUITED;
    }

    if ((mode != LOS_SWTMR_MODE_ONCE) && (mode != LOS_SWTMR_MODE_PERIOD) &&
        (mode != LOS_SWTMR_MODE_NO_SELFDELETE)) {
        //定时器种类不支持
        return LOS_ERRNO_SWTMR_MODE_INVALID;
    }

    if (handler == NULL) {
		//定时器必须有一个业务处理函数
        return LOS_ERRNO_SWTMR_PTR_NULL;
    }

    if (swtmrID == NULL) {
		//返回的定时器ID必须有地方存放
        return LOS_ERRNO_SWTMR_RET_PTR_NULL;
    }

    SWTMR_LOCK(intSave);
    if (LOS_ListEmpty(&g_swtmrFreeList)) {
        SWTMR_UNLOCK(intSave);  //没有空闲的定时器资源
        return LOS_ERRNO_SWTMR_MAXSIZE;
    }

	//取出一个空闲的定时器描述符
    sortList = LOS_DL_LIST_ENTRY(g_swtmrFreeList.pstNext, SortLinkList, sortLinkNode);
    swtmr = LOS_DL_LIST_ENTRY(sortList, SWTMR_CTRL_S, stSortList);
    LOS_ListDelete(LOS_DL_LIST_FIRST(&g_swtmrFreeList));
    SWTMR_UNLOCK(intSave);
	
    swtmr->uwOwnerPid = OsCurrProcessGet()->processID;  //归属的进程
    swtmr->pfnHandler = handler;  //超时时候的处理函数
    swtmr->ucMode = mode;  //种类
    swtmr->ucOverrun = 0;  //超时次数统计
    swtmr->uwInterval = interval;   //还有多久超时
    swtmr->uwExpiry = interval;  //还有多久超时
    swtmr->uwArg = arg;      //处理函数匹配的参数
    swtmr->ucState = OS_SWTMR_STATUS_CREATED;  //刚创建状态
    SET_SORTLIST_VALUE(&(swtmr->stSortList), 0);  //还未启动
    *swtmrID = swtmr->usTimerID;   //返回ID

    return LOS_OK;
}

//启动定时器
LITE_OS_SEC_TEXT UINT32 LOS_SwtmrStart(UINT16 swtmrID)
{
    SWTMR_CTRL_S *swtmr = NULL;
    UINT32 intSave;
    UINT32 ret = LOS_OK;
    UINT16 swtmrCBID;

    if (swtmrID >= OS_SWTMR_MAX_TIMERID) {
		//ID越界
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }

    SWTMR_LOCK(intSave);
	//在数组中的下标
    swtmrCBID = swtmrID % LOSCFG_BASE_CORE_SWTMR_LIMIT;
    swtmr = g_swtmrCBArray + swtmrCBID;

    if (swtmr->usTimerID != swtmrID) {
        SWTMR_UNLOCK(intSave);  //已经不是之前创建的那个定时器了
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }

    switch (swtmr->ucState) {
        case OS_SWTMR_STATUS_UNUSED:
            ret = LOS_ERRNO_SWTMR_NOT_CREATED;  //还未创建的定时器
            break;
        /*
         * If the status of swtmr is timing, it should stop the swtmr first,
         * then start the swtmr again.
         */
        case OS_SWTMR_STATUS_TICKING:
            OsSwtmrStop(swtmr);   //已经启动的定时器，先停止，然后重新启动
            /* fall-through */
        case OS_SWTMR_STATUS_CREATED:
            OsSwtmrStart(swtmr);  //之前创建的定时器，则启动它
            break;
        default:
            ret = LOS_ERRNO_SWTMR_STATUS_INVALID;
            break;
    }

    SWTMR_UNLOCK(intSave);
    return ret;
}

//停止指定的定时器
LITE_OS_SEC_TEXT UINT32 LOS_SwtmrStop(UINT16 swtmrID)
{
    SWTMR_CTRL_S *swtmr = NULL;
    UINT32 intSave;
    UINT32 ret = LOS_OK;
    UINT16 swtmrCBID;

    if (swtmrID >= OS_SWTMR_MAX_TIMERID) {
		//ID越界
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }

    SWTMR_LOCK(intSave);
	//数组下标
    swtmrCBID = swtmrID % LOSCFG_BASE_CORE_SWTMR_LIMIT;
    swtmr = g_swtmrCBArray + swtmrCBID;

    if (swtmr->usTimerID != swtmrID) {
		//定时器可能已删除
        SWTMR_UNLOCK(intSave);
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }

    switch (swtmr->ucState) {
        case OS_SWTMR_STATUS_UNUSED:
            ret = LOS_ERRNO_SWTMR_NOT_CREATED; //还未创建
            break;
        case OS_SWTMR_STATUS_CREATED:
            ret = LOS_ERRNO_SWTMR_NOT_STARTED; //还未启动
            break;
        case OS_SWTMR_STATUS_TICKING:
            OsSwtmrStop(swtmr);  //正常停止
            break;
        default:
            ret = LOS_ERRNO_SWTMR_STATUS_INVALID;
            break;
    }

    SWTMR_UNLOCK(intSave);
    return ret;
}

//获取某定时器还有多久超时
LITE_OS_SEC_TEXT UINT32 LOS_SwtmrTimeGet(UINT16 swtmrID, UINT32 *tick)
{
    SWTMR_CTRL_S *swtmr = NULL;
    UINT32 intSave;
    UINT32 ret = LOS_OK;
    UINT16 swtmrCBID;

    if (swtmrID >= OS_SWTMR_MAX_TIMERID) {
		//越界
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }

    if (tick == NULL) {
		//无法输出参数
        return LOS_ERRNO_SWTMR_TICK_PTR_NULL;
    }

    SWTMR_LOCK(intSave);
	//下标
    swtmrCBID = swtmrID % LOSCFG_BASE_CORE_SWTMR_LIMIT;
    swtmr = g_swtmrCBArray + swtmrCBID;

    if (swtmr->usTimerID != swtmrID) {
		//可能定时器已删除
        SWTMR_UNLOCK(intSave);
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }
    switch (swtmr->ucState) {
        case OS_SWTMR_STATUS_UNUSED:
            ret = LOS_ERRNO_SWTMR_NOT_CREATED; //未创建
            break;
        case OS_SWTMR_STATUS_CREATED:
            ret = LOS_ERRNO_SWTMR_NOT_STARTED; //未启动
            break;
        case OS_SWTMR_STATUS_TICKING:
            *tick = OsSwtmrTimeGet(swtmr);  //获取其还有多久超时
            break;
        default:
            ret = LOS_ERRNO_SWTMR_STATUS_INVALID;
            break;
    }
    SWTMR_UNLOCK(intSave);
    return ret;
}


//删除定时器
LITE_OS_SEC_TEXT UINT32 LOS_SwtmrDelete(UINT16 swtmrID)
{
    SWTMR_CTRL_S *swtmr = NULL;
    UINT32 intSave;
    UINT32 ret = LOS_OK;
    UINT16 swtmrCBID;

    if (swtmrID >= OS_SWTMR_MAX_TIMERID) {
		//越界
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }

    SWTMR_LOCK(intSave);
	//下标
    swtmrCBID = swtmrID % LOSCFG_BASE_CORE_SWTMR_LIMIT;
    swtmr = g_swtmrCBArray + swtmrCBID;

    if (swtmr->usTimerID != swtmrID) {
		//可能已删除
        SWTMR_UNLOCK(intSave);
        return LOS_ERRNO_SWTMR_ID_INVALID;
    }

    switch (swtmr->ucState) {
        case OS_SWTMR_STATUS_UNUSED:
            ret = LOS_ERRNO_SWTMR_NOT_CREATED; //未创建
            break;
        case OS_SWTMR_STATUS_TICKING:
            OsSwtmrStop(swtmr);   //已启动，则先停止
            /* fall-through */
        case OS_SWTMR_STATUS_CREATED:
            OsSwtmrDelete(swtmr); //已创建或已启动，删除
            break;
        default:
            ret = LOS_ERRNO_SWTMR_STATUS_INVALID;
            break;
    }

    SWTMR_UNLOCK(intSave);
    return ret;
}

#endif /* (LOSCFG_BASE_CORE_SWTMR == YES) */

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
