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

#include "los_memory_pri.h"
#include "los_vm_phys.h"
#include "los_vm_boot.h"
#include "los_vm_common.h"
#include "los_vm_filemap.h"
#include "asm/page.h"
#include "los_multipledlinkhead_pri.h"
#include "los_memstat_pri.h"
#include "los_memrecord_pri.h"
#include "los_task_pri.h"
#include "los_exc.h"
#include "los_spinlock.h"

#ifdef LOSCFG_SHELL_EXCINFO
#include "los_excinfo_pri.h"
#endif

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define NODEDUMPSIZE  64  /* the dump size of current broken node when memcheck error */
#define COLUMN_NUM    8   /* column num of the output info of mem node */

#define MEM_POOL_EXPAND_ENABLE  1
#define MEM_POOL_EXPAND_DISABLE 0

/* Memory pool information structure */
//内存池数据结构
typedef struct {
	//内存池起始地址
    VOID *pool;      /* Starting address of a memory pool */
	//内存池尺寸
    UINT32 poolSize; /* Memory pool size */
	//内存池是否支持伸缩
    UINT32 flag;     /* Whether the memory pool supports expansion */
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//内存池使用水线，即历史上最大使用量
    UINT32 poolWaterLine;   /* Maximum usage size in a memory pool */
	//内存池当前使用量
    UINT32 poolCurUsedSize; /* Current usage size in a memory pool */
#endif
#ifdef LOSCFG_MEM_MUL_POOL
    VOID *nextPool; //下一个内存池，在系统支持多内存池时有效
#endif
} LosMemPoolInfo;

/* Memory linked list control node structure */
//内存节点控制头部。分为2种内存节点
/*
 *  1. 数据节点(含控制头和数据)，用于提供给用户使用，数据节点在一块连续的内存块中可以切割出多个
 *  2. 哨兵节点(只含控制头)，用于辅助管理，特别是将离散的内存块链接起来，
 */
typedef struct {
	//对于数据节点，如果当前空闲，则用此字段连接空闲链表
	//              如果不空闲，pstNext记录申请者模块ID和线程ID
	//对于哨兵节点，pstNext用来连接下一个内存块 ，pstPrev用来标记当前节点是哨兵
    LOS_DL_LIST freeNodeInfo;         /* Free memory node */
	
	//当前节点的上一个节点
	//对于数据节点，上一个节点指的是内存地址稍小的那个邻居数据节点。
	//   如果当前是本内存块的第一个数据节点，前面不存在数据节点了，那么又分2种情况
	//      1. 内存池中第1个数据节点，preNode为NULL值
	//      2. 其他内存块中的第1个数据节点，preNode为本内存块中的哨兵节点地址
	//      这么设计是为了辅助内存池外其他内存的回收
    struct tagLosMemDynNode *preNode; /* Pointer to the previous memory node */

#ifdef LOSCFG_MEM_HEAD_BACKUP //将内存节点头部信息备份
	//对于有对齐要求的内存申请，如果申请出来的内存块满足不了对齐要求
	//则需要移动指针使得其满足要求，那么移动的字节数目就记录在gapSize中
    UINT32 gapSize;  
	//每一个节点把自己头部的校验和求出来，然后完整备份到前一个节点中
    UINTPTR checksum; /* magic = xor checksum */ 
#endif

#ifdef LOSCFG_MEM_RECORDINFO  //主要用于运维统计
    UINT32 originSize; //记录用户请求申请的尺寸
#ifdef LOSCFG_AARCH64
	//对于64位ARM架构，填充一个字段，使得后面的数组对齐在8字节边界上
    UINT32 reserve1; /* 64-bit alignment */
#endif
#endif

#ifdef LOSCFG_MEM_LEAKCHECK //支持内存泄露检测时
	//当做内存泄露检测时，需要了解残留内存是怎么申请的。
	//这里就是记录内存申请时的函数调用栈对应的LR寄存器的值
	//结合符号表，就可以获得对应的函数
    UINTPTR linkReg[LOS_RECORD_LR_CNT];
#endif

#ifdef LOSCFG_AARCH64
	//64位ARM用于8字节对齐
    UINT32 reserve2; /* 64-bit alignment */
#endif
	//当前内存节点的尺寸，最高2位用来做标记，剩余位用来表示尺寸
    /* Size and flag of the current node (the high two bits represent a flag,and the rest bits specify the size) */
    UINT32 sizeAndFlag;
} LosMemCtlNode;

/* Memory linked list node structure */
typedef struct tagLosMemDynNode {
#ifdef LOSCFG_MEM_HEAD_BACKUP
    LosMemCtlNode backupNode;  //用来备份下一个节点
#endif
    LosMemCtlNode selfNode;    //记录当前节点的信息
} LosMemDynNode;

#ifdef LOSCFG_MEM_MUL_POOL
VOID *g_poolHead = NULL;  //多内存池情况下的链表头，用来记录内存池链表
#endif

/* spinlock for mem module, only available on SMP mode */
LITE_OS_SEC_BSS  SPIN_LOCK_INIT(g_memSpin);

#ifdef LOSCFG_MEM_HEAD_BACKUP
/*
 * 0xffff0000U, 0xffffU
 * the taskID and moduleID multiplex the node->selfNode.freeNodeInfo.pstNext
 * the low 16 bits is the taskID and the high 16bits is the moduleID
 */
STATIC VOID OsMemNodeSave(LosMemDynNode *node);
//当内存节点不空闲时，我们就可以重用pstNext字段来存储信息
//这里用pstNext低16位来存储使用此内存节点的任务ID/线程ID
//并且需要重新备份节点头信息到前一个节点中，当然校验和也要刷新
#define OS_MEM_TASKID_SET(node, ID) do {                                                  \
    UINTPTR tmp_ = (UINTPTR)(((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext);   \
    tmp_ &= 0xffff0000U;                                                                  \
    tmp_ |= (ID);                                                                         \
    ((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext = (LOS_DL_LIST *)tmp_;       \
    OsMemNodeSave((LosMemDynNode *)(node));                                               \
} while (0)
#else
#define OS_MEM_TASKID_SET(node, ID) do {                                                  \
    UINTPTR tmp_ = (UINTPTR)(((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext);   \
    tmp_ &= 0xffff0000U;                                                                  \
    tmp_ |= (ID);                                                                         \
    ((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext = (LOS_DL_LIST *)tmp_;       \
} while (0)
#endif
#define OS_MEM_TASKID_GET(node) ((UINTPTR)(((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext) & 0xffffU)

#ifdef LOSCFG_MEM_MUL_MODULE  //多模块支持，即还要记录这个内存是由哪个模块使用的
#define BITS_NUM_OF_TYPE_SHORT    16
#ifdef LOSCFG_MEM_HEAD_BACKUP
//记录模块ID，在pstNext高16位
#define OS_MEM_MODID_SET(node, ID) do {                                                   \
    UINTPTR tmp_ = (UINTPTR)(((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext);   \
    tmp_ &= 0xffffU;                                                                      \
    tmp_ |= (ID) << BITS_NUM_OF_TYPE_SHORT;                                               \
    ((LosMemDynNode *)node)->selfNode.freeNodeInfo.pstNext = (LOS_DL_LIST *)tmp_;         \
    OsMemNodeSave((LosMemDynNode *)(node));                                               \
} while (0)
#else
#define OS_MEM_MODID_SET(node, ID) do {                                                   \
    UINTPTR tmp_ = (UINTPTR)(((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext);   \
    tmp_ &= 0xffffU;                                                                      \
    tmp_ |= (ID) << BITS_NUM_OF_TYPE_SHORT;                                               \
    ((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext = (LOS_DL_LIST *)tmp_;       \
} while (0)
#endif
#define OS_MEM_MODID_GET(node) \
    (((UINTPTR)(((LosMemDynNode *)(node))->selfNode.freeNodeInfo.pstNext) >> BITS_NUM_OF_TYPE_SHORT) & 0xffffU)
#endif

//移动指针，使得其满足对齐要求
#define OS_MEM_ALIGN(p, alignSize) (((UINTPTR)(p) + (alignSize) - 1) & ~((UINTPTR)((alignSize) - 1)))
//内存节点头部信息尺寸
#define OS_MEM_NODE_HEAD_SIZE      sizeof(LosMemDynNode)
//最小的内存池尺寸
//包含内存池结构体，2个无实际数据的内存节点，以及一个必须的双向链表头数组
#define OS_MEM_MIN_POOL_SIZE       (OS_DLNK_HEAD_SIZE + (2 * OS_MEM_NODE_HEAD_SIZE) + sizeof(LosMemPoolInfo))
//是否2的幂指数
#define IS_POW_TWO(value)          ((((UINTPTR)(value)) & ((UINTPTR)(value) - 1)) == 0)
//内存池地址需要对齐与64字节的整数倍
#define POOL_ADDR_ALIGNSIZE        64
#ifdef LOSCFG_AARCH64
//64位ARM CPU操作的数据需要对齐于8字节
#define OS_MEM_ALIGN_SIZE 8
#else
//32位ARM CPU操作的数据需要对齐于4字节
#define OS_MEM_ALIGN_SIZE 4
#endif
//最高位置1表示当前内存节点正在使用
#define OS_MEM_NODE_USED_FLAG             0x80000000U
//次高位置1表示当前内存节点进行了内存对齐处理
#define OS_MEM_NODE_ALIGNED_FLAG          0x40000000U
//最高2位都为1则表示上述2个逻辑同时满足
#define OS_MEM_NODE_ALIGNED_AND_USED_FLAG (OS_MEM_NODE_USED_FLAG | OS_MEM_NODE_ALIGNED_FLAG)

//内存节点是否做了对齐处理
#define OS_MEM_NODE_GET_ALIGNED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) & OS_MEM_NODE_ALIGNED_FLAG)
//设置内存节点的对齐标志
#define OS_MEM_NODE_SET_ALIGNED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) = ((sizeAndFlag) | OS_MEM_NODE_ALIGNED_FLAG))
//获取内存节点的实际尺寸
#define OS_MEM_NODE_GET_ALIGNED_GAPSIZE(sizeAndFlag) \
    ((sizeAndFlag) & ~OS_MEM_NODE_ALIGNED_FLAG)
//获取内存节点的使用标志
#define OS_MEM_NODE_GET_USED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) & OS_MEM_NODE_USED_FLAG)
//设置内存节点的使用标志
#define OS_MEM_NODE_SET_USED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) = ((sizeAndFlag) | OS_MEM_NODE_USED_FLAG))
//获取内存节点的尺寸
#define OS_MEM_NODE_GET_SIZE(sizeAndFlag) \
    ((sizeAndFlag) & ~OS_MEM_NODE_ALIGNED_AND_USED_FLAG)
//根据节点尺寸和内存池获取内存信息链表头
#define OS_MEM_HEAD(pool, size) \
    OsDLnkMultiHead(OS_MEM_HEAD_ADDR(pool), size)
//获取链表头数组首地址，链表头数组在内存池控制块之后
#define OS_MEM_HEAD_ADDR(pool) \
    ((VOID *)((UINTPTR)(pool) + sizeof(LosMemPoolInfo)))
//根据当前节点获取下一个节点
//当前节点地址+当前节点尺寸
#define OS_MEM_NEXT_NODE(node) \
    ((LosMemDynNode *)(VOID *)((UINT8 *)(node) + OS_MEM_NODE_GET_SIZE((node)->selfNode.sizeAndFlag)))
//获取第一个节点的地址，第一个节点在链表头数组之后
#define OS_MEM_FIRST_NODE(pool) \
    ((LosMemDynNode *)(VOID *)((UINT8 *)OS_MEM_HEAD_ADDR(pool) + OS_DLNK_HEAD_SIZE))
//获取末尾节点的地址，末尾节点在内存池的末尾，且只含内存节点头，不含数据，方便内存池管理
#define OS_MEM_END_NODE(pool, size) \
    ((LosMemDynNode *)(VOID *)(((UINT8 *)(pool) + (size)) - OS_MEM_NODE_HEAD_SIZE))
//middleAddr是否在半开区间[startAddr, endAddr)中
#define OS_MEM_MIDDLE_ADDR_OPEN_END(startAddr, middleAddr, endAddr) \
    (((UINT8 *)(startAddr) <= (UINT8 *)(middleAddr)) && ((UINT8 *)(middleAddr) < (UINT8 *)(endAddr)))
//middleAddr是否在闭区间[startAddr, endAddr]中
#define OS_MEM_MIDDLE_ADDR(startAddr, middleAddr, endAddr) \
    (((UINT8 *)(startAddr) <= (UINT8 *)(middleAddr)) && ((UINT8 *)(middleAddr) <= (UINT8 *)(endAddr)))
//设置魔数，让其与当前变量的地址相关，这样具有随机性
//因为变量value的位置无法提前确定，加载后才知道，所以具有随机性
#define OS_MEM_SET_MAGIC(value) \
    (value) = (LOS_DL_LIST *)(((UINTPTR)&(value)) ^ (UINTPTR)(-1))
//判断魔数是否正确，用于判断内存是否被改写
#define OS_MEM_MAGIC_VALID(value) \
    (((UINTPTR)(value) ^ ((UINTPTR)&(value))) == (UINTPTR)(-1))

UINT8 *m_aucSysMem0 = NULL;  //内核内存池0， 本内存池主要用于中断上下文中，以及shell中
UINT8 *m_aucSysMem1 = NULL;  //内核内存池1

#ifdef LOSCFG_BASE_MEM_NODE_SIZE_CHECK  //对内存节点进行合法性检查
//分成几个检查级别，缺省关闭检查
STATIC UINT8 g_memCheckLevel = LOS_MEM_CHECK_LEVEL_DEFAULT;
#endif

#ifdef LOSCFG_MEM_MUL_MODULE
//基于软件模块的内存占用量统计
UINT32 g_moduleMemUsedSize[MEM_MODULE_MAX + 1] = { 0 };
#endif

VOID OsMemInfoPrint(VOID *pool);
#ifdef LOSCFG_BASE_MEM_NODE_SIZE_CHECK
const VOID *OsMemFindNodeCtrl(const VOID *pool, const VOID *ptr);
#endif
#ifdef LOSCFG_MEM_HEAD_BACKUP
//用于辅助计算校验和
#define CHECKSUM_MAGICNUM    0xDEADBEEF 
//计算需要备份的信息的校验和
#define OS_MEM_NODE_CHECKSUN_CALCULATE(ctlNode)    \
    (((UINTPTR)(ctlNode)->freeNodeInfo.pstPrev) ^  \
    ((UINTPTR)(ctlNode)->freeNodeInfo.pstNext) ^   \
    ((UINTPTR)(ctlNode)->preNode) ^                \
    (ctlNode)->gapSize ^                           \
    (ctlNode)->sizeAndFlag ^                       \
    CHECKSUM_MAGICNUM)

//打印输出内存节点头部的有效信息
STATIC INLINE VOID OsMemDispCtlNode(const LosMemCtlNode *ctlNode)
{
    UINTPTR checksum;

    checksum = OS_MEM_NODE_CHECKSUN_CALCULATE(ctlNode);

    PRINTK("node:%p checksum=%p[%p] freeNodeInfo.pstPrev=%p "
           "freeNodeInfo.pstNext=%p preNode=%p gapSize=0x%x sizeAndFlag=0x%x\n",
           ctlNode,
           ctlNode->checksum,
           checksum,
           ctlNode->freeNodeInfo.pstPrev,
           ctlNode->freeNodeInfo.pstNext,
           ctlNode->preNode,
           ctlNode->gapSize,
           ctlNode->sizeAndFlag);
}


//打印输出内存节点的更详细信息
STATIC INLINE VOID OsMemDispMoreDetails(const LosMemDynNode *node)
{
    UINT32 taskID;
    LosTaskCB *taskCB = NULL;

    PRINT_ERR("************************************************\n");
    OsMemDispCtlNode(&node->selfNode);
    PRINT_ERR("the address of node :%p\n", node);

    if (!OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
        PRINT_ERR("this is a FREE node\n");
        PRINT_ERR("************************************************\n\n");
        return;
    }

    taskID = OS_MEM_TASKID_GET(node);
    if (OS_TID_CHECK_INVALID(taskID)) {
        PRINT_ERR("The task [ID: 0x%x] is ILLEGAL\n", taskID);
        if (taskID == g_taskMaxNum) {
            PRINT_ERR("PROBABLY alloc by SYSTEM INIT, NOT IN ANY TASK\n");
        }
        PRINT_ERR("************************************************\n\n");
        return;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    if (OsTaskIsUnused(taskCB) || (taskCB->taskEntry == NULL)) {
        PRINT_ERR("The task [ID: 0x%x] is NOT CREATED(ILLEGAL)\n", taskID);
        PRINT_ERR("************************************************\n\n");
        return;
    }

    PRINT_ERR("allocated by task: %s [ID = 0x%x]\n", taskCB->taskName, taskID);
#ifdef LOSCFG_MEM_MUL_MODULE
    PRINT_ERR("allocated by moduleID: %lu\n", OS_MEM_MODID_GET(node));
#endif

    PRINT_ERR("************************************************\n\n");
}


//打印输出野指针相关的信息，并输出函数调用栈
STATIC INLINE VOID OsMemDispWildPointerMsg(const LosMemDynNode *node, const VOID *ptr)
{
    PRINT_ERR("*****************************************************\n");
    PRINT_ERR("find an control block at: %p, gap size: 0x%x, sizeof(LosMemDynNode): 0x%x\n", node,
              node->selfNode.gapSize, sizeof(LosMemDynNode));
    PRINT_ERR("the pointer should be: %p\n",
              ((UINTPTR)node + node->selfNode.gapSize + sizeof(LosMemDynNode)));
    PRINT_ERR("the pointer given is: %p\n", ptr);
    PRINT_ERR("PROBABLY A WILD POINTER\n");
    OsBackTrace();
    PRINT_ERR("*****************************************************\n\n");
}


//计算并设置内存节点头部信息的校验和
STATIC INLINE VOID OsMemChecksumSet(LosMemCtlNode *ctlNode)
{
    ctlNode->checksum = OS_MEM_NODE_CHECKSUN_CALCULATE(ctlNode);
}

//检验校验和
STATIC INLINE BOOL OsMemChecksumVerify(const LosMemCtlNode *ctlNode)
{
    return ctlNode->checksum == OS_MEM_NODE_CHECKSUN_CALCULATE(ctlNode);
}

//对内存节点头部信息进行备份
//将其备份到前一个节点
STATIC INLINE VOID OsMemBackupSetup(const LosMemDynNode *node)
{
    LosMemDynNode *nodePre = node->selfNode.preNode;
    if (nodePre != NULL) {
        nodePre->backupNode.freeNodeInfo.pstNext = node->selfNode.freeNodeInfo.pstNext;
        nodePre->backupNode.freeNodeInfo.pstPrev = node->selfNode.freeNodeInfo.pstPrev;
        nodePre->backupNode.preNode = node->selfNode.preNode;
        nodePre->backupNode.checksum = node->selfNode.checksum;
        nodePre->backupNode.gapSize = node->selfNode.gapSize;
#ifdef LOSCFG_MEM_RECORDINFO
        nodePre->backupNode.originSize = node->selfNode.originSize;
#endif
        nodePre->backupNode.sizeAndFlag = node->selfNode.sizeAndFlag;
    }
}

//获取当前节点的下一个节点
//注意，尾部节点的下一个节点是第一个节点
LosMemDynNode *OsMemNodeNextGet(const VOID *pool, const LosMemDynNode *node)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;

    if (node == OS_MEM_END_NODE(pool, poolInfo->poolSize)) {
        return OS_MEM_FIRST_NODE(pool);
    } else {
        return OS_MEM_NEXT_NODE(node);
    }
}


//将下一个节点的头部信息备份到本节点
STATIC INLINE UINT32 OsMemBackupSetup4Next(const VOID *pool, LosMemDynNode *node)
{
    LosMemDynNode *nodeNext = OsMemNodeNextGet(pool, node);

    if (!OsMemChecksumVerify(&nodeNext->selfNode)) {
		//我们只备份正常的节点，如果节点已被毁坏，那么不再备份
		//使用校验和来验证，其是否已毁坏
        PRINT_ERR("[%s]the next node is broken!!\n", __FUNCTION__);
        OsMemDispCtlNode(&(nodeNext->selfNode));
        PRINT_ERR("Current node details:\n");
        OsMemDispMoreDetails(node);

        return LOS_NOK;
    }

    if (!OsMemChecksumVerify(&node->backupNode)) {
		//原来备份的信息已经不准确，那么需要再次备份
		//将下一个节点的头部信息备份到本节点
        node->backupNode.freeNodeInfo.pstNext = nodeNext->selfNode.freeNodeInfo.pstNext;
        node->backupNode.freeNodeInfo.pstPrev = nodeNext->selfNode.freeNodeInfo.pstPrev;
        node->backupNode.preNode = nodeNext->selfNode.preNode;
        node->backupNode.checksum = nodeNext->selfNode.checksum;
        node->backupNode.gapSize = nodeNext->selfNode.gapSize;
#ifdef LOSCFG_MEM_RECORDINFO
        node->backupNode.originSize = nodeNext->selfNode.originSize;
#endif
        node->backupNode.sizeAndFlag = nodeNext->selfNode.sizeAndFlag;
    }
    return LOS_OK;
}


//根据上一个节点存储的备份信息恢复当前节点头部
UINT32 OsMemBackupDoRestore(VOID *pool, const LosMemDynNode *nodePre, LosMemDynNode *node)
{
    if (node == NULL) {
        PRINT_ERR("the node is NULL.\n");
        return LOS_NOK;
    }
    PRINT_ERR("the backup node information of current node in previous node:\n");
    OsMemDispCtlNode(&nodePre->backupNode);
    PRINT_ERR("the detailed information of previous node:\n");
    OsMemDispMoreDetails(nodePre);

    node->selfNode.freeNodeInfo.pstNext = nodePre->backupNode.freeNodeInfo.pstNext;
    node->selfNode.freeNodeInfo.pstPrev = nodePre->backupNode.freeNodeInfo.pstPrev;
    node->selfNode.preNode = nodePre->backupNode.preNode;
    node->selfNode.checksum = nodePre->backupNode.checksum;
    node->selfNode.gapSize = nodePre->backupNode.gapSize;
#ifdef LOSCFG_MEM_RECORDINFO
    node->selfNode.originSize = nodePre->backupNode.originSize;
#endif
    node->selfNode.sizeAndFlag = nodePre->backupNode.sizeAndFlag;

    /* we should re-setup next node's backup on current node */
	//在当前节点恢复以后，需要再次备份一下下一个节点的头部信息
	//因为当前节点之前遭遇了破坏，很大概率上也破坏了存储在当前节点中的下一个节点的备份信息
    return OsMemBackupSetup4Next(pool, node);
}


//获取首节点的前一个节点(即尾节点)，并顺带检查正确性
STATIC LosMemDynNode *OsMemFirstNodePrevGet(const LosMemPoolInfo *poolInfo)
{
    LosMemDynNode *nodePre = NULL;

	//第一个内存节点的前一个节点就是(尾节点)
    nodePre = OS_MEM_END_NODE(poolInfo, poolInfo->poolSize);
    if (!OsMemChecksumVerify(&(nodePre->selfNode))) {
		//在尾节点中检查校验和，发现尾节点已被破坏
        PRINT_ERR("the current node is THE FIRST NODE !\n");
        PRINT_ERR("[%s]: the node information of previous node is bad !!\n", __FUNCTION__);
        OsMemDispCtlNode(&(nodePre->selfNode));
        return nodePre;
    }
    if (!OsMemChecksumVerify(&(nodePre->backupNode))) {
		//尾节点中的备份信息已被破坏
        PRINT_ERR("the current node is THE FIRST NODE !\n");
        PRINT_ERR("[%s]: the backup node information of current node in previous Node is bad !!\n", __FUNCTION__);
        OsMemDispCtlNode(&(nodePre->backupNode));
        return nodePre;
    }

    return NULL;
}


//获取当前节点的前一个节点
LosMemDynNode *OsMemNodePrevGet(VOID *pool, const LosMemDynNode *node)
{
    LosMemDynNode *nodeCur = NULL;
    LosMemDynNode *nodePre = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;

    if (node == OS_MEM_FIRST_NODE(pool)) {
		//如果当前节点是首节点，那么获取尾节点
        return OsMemFirstNodePrevGet(poolInfo);
    }

	//否则，遍历所有节点，不含尾节点
    for (nodeCur = OS_MEM_FIRST_NODE(pool);
         nodeCur < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         nodeCur = OS_MEM_NEXT_NODE(nodeCur)) {
        if (!OsMemChecksumVerify(&(nodeCur->selfNode))) {
			//如果当前节点头部已被破坏
            PRINT_ERR("[%s]: the node information of current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->selfNode));

            if (nodePre == NULL) {
                return NULL;
            }

            PRINT_ERR("the detailed information of previous node:\n");
            OsMemDispMoreDetails(nodePre);

            /* due to the every step's checksum verify, nodePre is trustful */
			//尝试修复当前节点
			//因为我们是从前往后修复的，所以当前节点的前一个节点总是可信任的
            if (OsMemBackupDoRestore(pool, nodePre, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

        if (!OsMemChecksumVerify(&(nodeCur->backupNode))) {
			//然后检查当前节点中存储的备份信息，如果无效
            PRINT_ERR("[%s]: the backup node information in current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->backupNode));

            if (nodePre != NULL) {
                PRINT_ERR("the detailed information of previous node:\n");
                OsMemDispMoreDetails(nodePre);
            }

			//则重建备份信息
            if (OsMemBackupSetup4Next(pool, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

        if (OS_MEM_NEXT_NODE(nodeCur) == node) {
            return nodeCur; //找到node的前一个节点
        }

        if (OS_MEM_NEXT_NODE(nodeCur) > node) {
            break; //没有找到node的前一个节点
        }

        nodePre = nodeCur; //继续寻找
    }

    return NULL; //没有找到的情况， 说明node参数给错了
}


//尝试获取当前节点的上一个节点
LosMemDynNode *OsMemNodePrevTryGet(VOID *pool, LosMemDynNode **node, const VOID *ptr)
{
    UINTPTR nodeShoudBe = 0;
    LosMemDynNode *nodeCur = NULL;
    LosMemDynNode *nodePre = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;

    if (ptr == OS_MEM_FIRST_NODE(pool)) {
		//首节点的上一个节点是尾节点
        return OsMemFirstNodePrevGet(poolInfo);
    }

	//遍历所有节点，尾节点除外
    for (nodeCur = OS_MEM_FIRST_NODE(pool);
         nodeCur < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         nodeCur = OS_MEM_NEXT_NODE(nodeCur)) {
        if (!OsMemChecksumVerify(&(nodeCur->selfNode))) {
			//当前节点头部损坏
            PRINT_ERR("[%s]: the node information of current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->selfNode));

            if (nodePre == NULL) {
                return NULL;  //如果首节点损坏，则无法修复
            }

            PRINT_ERR("the detailed information of previous node:\n");
            OsMemDispMoreDetails(nodePre);

            /* due to the every step's checksum verify, nodePre is trustful */
			//尝试修复
            if (OsMemBackupDoRestore(pool, nodePre, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

        if (!OsMemChecksumVerify(&(nodeCur->backupNode))) {
			//当前节点中的备份信息损坏
            PRINT_ERR("[%s]: the backup node information in current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->backupNode));

            if (nodePre != NULL) {
                PRINT_ERR("the detailed information of previous node:\n");
                OsMemDispMoreDetails(nodePre);
            }

			//则重建备份信息
            if (OsMemBackupSetup4Next(pool, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

		//当前节点是否就是用户指针ptr所在的节点
        nodeShoudBe = (UINTPTR)nodeCur + nodeCur->selfNode.gapSize + sizeof(LosMemDynNode);
        if (nodeShoudBe == (UINTPTR)ptr) {
			//如果是，那么记录下当前节点，并返回上一个节点
            *node = nodeCur;
            return nodePre;
        }

        if (OS_MEM_NEXT_NODE(nodeCur) > (LosMemDynNode *)ptr) {
            OsMemDispWildPointerMsg(nodeCur, ptr);
            break; //无法再找到用户指针所在的节点，用户可能传递了一个野指针
        }

        nodePre = nodeCur;
    }

    return NULL;
}


//根据备份信息恢复节点
STATIC INLINE UINT32 OsMemBackupTryRestore(VOID *pool, LosMemDynNode **node, const VOID *ptr)
{
    LosMemDynNode *nodeHead = NULL;
	//尝试获取指针对应的当前节点和上一个节点
    LosMemDynNode *nodePre = OsMemNodePrevTryGet(pool, &nodeHead, ptr); 
    if (nodePre == NULL) {
        return LOS_NOK; //没有上一个节点的情况下，没法恢复当前节点
    }

    *node = nodeHead;  //记录当前节点
    //根据上一个节点恢复当前节点
    return OsMemBackupDoRestore(pool, nodePre, *node);
}


//恢复某节点头部信息
STATIC INLINE UINT32 OsMemBackupRestore(VOID *pool, LosMemDynNode *node)
{
	//获取上一个节点
    LosMemDynNode *nodePre = OsMemNodePrevGet(pool, node);
    if (nodePre == NULL) {
        return LOS_NOK;
    }

	//恢复当前节点
    return OsMemBackupDoRestore(pool, nodePre, node);
}

//设置对齐需求导致的指针偏移字节数
STATIC INLINE VOID OsMemSetGapSize(LosMemCtlNode *ctlNode, UINT32 gapSize)
{
    ctlNode->gapSize = gapSize;
}

//普通节点的信息保存
STATIC VOID OsMemNodeSave(LosMemDynNode *node)
{
	//无对齐需求
    OsMemSetGapSize(&node->selfNode, 0);
	//计算校验和
    OsMemChecksumSet(&node->selfNode);
	//备份到前一个节点
    OsMemBackupSetup(node);
}


//有对齐需求的内存节点的保存
STATIC VOID OsMemNodeSaveWithGapSize(LosMemDynNode *node, UINT32 gapSize)
{
	//设置对齐引起的指针偏移字节数
    OsMemSetGapSize(&node->selfNode, gapSize);
    OsMemChecksumSet(&node->selfNode); //计算校验和
    OsMemBackupSetup(node); //备份
}

//从空闲链表中删除内存节点
STATIC VOID OsMemListDelete(LOS_DL_LIST *node, const VOID *firstNode)
{
    LosMemDynNode *dynNode = NULL;

	//从双向链表中下链
    node->pstNext->pstPrev = node->pstPrev;
    node->pstPrev->pstNext = node->pstNext;

    if ((VOID *)(node->pstNext) >= firstNode) {
		//如果下一个节点不是首节点，那么备份下一个节点
        dynNode = LOS_DL_LIST_ENTRY(node->pstNext, LosMemDynNode, selfNode.freeNodeInfo);
        OsMemNodeSave(dynNode);
    }

    if ((VOID *)(node->pstPrev) >= firstNode) {
		//如果上一个节点不是首节点，那么备份上一个节点
        dynNode = LOS_DL_LIST_ENTRY(node->pstPrev, LosMemDynNode, selfNode.freeNodeInfo);
        OsMemNodeSave(dynNode);
    }

	//此节点不在空闲链表上了
    node->pstNext = NULL;
    node->pstPrev = NULL;

    dynNode = LOS_DL_LIST_ENTRY(node, LosMemDynNode, selfNode.freeNodeInfo);
    OsMemNodeSave(dynNode); //备份当前节点
}


//将指定节点插入另一个节点之后
STATIC VOID OsMemListAdd(LOS_DL_LIST *listNode, LOS_DL_LIST *node, const VOID *firstNode)
{
    LosMemDynNode *dynNode = NULL;

	//node插入listNode之后
    node->pstNext = listNode->pstNext;
    node->pstPrev = listNode;

    dynNode = LOS_DL_LIST_ENTRY(node, LosMemDynNode, selfNode.freeNodeInfo);
    OsMemNodeSave(dynNode); //备份当前节点

    listNode->pstNext->pstPrev = node;
    if ((VOID *)(listNode->pstNext) >= firstNode) {
		//备份后一个节点
        dynNode = LOS_DL_LIST_ENTRY(listNode->pstNext, LosMemDynNode, selfNode.freeNodeInfo);
        OsMemNodeSave(dynNode);
    }

    listNode->pstNext = node;
}


//打印输出异常节点的信息
VOID LOS_MemBadNodeShow(VOID *pool)
{
    LosMemDynNode *nodePre = NULL;
    LosMemDynNode *tmpNode = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    UINT32 intSave;

    if (pool == NULL) {
        return;
    }

    MEM_LOCK(intSave);

	//遍历所有内存节点，尾节点除外
    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
        OsMemDispCtlNode(&tmpNode->selfNode);  //打印当前节点

        if (OsMemChecksumVerify(&tmpNode->selfNode)) {
            continue; //当前节点没有损坏
        }

		//获取上一个节点
        nodePre = OsMemNodePrevGet(pool, tmpNode);
        if (nodePre == NULL) {
            PRINT_ERR("the current node is invalid, but cannot find its previous Node\n");
            continue; //不存在上一个节点
        }

        PRINT_ERR("the detailed information of previous node:\n");
        OsMemDispMoreDetails(nodePre); //打印上一个节点的详细信息，用于诊断当前节点损坏情况
    }

    MEM_UNLOCK(intSave);
    PRINTK("check finish\n");
}

#else  /* without LOSCFG_MEM_HEAD_BACKUP */

//没有节点头部信息备份情况下的相关操作

//从空闲链表中移除
STATIC VOID OsMemListDelete(LOS_DL_LIST *node, const VOID *firstNode)
{
    (VOID)firstNode;
    LOS_ListDelete(node);
}

//添加到空闲链表中
STATIC VOID OsMemListAdd(LOS_DL_LIST *listNode, LOS_DL_LIST *node, const VOID *firstNode)
{
    (VOID)firstNode;
    LOS_ListHeadInsert(listNode, node);
}

#endif

#ifdef LOSCFG_EXC_INTERACTION  //支持系统异常状态下，shell仍然能受限使用
//将此内存池独立出来，是方便做故障隔离，因为另外一个内存池损坏概率大于我这个内存池
//我这边消耗的资源相对较少
LITE_OS_SEC_TEXT_INIT UINT32 OsMemExcInteractionInit(UINTPTR memStart)
{
    UINT32 ret;
    UINT32 poolSize;
    m_aucSysMem0 = (UINT8 *)((memStart + (POOL_ADDR_ALIGNSIZE - 1)) & ~((UINTPTR)(POOL_ADDR_ALIGNSIZE - 1)));
    poolSize = OS_EXC_INTERACTMEM_SIZE;
    ret = LOS_MemInit(m_aucSysMem0, poolSize); //初始化内存池
    PRINT_INFO("LiteOS kernel exc interaction memory address:%p,size:0x%x\n", m_aucSysMem0, poolSize);
    return ret;
}
#endif

//内存系统初始化
LITE_OS_SEC_TEXT_INIT UINT32 OsMemSystemInit(UINTPTR memStart)
{
    UINT32 ret;
    UINT32 poolSize;

    m_aucSysMem1 = (UINT8 *)((memStart + (POOL_ADDR_ALIGNSIZE - 1)) & ~((UINTPTR)(POOL_ADDR_ALIGNSIZE - 1)));
    poolSize = OS_SYS_MEM_SIZE;
    ret = LOS_MemInit(m_aucSysMem1, poolSize); //内核内存池初始化
    PRINT_INFO("LiteOS system heap memory address:%p,size:0x%x\n", m_aucSysMem1, poolSize);
#ifndef LOSCFG_EXC_INTERACTION
    m_aucSysMem0 = m_aucSysMem1; //如果不支持异常交互的话，内核中就1个内存池，否则分为2个内存池
#endif
    return ret;
}

#ifdef LOSCFG_MEM_LEAKCHECK
//内存泄露检测，记录申请内存时的函数调用栈
STATIC INLINE VOID OsMemLinkRegisterRecord(LosMemDynNode *node)
{
    UINT32 count = 0;
    UINT32 index = 0;
    UINTPTR framePtr, tmpFramePtr, linkReg;

    (VOID)memset_s(node->selfNode.linkReg, (LOS_RECORD_LR_CNT * sizeof(UINTPTR)), 0,
        (LOS_RECORD_LR_CNT * sizeof(UINTPTR)));
    framePtr = Get_Fp();  //获取当前栈帧地址
    while ((framePtr > OS_SYS_FUNC_ADDR_START) && (framePtr < OS_SYS_FUNC_ADDR_END)) {
        tmpFramePtr = framePtr;
#ifdef __LP64__
        framePtr = *(UINTPTR *)framePtr;
        linkReg = *(UINTPTR *)(tmpFramePtr + sizeof(UINTPTR)); //获取栈里面存储的LR的值
#else
        linkReg = *(UINTPTR *)framePtr;
        framePtr = *(UINTPTR *)(tmpFramePtr - sizeof(UINTPTR)); //获取栈里面存储的LR的值
#endif
        if (index >= LOS_OMIT_LR_CNT) {  //LOS_MemAlloc等函数的记录没有意义，应该记录调用这个函数的函数
            node->selfNode.linkReg[count++] = linkReg;  //把LR记录下来
            if (count == LOS_RECORD_LR_CNT) {
                break;  //目前记录3个LR, 即对应3个函数，一般来说足够识别内存是怎么申请的了
            }
        }
        index++; //更外层栈帧
    }
}
#endif

//空指针检查
#define OS_CHECK_NULL_RETURN(param) do {              \
    if ((param) == NULL) {                            \
        PRINT_ERR("%s %d\n", __FUNCTION__, __LINE__); \
        return;                                       \
    }                                                 \
} while (0);

/*
 * Description : find suitable free block use "best fit" algorithm
 * Input       : pool      --- Pointer to memory pool
 *               allocSize --- Size of memory in bytes which note need allocate
 * Return      : NULL      --- no suitable block found
 *               tmpNode   --- pointer a suitable free block
 */
 //寻找合适的空闲内存块
STATIC INLINE LosMemDynNode *OsMemFindSuitableFreeBlock(VOID *pool, UINT32 allocSize)
{
    LOS_DL_LIST *listNodeHead = NULL;
    LosMemDynNode *tmpNode = NULL;
	//遍历寻找次数不能太多，因为链表可能损坏，导致遍历链表死循环
	//需要避开死循环问题
    UINT32 maxCount = (LOS_MemPoolSizeGet(pool) / allocSize) << 1;
    UINT32 count;
#ifdef LOSCFG_MEM_HEAD_BACKUP
    UINT32 ret = LOS_OK;
#endif
	//从合适尺寸链表开始，依次遍历更大规格的链表，直到最大规格链表
    for (listNodeHead = OS_MEM_HEAD(pool, allocSize); listNodeHead != NULL;
         listNodeHead = OsDLnkNextMultiHead(OS_MEM_HEAD_ADDR(pool), listNodeHead)) {
        count = 0;
		//针对每一个链表，进行遍历
        LOS_DL_LIST_FOR_EACH_ENTRY(tmpNode, listNodeHead, LosMemDynNode, selfNode.freeNodeInfo) {
            if (count++ >= maxCount) {
				//可能死循环了，及时止损退出
                PRINT_ERR("[%s:%d]node: execute too much time\n", __FUNCTION__, __LINE__);
                break;
            }

#ifdef LOSCFG_MEM_HEAD_BACKUP
            if (!OsMemChecksumVerify(&tmpNode->selfNode)) {
				//当前节点损坏
                PRINT_ERR("[%s]: the node information of current node is bad !!\n", __FUNCTION__);
                OsMemDispCtlNode(&tmpNode->selfNode);
				//尝试修复
                ret = OsMemBackupRestore(pool, tmpNode);
            }
            if (ret != LOS_OK) {
                break; //修复失败
            }
#endif

            if (((UINTPTR)tmpNode & (OS_MEM_ALIGN_SIZE - 1)) != 0) {
				//当前节点不满足CPU的对齐要求
                LOS_Panic("[%s:%d]Mem node data error:OS_MEM_HEAD_ADDR(pool)=%p, listNodeHead:%p,"
                          "allocSize=%u, tmpNode=%p\n",
                          __FUNCTION__, __LINE__, OS_MEM_HEAD_ADDR(pool), listNodeHead, allocSize, tmpNode);
                break;
            }
            if (tmpNode->selfNode.sizeAndFlag >= allocSize) {
                return tmpNode; //找到了满足要求的内存节点
            }
        }
    }

    return NULL; //没有找到满足要求的内存节点
}

/*
 * Description : clear a mem node, set every member to NULL
 * Input       : node    --- Pointer to the mem node which will be cleared up
 */
 //清空内存节点头部信息
STATIC INLINE VOID OsMemClearNode(LosMemDynNode *node)
{
    (VOID)memset_s((VOID *)node, sizeof(LosMemDynNode), 0, sizeof(LosMemDynNode));
}

/*
 * Description : merge this node and pre node, then clear this node info
 * Input       : node    --- Pointer to node which will be merged
 */
 //将当前节点(空闲)合并到前一个节点上
STATIC INLINE VOID OsMemMergeNode(LosMemDynNode *node)
{
    LosMemDynNode *nextNode = NULL;

	//将当前节点的尺寸汇总到前一个节点上
    node->selfNode.preNode->selfNode.sizeAndFlag += node->selfNode.sizeAndFlag;
	//获取下一个节点
    nextNode = (LosMemDynNode *)((UINTPTR)node + node->selfNode.sizeAndFlag);
	//刷新下一个节点的前方节点
    nextNode->selfNode.preNode = node->selfNode.preNode;
#ifdef LOSCFG_MEM_HEAD_BACKUP
	//备份上一个节点
    OsMemNodeSave(node->selfNode.preNode);
	//备份下一个节点
    OsMemNodeSave(nextNode);
#endif
	//清除当前节点
    OsMemClearNode(node);
}

//此节点是否是外部申请的节点(不是通过当前内存池申请)
STATIC INLINE BOOL IsExpandPoolNode(VOID *pool, LosMemDynNode *node)
{
    UINTPTR start = (UINTPTR)pool;
    UINTPTR end = start + ((LosMemPoolInfo *)pool)->poolSize;
	//节点地址在当前内存池之外
    return ((UINTPTR)node < start) || ((UINTPTR)node > end);
}

/*
 * Description : split new node from allocNode, and merge remainder mem if necessary
 * Input       : pool      -- Pointer to memory pool
 *               allocNode -- the source node which new node be spit from to.
 *                            After pick up it's node info, change to point the new node
 *               allocSize -- the size of new node
 * Output      : allocNode -- save new node addr
 */
 //将一个大的内存节点分割成2个节点，并将其中一个归还会系统，
 //归还过程中处理合适的合并逻辑
STATIC INLINE VOID OsMemSplitNode(VOID *pool,
                                  LosMemDynNode *allocNode, UINT32 allocSize)
{
    LosMemDynNode *newFreeNode = NULL;
    LosMemDynNode *nextNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;
	//首节点
    const VOID *firstNode = (const VOID *)OS_MEM_FIRST_NODE(pool);

	//保留需要使用的部分，余下的就是空闲部分，构造一个空闲节点
    newFreeNode = (LosMemDynNode *)(VOID *)((UINT8 *)allocNode + allocSize);
	//空闲节点在当前节点之后
    newFreeNode->selfNode.preNode = allocNode;
	//空闲节点的尺寸计算
    newFreeNode->selfNode.sizeAndFlag = allocNode->selfNode.sizeAndFlag - allocSize;
	//需要使用的这个节点的尺寸设置
    allocNode->selfNode.sizeAndFlag = allocSize;
	//原大内存节点的下一个节点
    nextNode = OS_MEM_NEXT_NODE(newFreeNode);
	//成为空闲节点的下一个节点
    nextNode->selfNode.preNode = newFreeNode;
    if (!OS_MEM_NODE_GET_USED_FLAG(nextNode->selfNode.sizeAndFlag)) {
		//如果下一个节点也空闲的话，那么得和本次切割出去的空闲节点进行合并
        OsMemListDelete(&nextNode->selfNode.freeNodeInfo, firstNode); //先让其中空闲链表移除
        OsMemMergeNode(nextNode); //然后合并到我们分隔出来的空闲节点上
#ifdef LOSCFG_MEM_HEAD_BACKUP
    } else {
        OsMemNodeSave(nextNode);  //下一个节点在使用，但是其preNode字段发生了变化，所以需要重新备份
#endif
    }
	//将新的空闲节点放入合适的空闲链表--依据尺寸
    listNodeHead = OS_MEM_HEAD(pool, newFreeNode->selfNode.sizeAndFlag);
    OS_CHECK_NULL_RETURN(listNodeHead);
    /* add expand node to tail to make sure origin pool used first */
    if (IsExpandPoolNode(pool, newFreeNode)) {
		//如果节点不是本内存池申请的，那么放入空闲队列的尾部，我们未来优先使用本内存池中的节点
        OsMemListAdd(listNodeHead->pstPrev, &newFreeNode->selfNode.freeNodeInfo, firstNode);
    } else {
        OsMemListAdd(listNodeHead, &newFreeNode->selfNode.freeNodeInfo, firstNode);
    }

#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(newFreeNode);  //对新空闲节点进行备份
#endif
}

STATIC INLINE LosMemDynNode *PreSentinelNodeGet(const VOID *pool, const LosMemDynNode *node);
STATIC INLINE BOOL OsMemIsLastSentinelNode(LosMemDynNode *node);

//大内存节点的释放
UINT32 OsMemLargeNodeFree(const VOID *ptr)
{
	//大内存节点的申请以页为单位，所以直接按页释放
    LosVmPage *page = OsVmVaddrToPage((VOID *)ptr);
    if ((page == NULL) || (page->nPages == 0)) {
        return LOS_NOK;
    }
    LOS_PhysPagesFreeContiguous((VOID *)ptr, page->nPages); //释放申请到的连续内存页

    return LOS_OK;
}

//尝试释放离散内存节点
STATIC INLINE BOOL TryShrinkPool(const VOID *pool, const LosMemDynNode *node)
{
    LosMemDynNode *mySentinel = NULL;
    LosMemDynNode *preSentinel = NULL;
	//大内存块的数据节点大小
    size_t totalSize = (UINTPTR)node->selfNode.preNode - (UINTPTR)node;
	//本内存节点的大小
    size_t nodeSize = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);

    if (nodeSize != totalSize) {
        return FALSE;  //说明本内存块中目前不只一个内存节点，还不能回收
    }

	//本数据节点是本内存块最后一个节点
	//本节点释放后，整个内存块就可以释放了
	//找出描述本内存块的哨兵节点
    preSentinel = PreSentinelNodeGet(pool, node);
    if (preSentinel == NULL) {
        return FALSE;
    }

	//本内存块对应的哨兵节点
    mySentinel = node->selfNode.preNode;
    if (OsMemIsLastSentinelNode(mySentinel)) {
		//我是内存块链表中最后一个内存块
		//那么上一个内存块即将变成最后一个，更新这个信息到上一个哨兵节点中
		//哨兵节点中记录尺寸为0
        preSentinel->selfNode.sizeAndFlag = OS_MEM_NODE_USED_FLAG;
		//哨兵节点中记录地址也为0
        preSentinel->selfNode.freeNodeInfo.pstNext = NULL;
    } else {
		//我是中间某内存块(肯定不是第一个内存块(第1个内存块为初始化的内存池，一直占有不会释放))
		//通过修改上一个内存块的哨兵节点信息，将我移出链表
        preSentinel->selfNode.sizeAndFlag = mySentinel->selfNode.sizeAndFlag;
        preSentinel->selfNode.freeNodeInfo.pstNext = mySentinel->selfNode.freeNodeInfo.pstNext;
    }
	//释放我所在的内存块
    if (OsMemLargeNodeFree(node) != LOS_OK) {
        PRINT_ERR("TryShrinkPool free %p failed!\n", node);
        return FALSE;
    }

    return TRUE;
}

/*
 * Description : free the node from memory & if there are free node beside, merger them.
 *               at last update "listNodeHead' which saved all free node control head
 * Input       : node -- the node which need be freed
 *               pool -- Pointer to memory pool
 */
 //释放内存节点
STATIC INLINE VOID OsMemFreeNode(LosMemDynNode *node, VOID *pool)
{
    LosMemDynNode *preNode = NULL;
    LosMemDynNode *nextNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;
    const VOID *firstNode = (const VOID *)OS_MEM_FIRST_NODE(pool);
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;

#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//本内存节点释放后，内存池中内存使用量更新
    poolInfo->poolCurUsedSize -= OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);
#endif
    if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
		//更新任务占用的内存统计
        OS_MEM_REDUCE_USED(OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag), OS_MEM_TASKID_GET(node));
    }

	//标记内存节点为空闲
    node->selfNode.sizeAndFlag = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(node);  //先备份一下内存节点
#endif
#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemLinkRegisterRecord(node); //记录一下此内存释放过程的调用栈,方便分析内存泄露
#endif
    preNode = node->selfNode.preNode; /* merage preNode */
    if ((preNode != NULL) && !OS_MEM_NODE_GET_USED_FLAG(preNode->selfNode.sizeAndFlag)) {
		//如果上一个节点也空闲，那么可以合并到上一个节点
        OsMemListDelete(&(preNode->selfNode.freeNodeInfo), firstNode);
        OsMemMergeNode(node);
        node  = preNode; //合并后的节点
    }

    nextNode = OS_MEM_NEXT_NODE(node); /* merage nextNode */
    if ((nextNode != NULL) && !OS_MEM_NODE_GET_USED_FLAG(nextNode->selfNode.sizeAndFlag)) {
		//如果下一个节点也空闲，那么继续合并
        OsMemListDelete(&nextNode->selfNode.freeNodeInfo, firstNode);
        OsMemMergeNode(nextNode);
    }

    if (poolInfo->flag & MEM_POOL_EXPAND_ENABLE) {
        /* if this is a expand head node, and all unused, free it to pmm */
        if ((node->selfNode.preNode > node) && (node != firstNode)) {
			//说明这个内存节点是内存池外某一个大内存块中的第一个内存节点	
			//这个时候可以尝试释放这个大内存块
            if (TryShrinkPool(pool, node)) {
                return;
            }
        }
    }

	//将释放后的空闲数据节点放入合适空闲链表
    listNodeHead = OS_MEM_HEAD(pool, node->selfNode.sizeAndFlag);
    OS_CHECK_NULL_RETURN(listNodeHead);
    /* add expand node to tail to make sure origin pool used first */
    if (IsExpandPoolNode(pool, node)) {
		//对于内存池外扩展内存块中的数据节点不优先使用，我们还是优先使用内存池内的节点。
		//这样，可以加大它被合并的机会，从而进一步增加整个大内存块被回收的机会
        OsMemListAdd(listNodeHead->pstPrev, &node->selfNode.freeNodeInfo, firstNode);
    } else {
    	//内存池内的节点放入空闲链表的前部，后续优先使用
        OsMemListAdd(listNodeHead, &node->selfNode.freeNodeInfo, firstNode);
    }
}

/*
 * Description : check the result if pointer memory node belongs to pointer memory pool
 * Input       : pool -- Pointer to memory pool
 *               node -- the node which need be checked
 * Return      : LOS_OK or LOS_NOK
 */
#ifdef LOS_DLNK_SAFE_CHECK
//检查指定节点是否是本堆中的正在使用的节点，精确判断
STATIC INLINE UINT32 OsMemCheckUsedNode(const VOID *pool, const LosMemDynNode *node)
{
    LosMemDynNode *tmpNode = OS_MEM_FIRST_NODE(pool); //内存池第1个节点(数据)
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    LosMemDynNode *endNode = (const LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize); //内存池最后一个节点(哨兵)

    do {
		//遍历本内存块中的所有数据节点
        for (; tmpNode < endNode; tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
            if ((tmpNode == node) &&
                OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
                return LOS_OK; //找到对应的节点，且其不是空闲状态
            }
        }

		
        if (OsMemIsLastSentinelNode(endNode) == FALSE) {
			//如果不是最后一个内存块，那么继续找下一个内存块
            tmpNode = OsMemSentinelNodeGet(endNode);
			//以及其中的哨兵节点
            endNode = OS_MEM_END_NODE(tmpNode, OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag));
        } else {
            break;
        }
    } while (1);

    return LOS_NOK;  //本节点不属于堆空间的合法数据节点
}

#elif defined(LOS_DLNK_SIMPLE_CHECK)
//判断某节点是否本堆中的正在使用的节点，模糊判断
STATIC INLINE UINT32 OsMemCheckUsedNode(const VOID *pool, const LosMemDynNode *node)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    const LosMemDynNode *startNode = (const LosMemDynNode *)OS_MEM_FIRST_NODE(pool);
    const LosMemDynNode *endNode = (const LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize);
    if (!OS_MEM_MIDDLE_ADDR_OPEN_END(startNode, node, endNode)) {
		//不再内存池中
        return LOS_NOK;
    }

    if (!OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
		//空闲节点
        return LOS_NOK;
    }

    if (!OS_MEM_MAGIC_VALID(node->selfNode.freeNodeInfo.pstPrev)) {
		//分配时设置的魔数被改写
        return LOS_NOK;
    }

    return LOS_OK;
}

#else
//从某哨兵节点开始查找最后一个哨兵节点
STATIC LosMemDynNode *OsMemLastSentinelNodeGet(LosMemDynNode *sentinelNode)
{
    LosMemDynNode *node = NULL;
    VOID *ptr = sentinelNode->selfNode.freeNodeInfo.pstNext; //下一个内存块起始地址
    UINT32 size = OS_MEM_NODE_GET_SIZE(sentinelNode->selfNode.sizeAndFlag); //尺寸

    while ((ptr != NULL) && (size != 0)) {
		//下一个内存块存在的情况下，继续寻找下下一个内存块
        node = OS_MEM_END_NODE(ptr, size); //下一个哨兵节点
        ptr = node->selfNode.freeNodeInfo.pstNext; //下下内存块起始地址
        size = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag); //下下内存块尺寸
    }

    return node; //最后一个哨兵后面没有内存块
}

//哨兵节点完整性检查
STATIC INLINE BOOL OsMemSentinelNodeCheck(LosMemDynNode *node)
{
    if (!OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
        return FALSE; //哨兵节点一定不是空闲的
    }

    if (!OS_MEM_MAGIC_VALID(node->selfNode.freeNodeInfo.pstPrev)) {
        return FALSE; //哨兵节点魔数一定要正确
    }

    return TRUE;
}

//当前节点是否最后一个哨兵节点
STATIC INLINE BOOL OsMemIsLastSentinelNode(LosMemDynNode *node)
{
    if (OsMemSentinelNodeCheck(node) == FALSE) {
		//不是哨兵节点
        PRINT_ERR("%s %d, The current sentinel node is invalid\n", __FUNCTION__, __LINE__);
        return TRUE;
    }

    if ((OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag) == 0) ||
        (node->selfNode.freeNodeInfo.pstNext == NULL)) {
        return TRUE; //最后一个哨兵节点记录的尺寸为0，记录的地址也为0
    } else {
        return FALSE; //不是最后一个哨兵
    }
}


//在哨兵节点中记录下一个内存块的起始地址和尺寸
STATIC INLINE VOID OsMemSentinelNodeSet(LosMemDynNode *sentinelNode, VOID *newNode, UINT32 size)
{
    if (sentinelNode->selfNode.freeNodeInfo.pstNext != NULL) {
		//新的内存块需要放到尾部，所以需要在最后一个哨兵做记录
        sentinelNode = OsMemLastSentinelNodeGet(sentinelNode);
    }

	//设置文件魔数
    OS_MEM_SET_MAGIC(sentinelNode->selfNode.freeNodeInfo.pstPrev);
	//记录新内存块尺寸
    sentinelNode->selfNode.sizeAndFlag = size;
	//设置哨兵节点已使用标记
    OS_MEM_NODE_SET_USED_FLAG(sentinelNode->selfNode.sizeAndFlag);
	//设置新内存块起始地址
    sentinelNode->selfNode.freeNodeInfo.pstNext = newNode;
}

//根据哨兵节点获取下一个内存块首地址
STATIC INLINE VOID *OsMemSentinelNodeGet(LosMemDynNode *node)
{
    if (OsMemSentinelNodeCheck(node) == FALSE) {
        return NULL;  //必须是哨兵节点才能读取下一个内存块
    }

    return node->selfNode.freeNodeInfo.pstNext;
}

//获取当前节点的前一个哨兵节点
STATIC INLINE LosMemDynNode *PreSentinelNodeGet(const VOID *pool, const LosMemDynNode *node)
{
    UINT32 nextSize;
    LosMemDynNode *nextNode = NULL;
    LosMemDynNode *sentinelNode = NULL;

	//初始哨兵节点是内存池尾节点
    sentinelNode = OS_MEM_END_NODE(pool, ((LosMemPoolInfo *)pool)->poolSize);
    while (sentinelNode != NULL) {
        if (OsMemIsLastSentinelNode(sentinelNode)) {
			//如果找完了最后一个哨兵节点，还未找到，则直接返回
            PRINT_ERR("PreSentinelNodeGet can not find node %p\n", node);
            return NULL;
        }
        nextNode = OsMemSentinelNodeGet(sentinelNode); //下一个内存块
        if (nextNode == node) {
            return sentinelNode; //上一个哨兵节点的下一个节点是自己
        }
		//下一个内存块的尺寸
        nextSize = OS_MEM_NODE_GET_SIZE(sentinelNode->selfNode.sizeAndFlag);
		//下一个内存块中的哨兵节点
        sentinelNode = OS_MEM_END_NODE(nextNode, nextSize);
    }

    return NULL;
}

//判断此地址是否为堆空间中的节点地址
BOOL OsMemIsHeapNode(const VOID *ptr)
{
    LosMemPoolInfo *pool = (LosMemPoolInfo *)m_aucSysMem1;
    LosMemDynNode *firstNode = OS_MEM_FIRST_NODE(pool);
    LosMemDynNode *endNode = OS_MEM_END_NODE(pool, pool->poolSize);
    UINT32 intSave;
    UINT32 size;

    if (OS_MEM_MIDDLE_ADDR(firstNode, ptr, endNode)) {
        return TRUE; //如果在内存池中，肯定是堆空间中
    }

    MEM_LOCK(intSave);
	//然后遍历哨兵节点维护的内存块链表
    while (OsMemIsLastSentinelNode(endNode) == FALSE) {
        size = OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
        firstNode = OsMemSentinelNodeGet(endNode);
        endNode = OS_MEM_END_NODE(firstNode, size);
        if (OS_MEM_MIDDLE_ADDR(firstNode, ptr, endNode)) {
			//如果地址在这些内存块中，也算
            MEM_UNLOCK(intSave);
            return TRUE;
        }
    }
    MEM_UNLOCK(intSave);

    return FALSE;
}


//对内存地址进行合法性检查
STATIC BOOL OsMemAddrValidCheck(const LosMemPoolInfo *pool, const VOID *addr)
{
    UINT32 size;
    LosMemDynNode *node = NULL;
    LosMemDynNode *sentinel = NULL;

    size = ((LosMemPoolInfo *)pool)->poolSize;
    if (OS_MEM_MIDDLE_ADDR_OPEN_END(pool + 1, addr, (UINTPTR)pool + size)) {
        return TRUE; //地址在内存池中，合法
    }

	//遍历哨兵节点维护的内存块链表
    sentinel = OS_MEM_END_NODE(pool, size);
    while (OsMemIsLastSentinelNode(sentinel) == FALSE) {
        size = OS_MEM_NODE_GET_SIZE(sentinel->selfNode.sizeAndFlag);
        node = OsMemSentinelNodeGet(sentinel);
        sentinel = OS_MEM_END_NODE(node, size);
        if (OS_MEM_MIDDLE_ADDR_OPEN_END(node, addr, (UINTPTR)node + size)) {
			//地址在内存块中，也合法
            return TRUE;
        }
    }

    return FALSE;
}


//node是否合法的节点
STATIC INLINE BOOL OsMemIsNodeValid(const LosMemDynNode *node, const LosMemDynNode *startNode,
                                    const LosMemDynNode *endNode,
                                    const LosMemPoolInfo *poolInfo)
{
    if (!OS_MEM_MIDDLE_ADDR(startNode, node, endNode)) {
        return FALSE; //node必须在start和endNode之间
    }

    if (OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
		//非空闲node
        if (!OS_MEM_MAGIC_VALID(node->selfNode.freeNodeInfo.pstPrev)) {
            return FALSE; //那么其魔数一定要正确
        }
        return TRUE;
    }

	//空闲节点
    if (!OsMemAddrValidCheck(poolInfo, node->selfNode.freeNodeInfo.pstPrev)) {
        return FALSE;  //空闲节点的前一个节点也必须合法
    }

    return TRUE;
}

//检查一个非空闲的节点的合法性
STATIC INLINE UINT32 OsMemCheckUsedNode(const VOID *pool, const LosMemDynNode *node)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    LosMemDynNode *startNode = (LosMemDynNode *)OS_MEM_FIRST_NODE(pool);
    LosMemDynNode *endNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize);
    LosMemDynNode *nextNode = NULL;
    BOOL doneFlag = FALSE;

    do {
        do {
            if (!OsMemIsNodeValid(node, startNode, endNode, poolInfo)) {
                break; //所考察的节点需要在起始节点和结束节点之间
            }

            if (!OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
                break; //所考察的节点不能是空闲节点
            }

            nextNode = OS_MEM_NEXT_NODE(node);
            if (!OsMemIsNodeValid(nextNode, startNode, endNode, poolInfo)) {
                break; //它的下一个节点也要是一个合法节点
            }

            if (nextNode->selfNode.preNode != node) {
                break; //nextNode的前一个节点应该是node才对
            }

            if ((node != startNode) &&
                ((!OsMemIsNodeValid(node->selfNode.preNode, startNode, endNode, poolInfo)) ||
                (OS_MEM_NEXT_NODE(node->selfNode.preNode) != node))) {
                //如果不是首节点，那么它的上一个节点应该也是合法的
                //且上一个节点对应的下一个节点就应该是我
                break;
            }
			//本内存节点通过了各项检查
            doneFlag = TRUE;
        } while (0);

        if (!doneFlag) {
			//如果上述检查没有通过，那么我们在其它额外申请的内存块中去查找看看
            if (OsMemIsLastSentinelNode(endNode) == FALSE) {
				//找下一个内存块
				//下一个内存块中的起始节点
                startNode = OsMemSentinelNodeGet(endNode);
				//下一个内存块中的结束节点(哨兵节点)
                endNode = OS_MEM_END_NODE(startNode, OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag));
                continue;  //进行下一轮判断
            }
			//所有内存块都查看过，没有通过检查，那么说明这个节点不合法
            return LOS_NOK;
        }
    } while (!doneFlag);

    return LOS_OK;  //存在某内存块，使得上述内存节点合法
}

#endif

/*
 * Description : set magic & taskid
 * Input       : node -- the node which will be set magic & taskid
 */
STATIC INLINE VOID OsMemSetMagicNumAndTaskID(LosMemDynNode *node)
{
    LosTaskCB *runTask = OsCurrTaskGet();

	//当前节点不在空闲链中了，可以重用pstPrev来设置魔数
    OS_MEM_SET_MAGIC(node->selfNode.freeNodeInfo.pstPrev);

    /*
     * If the operation occured before task initialization(runTask was not assigned)
     * or in interrupt, make the value of taskid of node to 0xffffffff
     */
    if ((runTask != NULL) && OS_INT_INACTIVE) {
		//在任务上下文，那么设置节点使用者的任务ID，存入pstNext字段
        OS_MEM_TASKID_SET(node, runTask->taskID);
    } else {
        /* If the task mode does not initialize, the field is the 0xffffffff */
		//中断上下文或者无task的情况，设置成特殊值
        node->selfNode.freeNodeInfo.pstNext = (LOS_DL_LIST *)OS_NULL_INT;
    }
}


//检查内存池中的双链表的合法性
LITE_OS_SEC_TEXT_MINOR STATIC INLINE UINT32 OsMemPoolDlinkcheck(const LosMemPoolInfo *pool, LOS_DL_LIST listHead)
{
    if (!OsMemAddrValidCheck(pool, listHead.pstPrev) ||
        !OsMemAddrValidCheck(pool, listHead.pstNext)) {
        return LOS_NOK; //链表头部和尾部节点都必须在内存池中
    }

    if (!IS_ALIGNED(listHead.pstPrev, sizeof(VOID *)) ||
        !IS_ALIGNED(listHead.pstNext, sizeof(VOID *))) {
        return LOS_NOK; //并且必须对齐到指针变量上
    }

    return LOS_OK;
}

/*
 * Description : show mem pool header info
 * Input       : pool --Pointer to memory pool
 */
 //打印输出内存池各空闲链表头信息
LITE_OS_SEC_TEXT_MINOR VOID OsMemPoolHeadInfoPrint(const VOID *pool)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    UINT32 dlinkNum;
    UINT32 flag = 0;
    const LosMultipleDlinkHead *dlinkHead = NULL;

    if (!IS_ALIGNED(poolInfo, sizeof(VOID *))) {
		//内存池控制块首地址必须按指针要求自然对齐
        PRINT_ERR("wrong mem pool addr: %p, func:%s,line:%d\n", poolInfo, __FUNCTION__, __LINE__);
#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("wrong mem pool addr: %p, func:%s,line:%d\n", poolInfo, __FUNCTION__, __LINE__);
#endif
        return;
    }

	//检查各空闲队列
    dlinkHead = (const LosMultipleDlinkHead *)(VOID *)(poolInfo + 1);
    for (dlinkNum = 0; dlinkNum < OS_MULTI_DLNK_NUM; dlinkNum++) {
        if (OsMemPoolDlinkcheck(pool, dlinkHead->listHead[dlinkNum])) {
            flag = 1;
			//如果空闲队列头部异常，那么需要输出错误日志
            PRINT_ERR("DlinkHead[%u]: pstPrev:%p, pstNext:%p\n",
                      dlinkNum, dlinkHead->listHead[dlinkNum].pstPrev, dlinkHead->listHead[dlinkNum].pstNext);
#ifdef LOSCFG_SHELL_EXCINFO
            WriteExcInfoToBuf("DlinkHead[%u]: pstPrev:%p, pstNext:%p\n",
                              dlinkNum, dlinkHead->listHead[dlinkNum].pstPrev, dlinkHead->listHead[dlinkNum].pstNext);
#endif
        }
    }
    if (flag) {
		//存在空闲队列异常的情况下
		//补充输出内存池的整体信息
        PRINTK("mem pool info: poolAddr:%p, poolSize:0x%x\n", poolInfo->pool, poolInfo->poolSize);
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
        PRINTK("mem pool info: poolWaterLine:0x%x, poolCurUsedSize:0x%x\n", poolInfo->poolWaterLine,
               poolInfo->poolCurUsedSize);
#endif

#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("mem pool info: poolAddr:%p, poolSize:0x%x\n", poolInfo->pool, poolInfo->poolSize);
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
        WriteExcInfoToBuf("mem pool info: poolWaterLine:0x%x, poolCurUsedSize:0x%x\n",
                          poolInfo->poolWaterLine, poolInfo->poolCurUsedSize);
#endif
#endif
    }
}


//魔数检查失败，并输出错误日志
STATIC VOID OsMemMagicCheckPrint(LosMemDynNode **tmpNode)
{
    PRINT_ERR("[%s], %d, memory check error!\n"
              "memory used but magic num wrong, freeNodeInfo.pstPrev(magic num):%p \n",
              __FUNCTION__, __LINE__, (*tmpNode)->selfNode.freeNodeInfo.pstPrev);
#ifdef LOSCFG_SHELL_EXCINFO
    WriteExcInfoToBuf("[%s], %d, memory check error!\n"
                      "memory used but magic num wrong, freeNodeInfo.pstPrev(magic num):%p \n",
                      __FUNCTION__, __LINE__, (*tmpNode)->selfNode.freeNodeInfo.pstPrev);
#endif
}

STATIC UINT32 OsMemAddrValidCheckPrint(const VOID *pool, const LosMemDynNode *endPool, LosMemDynNode **tmpNode)
{
    if (!OsMemAddrValidCheck(pool, (*tmpNode)->selfNode.freeNodeInfo.pstPrev)) {
        PRINT_ERR("[%s], %d, memory check error!\n"
                  " freeNodeInfo.pstPrev:%p is out of legal mem range[%p, %p]\n",
                  __FUNCTION__, __LINE__, (*tmpNode)->selfNode.freeNodeInfo.pstPrev, pool, endPool);
#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("[%s], %d, memory check error!\n"
                          " freeNodeInfo.pstPrev:%p is out of legal mem range[%p, %p]\n",
                          __FUNCTION__, __LINE__, (*tmpNode)->selfNode.freeNodeInfo.pstPrev, pool, endPool);
#endif
        return LOS_NOK;
    }
    if (!OsMemAddrValidCheck(pool, (*tmpNode)->selfNode.freeNodeInfo.pstNext)) {
        PRINT_ERR("[%s], %d, memory check error!\n"
                  " freeNodeInfo.pstNext:%p is out of legal mem range[%p, %p]\n",
                  __FUNCTION__, __LINE__, (*tmpNode)->selfNode.freeNodeInfo.pstNext, pool, endPool);
#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("[%s], %d, memory check error!\n"
                          " freeNodeInfo.pstNext:%p is out of legal mem range[%p, %p]\n",
                          __FUNCTION__, __LINE__, (*tmpNode)->selfNode.freeNodeInfo.pstNext, pool, endPool);
#endif
        return LOS_NOK;
    }
    return LOS_OK;
}

//内存节点完整性检查
STATIC UINT32 DoOsMemIntegrityCheck(LosMemDynNode **tmpNode, const VOID *pool, const LosMemDynNode *endPool)
{
    if (OS_MEM_NODE_GET_USED_FLAG((*tmpNode)->selfNode.sizeAndFlag)) {
		//已分配节点
        if (!OS_MEM_MAGIC_VALID((*tmpNode)->selfNode.freeNodeInfo.pstPrev)) {
			//魔数错误
            OsMemMagicCheckPrint(tmpNode);
            return LOS_NOK;
        }
    } else { /* is free node, check free node range */
    	//空闲节点
        if (OsMemAddrValidCheckPrint(pool, endPool, tmpNode) == LOS_NOK) {
			//空闲节点的next , prev需要合法
            return LOS_NOK;
        }
    }
    return LOS_OK;
}


//节点完整性检查
STATIC UINT32 OsMemIntegrityCheck(const VOID *pool, LosMemDynNode **tmpNode, LosMemDynNode **preNode)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    LosMemDynNode *endPool = (LosMemDynNode *)((UINT8 *)pool + poolInfo->poolSize - OS_MEM_NODE_HEAD_SIZE);

    OsMemPoolHeadInfoPrint(pool); //内存池头部，空闲链表头部检查，发现异常则打印错误日志
    *preNode = OS_MEM_FIRST_NODE(pool); //初始化为链表头部

    do {
		//遍历本内存块中的数据节点
        for (*tmpNode = *preNode; *tmpNode < endPool; *tmpNode = OS_MEM_NEXT_NODE(*tmpNode)) {
			//检查数据节点完整性，并打印错误日志
            if (DoOsMemIntegrityCheck(tmpNode, pool, endPool) == LOS_NOK) {
				//数据节点出错，则返回
                return LOS_NOK;
            }
            *preNode = *tmpNode;
        }

		//如果不是最后一个内存块
        if (OsMemIsLastSentinelNode(*tmpNode) == FALSE) {
			//则获取下一个内存块
            *preNode = OsMemSentinelNodeGet(*tmpNode);
            endPool = OS_MEM_END_NODE(*preNode, OS_MEM_NODE_GET_SIZE((*tmpNode)->selfNode.sizeAndFlag));
        } else {
            break; //遍历完了所有的内存块
        }
    } while (1);

    return LOS_OK;
}

#ifdef LOSCFG_MEM_LEAKCHECK
//检测内存泄露后，输出相关的诊断辅助信息，供人工判断
STATIC VOID OsMemNodeBacktraceInfo(const LosMemDynNode *tmpNode,
                                   const LosMemDynNode *preNode)
{
    int i;
    PRINTK("\n broken node head LR info: \n");
	//损毁节点对应的调用栈回溯
    for (i = 0; i < LOS_RECORD_LR_CNT; i++) {
        PRINTK(" LR[%d]:%p\n", i, tmpNode->selfNode.linkReg[i]);
    }
    PRINTK("\n pre node head LR info: \n");
	//上一个节点的调用栈回溯
    for (i = 0; i < LOS_RECORD_LR_CNT; i++) {
        PRINTK(" LR[%d]:%p\n", i, preNode->selfNode.linkReg[i]);
    }

#ifdef LOSCFG_SHELL_EXCINFO
    WriteExcInfoToBuf("\n broken node head LR info: \n");
    for (i = 0; i < LOS_RECORD_LR_CNT; i++) {
        WriteExcInfoToBuf("LR[%d]:%p\n", i, tmpNode->selfNode.linkReg[i]);
    }
    WriteExcInfoToBuf("\n pre node head LR info: \n");
    for (i = 0; i < LOS_RECORD_LR_CNT; i++) {
        WriteExcInfoToBuf("LR[%d]:%p\n", i, preNode->selfNode.linkReg[i]);
    }
#endif
}
#endif


//异常节点信息输出
STATIC VOID OsMemNodeInfo(const LosMemDynNode *tmpNode,
                          const LosMemDynNode *preNode)
{
    if (tmpNode == preNode) {
        PRINTK("\n the broken node is the first node\n");
#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("\n the broken node is the first node\n");
#endif
    }
    PRINTK("\n broken node head: %p  %p  %p  0x%x, pre node head: %p  %p  %p  0x%x\n",
           tmpNode->selfNode.freeNodeInfo.pstPrev, tmpNode->selfNode.freeNodeInfo.pstNext,
           tmpNode->selfNode.preNode, tmpNode->selfNode.sizeAndFlag,
           preNode->selfNode.freeNodeInfo.pstPrev, preNode->selfNode.freeNodeInfo.pstNext,
           preNode->selfNode.preNode, preNode->selfNode.sizeAndFlag);

#ifdef LOSCFG_SHELL_EXCINFO
    WriteExcInfoToBuf("\n broken node head: %p  %p  %p  0x%x, pre node head: %p  %p  %p  0x%x\n",
                      tmpNode->selfNode.freeNodeInfo.pstPrev, tmpNode->selfNode.freeNodeInfo.pstNext,
                      tmpNode->selfNode.preNode, tmpNode->selfNode.sizeAndFlag,
                      preNode->selfNode.freeNodeInfo.pstPrev, preNode->selfNode.freeNodeInfo.pstNext,
                      preNode->selfNode.preNode, preNode->selfNode.sizeAndFlag);
#endif
#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemNodeBacktraceInfo(tmpNode, preNode);
#endif

    PRINTK("\n---------------------------------------------\n");
    PRINTK(" dump mem tmpNode:%p ~ %p\n", tmpNode, ((UINTPTR)tmpNode + NODEDUMPSIZE));
    OsDumpMemByte(NODEDUMPSIZE, (UINTPTR)tmpNode);
    PRINTK("\n---------------------------------------------\n");
    if (preNode != tmpNode) {
        PRINTK(" dump mem :%p ~ tmpNode:%p\n", ((UINTPTR)tmpNode - NODEDUMPSIZE), tmpNode);
        OsDumpMemByte(NODEDUMPSIZE, ((UINTPTR)tmpNode - NODEDUMPSIZE));
        PRINTK("\n---------------------------------------------\n");
    }
}

STATIC VOID OsMemIntegrityCheckError(const LosMemDynNode *tmpNode,
                                     const LosMemDynNode *preNode,
                                     UINT32 intSave)
{
    LosTaskCB *taskCB = NULL;
    UINT32 taskID;

    OsMemNodeInfo(tmpNode, preNode);

    taskID = OS_MEM_TASKID_GET(preNode);
    if (OS_TID_CHECK_INVALID(taskID)) {
#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("Task ID %u in pre node is invalid!\n", taskID);
#endif
        MEM_UNLOCK(intSave);
        LOS_Panic("Task ID %u in pre node is invalid!\n", taskID);
        return;
    }

    taskCB = OS_TCB_FROM_TID(taskID);
    if (OsTaskIsUnused(taskCB) || (taskCB->taskEntry == NULL)) {
#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("\r\nTask ID %u in pre node is not created or deleted!\n", taskID);
#endif
        MEM_UNLOCK(intSave);
        LOS_Panic("\r\nTask ID %u in pre node is not created!\n", taskID);
        return;
    }
#ifdef LOSCFG_SHELL_EXCINFO
    WriteExcInfoToBuf("cur node: %p\npre node: %p\npre node was allocated by task:%s\n",
                      tmpNode, preNode, taskCB->taskName);
#endif
    MEM_UNLOCK(intSave);
    LOS_Panic("cur node: %p\npre node: %p\npre node was allocated by task:%s\n",
              tmpNode, preNode, taskCB->taskName);
    return;
}

/*
 * Description : memory pool integrity checking
 * Input       : pool --Pointer to memory pool
 * Return      : LOS_OK --memory pool integrate or LOS_NOK--memory pool impaired
 */
 //内存池正确性检查，含所有的数据节点
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemIntegrityCheck(const VOID *pool)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemDynNode *preNode = NULL;
    UINT32 intSave;

    if (pool == NULL) {
        return LOS_NOK;
    }

    MEM_LOCK(intSave);
    if (OsMemIntegrityCheck(pool, &tmpNode, &preNode)) {
        goto ERROR_OUT;
    }
    MEM_UNLOCK(intSave);
    return LOS_OK;

ERROR_OUT:
    OsMemIntegrityCheckError(tmpNode, preNode, intSave);
    return LOS_NOK;
}

STATIC INLINE VOID OsMemNodeDebugOperate(VOID *pool, LosMemDynNode *allocNode, UINT32 size)
{
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    poolInfo->poolCurUsedSize += OS_MEM_NODE_GET_SIZE(allocNode->selfNode.sizeAndFlag);
    if (poolInfo->poolCurUsedSize > poolInfo->poolWaterLine) {
        poolInfo->poolWaterLine = poolInfo->poolCurUsedSize;
    }
#endif

#ifdef LOSCFG_MEM_RECORDINFO
    allocNode->selfNode.originSize = size;
#endif

#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(allocNode);
#endif

#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemLinkRegisterRecord(allocNode);
#endif
}


//内存池中没有找到合适的节点，去内存池外申请节点
//相当于扩大了内存池
STATIC INLINE INT32 OsMemPoolExpand(VOID *pool, UINT32 size, UINT32 intSave)
{
    UINT32 tryCount = MAX_SHRINK_PAGECACHE_TRY;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *newNode = NULL;
    LosMemDynNode *endNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;

	//这里额外申请了OS_MEM_NODE_HEAD_SIZE字节，额外申请的部分用来充当哨兵节点
	//哨兵节点的主要作用是为了记录数据节点(用户实际使用的)的元数据。如实际数据节点的起始地址和尺寸
	//并让离散的内存节点链接起来形成单链表(前后关系)
    size = ROUNDUP(size + OS_MEM_NODE_HEAD_SIZE, PAGE_SIZE);  //新申请的内存大小要为若干内存页
    //原内存池的尾节点
    endNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize);

RETRY:
	//新申请一块内存(若干内存页)
    newNode = (LosMemDynNode *)LOS_PhysPagesAllocContiguous(size >> PAGE_SHIFT);
    if (newNode == NULL) {
		//申请失败，则释放一些物理页再重试几次
        if (tryCount > 0) {
            tryCount--;
            MEM_UNLOCK(intSave);
            OsTryShrinkMemory(size >> PAGE_SHIFT);
            MEM_LOCK(intSave);
            goto RETRY;
        }

		//确实无法申请到了，内存耗尽状态或者size太大
        PRINT_ERR("OsMemPoolExpand alloc failed size = %u\n", size);
        return -1;
    }
	//成功申请到以后，
	//新节点占用若干内存页，除了头部信息都是可用的数据部分
    newNode->selfNode.sizeAndFlag = (size - OS_MEM_NODE_HEAD_SIZE);	
	//对于离散存储的内存节点，获取当前节点的前一个节点没有什么实际的意义
	//这里通过将此字段设置成本节点后的哨兵节点(会导致preNode > newNode)
	//其目的是通过node中存储的preNode字段来区分node是在连续内存中，还是在离散内存中
	//所以哨兵节点的意义还是非常大的。另外，通过哨兵节点，可以将离散的内存节点链接起来。
    newNode->selfNode.preNode = (LosMemDynNode *)OS_MEM_END_NODE(newNode, size);
	//寻找合适的链表插入
    listNodeHead = OS_MEM_HEAD(pool, newNode->selfNode.sizeAndFlag);
    if (listNodeHead == NULL) {
        return -1;
    }
	//先将本节点信息放入哨兵链表
    OsMemSentinelNodeSet(endNode, newNode, size);
	//然后将本节点放入空闲链表
    LOS_ListTailInsert(listNodeHead, &(newNode->selfNode.freeNodeInfo));

	//本节点的末尾部分用来充当哨兵
    endNode = (LosMemDynNode *)OS_MEM_END_NODE(newNode, size);
    (VOID)memset_s(endNode, sizeof(*endNode), 0, sizeof(*endNode));

	//哨兵节点的前一个节点就是本节点	
    endNode->selfNode.preNode = newNode;
	//新哨兵节点后面没有节点
    OsMemSentinelNodeSet(endNode, NULL, 0);
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    poolInfo->poolCurUsedSize = sizeof(LosMemPoolInfo) + OS_MULTI_DLNK_HEAD_SIZE +
                                OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
    poolInfo->poolWaterLine = poolInfo->poolCurUsedSize;
#endif
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(newNode);
    OsMemNodeSave(endNode);
#endif
    return 0;
}

//设置内存池为可扩展的
VOID LOS_MemExpandEnable(VOID *pool)
{
    if (pool == NULL) {
        return;
    }

    ((LosMemPoolInfo *)pool)->flag = MEM_POOL_EXPAND_ENABLE;
}

#ifdef LOSCFG_BASE_MEM_NODE_INTEGRITY_CHECK
//申请内存前，先做一次内存检查，性能较低，可靠性较高
STATIC INLINE UINT32 OsMemAllocCheck(VOID *pool, UINT32 intSave)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemDynNode *preNode = NULL;

    if (OsMemIntegrityCheck(pool, &tmpNode, &preNode)) {
        OsMemIntegrityCheckError(tmpNode, preNode, intSave);
        return LOS_NOK;
    }
    return LOS_OK;
}
#else
STATIC INLINE UINT32 OsMemAllocCheck(VOID *pool, UINT32 intSave)
{
    return LOS_OK;
}

#endif


/*
 * Description : Allocate node from Memory pool
 * Input       : pool  --- Pointer to memory pool
 *               size  --- Size of memory in bytes to allocate
 * Return      : Pointer to allocated memory
 */
 //在内存池pool中申请size字节的内存节点
STATIC INLINE VOID *OsMemAllocWithCheck(VOID *pool, UINT32 size, UINT32 intSave)
{
    LosMemDynNode *allocNode = NULL;
    UINT32 allocSize;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    const VOID *firstNode = (const VOID *)((UINT8 *)OS_MEM_HEAD_ADDR(pool) + OS_DLNK_HEAD_SIZE);
    INT32 ret;

	//如果开启了内存检测功能，则检查内存池的完整和正确性
    if (OsMemAllocCheck(pool, intSave) == LOS_NOK) {
        return NULL;
    }

	//申请的内存节点需要额外包含一个控制头，且需要对齐在4字节边界(32位CPU上)
    allocSize = OS_MEM_ALIGN(size + OS_MEM_NODE_HEAD_SIZE, OS_MEM_ALIGN_SIZE);
    if (allocSize == 0) {
        return NULL;
    }
retry:

	//尝试从空闲队列里面取一个满足要求的内存节点
    allocNode = OsMemFindSuitableFreeBlock(pool, allocSize);
    if (allocNode == NULL) {
		//如果当前没有满足要求的节点		
        if (poolInfo->flag & MEM_POOL_EXPAND_ENABLE) {
			//那么如果内存池支持扩容的话，则继续申请离散的内存块
			//申请到的内存块放入空闲队列
            ret = OsMemPoolExpand(pool, allocSize, intSave);
            if (ret == 0) {
                goto retry; //然后重试
            }
        }
		//无法申请到需要的内存块，打印错误日志
        PRINT_ERR("---------------------------------------------------"
                  "--------------------------------------------------------\n");
        MEM_UNLOCK(intSave);
        OsMemInfoPrint(pool);
        MEM_LOCK(intSave);
        PRINT_ERR("[%s] No suitable free block, require free node size: 0x%x\n", __FUNCTION__, allocSize);
        PRINT_ERR("----------------------------------------------------"
                  "-------------------------------------------------------\n");
        return NULL;
    }
    if ((allocSize + OS_MEM_NODE_HEAD_SIZE + OS_MEM_ALIGN_SIZE) <= allocNode->selfNode.sizeAndFlag) {
		//当从空闲队列中取出的内存块较大，超出了需求，则进行切割，多余的部分放回空闲队列
        OsMemSplitNode(pool, allocNode, allocSize);
    }
	//从空闲队列里取出刚好满足需求的内存节点
    OsMemListDelete(&allocNode->selfNode.freeNodeInfo, firstNode);
	//设置魔数和任务ID
    OsMemSetMagicNumAndTaskID(allocNode);
	//设置内存节点为已使用
    OS_MEM_NODE_SET_USED_FLAG(allocNode->selfNode.sizeAndFlag);
    if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
		//增加相关任务内存占用运维统计
        OS_MEM_ADD_USED(OS_MEM_NODE_GET_SIZE(allocNode->selfNode.sizeAndFlag), OS_MEM_TASKID_GET(allocNode));
    }
	//处理此内存节点调试和运维相关的逻辑
    OsMemNodeDebugOperate(pool, allocNode, size);
    return (allocNode + 1); //返回此内存节点数据部分首地址
}

/*
 * Description : reAlloc a smaller memory node
 * Input       : pool      --- Pointer to memory pool
 *               allocSize --- the size of new node which will be alloced
 *               node      --- the node which wille be realloced
 *               nodeSize  --- the size of old node
 * Output      : node      --- pointer to the new node after realloc
 */
 //重新申请一个更小的内存块
STATIC INLINE VOID OsMemReAllocSmaller(VOID *pool, UINT32 allocSize, LosMemDynNode *node, UINT32 nodeSize)
{
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
#endif
    if ((allocSize + OS_MEM_NODE_HEAD_SIZE + OS_MEM_ALIGN_SIZE) <= nodeSize) {
		//新内存节点比原内存节点小
        node->selfNode.sizeAndFlag = nodeSize;
		//直接在原内存节点上切割，多余部分放回空闲队列
        OsMemSplitNode(pool, node, allocSize);
		//满足要求的这个节点设置成已使用
        OS_MEM_NODE_SET_USED_FLAG(node->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_HEAD_BACKUP
        OsMemNodeSave(node); //备份节点头部信息
#endif
        if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
			//任务占用的内存节点变小
            OS_MEM_REDUCE_USED(nodeSize - allocSize, OS_MEM_TASKID_GET(node));
        }
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
		//内存池占用的内存减少
        poolInfo->poolCurUsedSize -= nodeSize - allocSize;
#endif
    }
#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemLinkRegisterRecord(node); //记录调用栈回溯信息
#endif
}

/*
 * Description : reAlloc a Bigger memory node after merge node and nextNode
 * Input       : pool      --- Pointer to memory pool
 *               allocSize --- the size of new node which will be alloced
 *               node      --- the node which wille be realloced
 *               nodeSize  --- the size of old node
 *               nextNode  --- pointer next node which will be merged
 * Output      : node      --- pointer to the new node after realloc
 */
 //重申请一个更大的内存节点
STATIC INLINE VOID OsMemMergeNodeForReAllocBigger(VOID *pool, UINT32 allocSize, LosMemDynNode *node,
                                                  UINT32 nodeSize, LosMemDynNode *nextNode)
{
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
#endif
	//内存池中第一个节点
    const VOID *firstNode = (const VOID *)((UINT8 *)OS_MEM_HEAD_ADDR(pool) + OS_DLNK_HEAD_SIZE);

	//先将原节点置成空闲节点
    node->selfNode.sizeAndFlag = nodeSize;
	//将后一个节点从空闲队列取出
    OsMemListDelete(&nextNode->selfNode.freeNodeInfo, firstNode);
	//然后将2个节点合并
    OsMemMergeNode(nextNode);
    if ((allocSize + OS_MEM_NODE_HEAD_SIZE + OS_MEM_ALIGN_SIZE) <= node->selfNode.sizeAndFlag) {
		//合并后的节点尺寸超过了需求，把多余的部分裁剪掉并放回空闲队列
        OsMemSplitNode(pool, node, allocSize);
    }
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//记录内存池占用增量
    poolInfo->poolCurUsedSize += (node->selfNode.sizeAndFlag - nodeSize);
    if (poolInfo->poolCurUsedSize > poolInfo->poolWaterLine) {
		//刷新水线峰值
        poolInfo->poolWaterLine = poolInfo->poolCurUsedSize;
    }
#endif
    if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
		//统计任务占用
        OS_MEM_ADD_USED(node->selfNode.sizeAndFlag - nodeSize, OS_MEM_TASKID_GET(node));
    }
	//将节点标记成已使用
    OS_MEM_NODE_SET_USED_FLAG(node->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(node); //备份节点头部信息
#endif
#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemLinkRegisterRecord(node); //记录调用栈回溯
#endif
}

#ifdef LOSCFG_MEM_MUL_POOL
//支持多内存池时，添加新的内存池
STATIC UINT32 OsMemPoolAdd(VOID *pool, UINT32 size)
{
    VOID *nextPool = g_poolHead;
    VOID *curPool = g_poolHead;
    UINTPTR poolEnd;
    while (nextPool != NULL) {
        poolEnd = (UINTPTR)nextPool + LOS_MemPoolSizeGet(nextPool);
        if (((pool <= nextPool) && (((UINTPTR)pool + size) > (UINTPTR)nextPool)) ||
            (((UINTPTR)pool < poolEnd) && (((UINTPTR)pool + size) >= poolEnd))) {
            //新添加的内存池不能和已有内存池冲突
            PRINT_ERR("pool [%p, %p) conflict with pool [%p, %p)\n",
                      pool, (UINTPTR)pool + size,
                      nextPool, (UINTPTR)nextPool + LOS_MemPoolSizeGet(nextPool));
            return LOS_NOK;
        }
        curPool = nextPool;
        nextPool = ((LosMemPoolInfo *)nextPool)->nextPool;
    }

    if (g_poolHead == NULL) {
        g_poolHead = pool;
    } else {
        ((LosMemPoolInfo *)curPool)->nextPool = pool; //新内存池添加到队列尾部
    }

    ((LosMemPoolInfo *)pool)->nextPool = NULL;
    return LOS_OK;
}
#endif

//初始化内存池
STATIC UINT32 OsMemInit(VOID *pool, UINT32 size)
{
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *newNode = NULL;
    LosMemDynNode *endNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;

    poolInfo->pool = pool; //内存池起始地址
    poolInfo->poolSize = size; //内存池静态尺寸
    poolInfo->flag = MEM_POOL_EXPAND_DISABLE; //刚开始内存池不支持链式扩展
    OsDLnkInitMultiHead(OS_MEM_HEAD_ADDR(pool)); //初始化空闲队列数组
    newNode = OS_MEM_FIRST_NODE(pool); //获取第一个内存节点地址，在上述数组之后
    //第一个节点的尺寸为内存池中剩余的空间再排除掉一个空节点
    newNode->selfNode.sizeAndFlag = (size - (UINT32)((UINTPTR)newNode - (UINTPTR)pool) - OS_MEM_NODE_HEAD_SIZE);
	//第一个节点的前一个节点设置成尾节点(即处于内存池尾部但无数据的空节点)
    newNode->selfNode.preNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, size);
	//将第一个节点放入某个空闲队列(根据其尺寸获得合适的队列)
    listNodeHead = OS_MEM_HEAD(pool, newNode->selfNode.sizeAndFlag);
    if (listNodeHead == NULL) {
        return LOS_NOK;
    }

	//放入空闲队列
    LOS_ListTailInsert(listNodeHead, &(newNode->selfNode.freeNodeInfo));
	//内存池中的尾节点处于最末尾
    endNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, size);
	//清空尾节点的内容
    (VOID)memset_s(endNode, sizeof(*endNode), 0, sizeof(*endNode));
	//尾节点的上一个节点初始化为第一个节点
    endNode->selfNode.preNode = newNode;
	//尾节点设置成哨兵节点，且后续无内存块
    OsMemSentinelNodeSet(endNode, NULL, 0);
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//当前内存池中已使用的内存含：内存池控制头部，若干个队列头部，以及尾节点(哨兵节点)。
    poolInfo->poolCurUsedSize = sizeof(LosMemPoolInfo) + OS_MULTI_DLNK_HEAD_SIZE +
                                OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
    poolInfo->poolWaterLine = poolInfo->poolCurUsedSize;
#endif
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(newNode); //备份首节点
    OsMemNodeSave(endNode); //备份尾节点
#endif

    return LOS_OK;
}

//初始化内存池
LITE_OS_SEC_TEXT_INIT UINT32 LOS_MemInit(VOID *pool, UINT32 size)
{
    UINT32 intSave;

    if ((pool == NULL) || (size < OS_MEM_MIN_POOL_SIZE)) {
        return OS_ERROR;
    }

    if (!IS_ALIGNED(size, OS_MEM_ALIGN_SIZE)) { //内存池尺寸需要是4字节的整数倍
        PRINT_WARN("pool [%p, %p) size 0x%x sholud be aligned with OS_MEM_ALIGN_SIZE\n",
                   pool, (UINTPTR)pool + size, size);
        size = OS_MEM_ALIGN(size, OS_MEM_ALIGN_SIZE) - OS_MEM_ALIGN_SIZE;
    }

    MEM_LOCK(intSave);
#ifdef LOSCFG_MEM_MUL_POOL
	//支持多内存池的情况下，将本内存池加入系统中
    if (OsMemPoolAdd(pool, size)) {
        MEM_UNLOCK(intSave);
        return OS_ERROR;
    }
#endif

    if (OsMemInit(pool, size)) { //初始化本内存池
#ifdef LOSCFG_MEM_MUL_POOL
        (VOID)LOS_MemDeInit(pool);
#endif
        MEM_UNLOCK(intSave);
        return OS_ERROR;
    }

    MEM_UNLOCK(intSave);
    return LOS_OK;
}

#ifdef LOSCFG_MEM_MUL_POOL
//将内存池从系统中删除
LITE_OS_SEC_TEXT_INIT UINT32 LOS_MemDeInit(VOID *pool)
{
    UINT32 intSave;
    UINT32 ret = LOS_NOK;
    VOID *nextPool = NULL;
    VOID *curPool = NULL;

    MEM_LOCK(intSave);
    do {
        if (pool == NULL) {
            break;
        }

        if (pool == g_poolHead) {
			//内存池在链表头部，直接移除
            g_poolHead = ((LosMemPoolInfo *)g_poolHead)->nextPool;
            ret = LOS_OK;
            break;
        }

        curPool = g_poolHead;
        nextPool = g_poolHead;
        while (nextPool != NULL) {
            if (pool == nextPool) {
				//从链表中部移除
                ((LosMemPoolInfo *)curPool)->nextPool = ((LosMemPoolInfo *)nextPool)->nextPool;
                ret = LOS_OK;
                break;
            }
            curPool = nextPool;
            nextPool = ((LosMemPoolInfo *)nextPool)->nextPool;
        }
    } while (0);

    MEM_UNLOCK(intSave);
    return ret;
}

//遍历内存池链表，打印输出每个内存池的简要信息
LITE_OS_SEC_TEXT_INIT UINT32 LOS_MemPoolList(VOID)
{
    VOID *nextPool = g_poolHead;
    UINT32 index = 0;
    while (nextPool != NULL) {
        PRINTK("pool%u :\n", index);
        index++;
        OsMemInfoPrint(nextPool);
        nextPool = ((LosMemPoolInfo *)nextPool)->nextPool;
    }
    return index;
}
#endif

//申请内存节点
LITE_OS_SEC_TEXT VOID *LOS_MemAlloc(VOID *pool, UINT32 size)
{
    VOID *ptr = NULL;
    UINT32 intSave;

    if ((pool == NULL) || (size == 0)) {
		//内存池还没有创建时，使用最简单的内存申请方式，不支持释放
        return (size > 0) ? OsVmBootMemAlloc(size) : NULL;
    }

    MEM_LOCK(intSave);
    do {
        if (OS_MEM_NODE_GET_USED_FLAG(size) || OS_MEM_NODE_GET_ALIGNED_FLAG(size)) {
            break; //size参数不合法，则放弃申请
        }

		//然后进行实际的内存申请操作
        ptr = OsMemAllocWithCheck(pool, size, intSave);
    } while (0);

#ifdef LOSCFG_MEM_RECORDINFO
    OsMemRecordMalloc(ptr, size); //记录内存申请过程的调用栈回溯信息
#endif
    MEM_UNLOCK(intSave);

    return ptr;
}


//有对齐要求的内存申请
LITE_OS_SEC_TEXT VOID *LOS_MemAllocAlign(VOID *pool, UINT32 size, UINT32 boundary)
{
    UINT32 useSize;
    UINT32 gapSize;
    VOID *ptr = NULL;
    VOID *alignedPtr = NULL;
    LosMemDynNode *allocNode = NULL;
    UINT32 intSave;

    if ((pool == NULL) || (size == 0) || (boundary == 0) || !IS_POW_TWO(boundary) ||
        !IS_ALIGNED(boundary, sizeof(VOID *))) {
        return NULL;
    }

    MEM_LOCK(intSave);
    /*
     * sizeof(gapSize) bytes stores offset between alignedPtr and ptr,
     * the ptr has been OS_MEM_ALIGN_SIZE(4 or 8) aligned, so maximum
     * offset between alignedPtr and ptr is boundary - OS_MEM_ALIGN_SIZE
     */
    if ((boundary - sizeof(gapSize)) > ((UINT32)(-1) - size)) {
        goto out;
    }

	//为了达到对齐要求，多申请boundary-sizeof(gapSize)字节内存
	//因为内存申请会对齐到sizeof(gapSize)
    useSize = (size + boundary) - sizeof(gapSize); 
    if (OS_MEM_NODE_GET_USED_FLAG(useSize) || OS_MEM_NODE_GET_ALIGNED_FLAG(useSize)) {
        goto out; //useSize太大，不支持
    }

	//做实际的申请动作
    ptr = OsMemAllocWithCheck(pool, useSize, intSave);

	//对指针进行一个调整，使得其按boundary字节对齐
    alignedPtr = (VOID *)OS_MEM_ALIGN(ptr, boundary);
    if (ptr == alignedPtr) {
        goto out; //如果对齐动作没有调整指针，则直接结束
    }

    /* store gapSize in address (ptr -4), it will be checked while free */
	//计算对齐调整导致指针偏移的字节数目
    gapSize = (UINT32)((UINTPTR)alignedPtr - (UINTPTR)ptr);
    allocNode = (LosMemDynNode *)ptr - 1;  //获取内存节点头控制块
    //设置本内存节点含对齐处理空洞标志
    OS_MEM_NODE_SET_ALIGNED_FLAG(allocNode->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_RECORDINFO
    allocNode->selfNode.originSize = size; //记录用户原始需求尺寸
#endif
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSaveWithGapSize(allocNode, gapSize); //备份内存节点头
#endif
    OS_MEM_NODE_SET_ALIGNED_FLAG(gapSize);
	//将指针偏移字节数以及对应的标记记录在gap空间末尾
    *(UINT32 *)((UINTPTR)alignedPtr - sizeof(gapSize)) = gapSize; 
    ptr = alignedPtr; //返回满足对齐要求的内存
out:
#ifdef LOSCFG_MEM_RECORDINFO
    OsMemRecordMalloc(ptr, size);
#endif
    MEM_UNLOCK(intSave);

    return ptr;
}


//释放内存
LITE_OS_SEC_TEXT STATIC INLINE UINT32 OsDoMemFree(VOID *pool, const VOID *ptr, LosMemDynNode *node)
{
	//先检查即将被释放的内存块是否合法
    UINT32 ret = OsMemCheckUsedNode(pool, node);
    if (ret == LOS_OK) {
#ifdef LOSCFG_MEM_RECORDINFO
		//做运维统计
        OsMemRecordFree(ptr, node->selfNode.originSize);
#endif
		//释放内存节点
        OsMemFreeNode(node, pool);
    }
    return ret;
}

#ifdef LOSCFG_MEM_HEAD_BACKUP
//检查节点正确性，如果有问题，尝试修复
LITE_OS_SEC_TEXT STATIC INLINE UINT32 OsMemBackupCheckAndRetore(VOID *pool, VOID *ptr, LosMemDynNode *node)
{
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *startNode = OS_MEM_FIRST_NODE(pool);
    LosMemDynNode *endNode   = OS_MEM_END_NODE(pool, poolInfo->poolSize);

    if (OS_MEM_MIDDLE_ADDR(startNode, node, endNode)) {
        /* GapSize is bad or node is broken, we need to verify & try to restore */
        if (!OsMemChecksumVerify(&(node->selfNode))) {
            node = (LosMemDynNode *)((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE);
            return OsMemBackupTryRestore(pool, &node, ptr);
        }
    }
    return LOS_OK;
}
#endif


//释放内存到内存池中
LITE_OS_SEC_TEXT UINT32 LOS_MemFree(VOID *pool, VOID *ptr)
{
    UINT32 ret = LOS_NOK;
    UINT32 gapSize;
    UINT32 intSave;
    LosMemDynNode *node = NULL;

    if ((pool == NULL) || (ptr == NULL) || !IS_ALIGNED(pool, sizeof(VOID *)) || !IS_ALIGNED(ptr, sizeof(VOID *))) {
        return LOS_NOK;
    }

    MEM_LOCK(intSave);
    do {
		//对于无指针对齐需求的内存申请，LosMemCtlNode的最后一个字段就是sizeAndFlag
		//对于有指针对齐需求的内存申请，额外在gap区末尾使用了4字节来存储上述信息
		//不管上述哪种情况，通过用户ptr向前移动4字节都可以找到sizeAndFlag
        gapSize = *(UINT32 *)((UINTPTR)ptr - sizeof(UINT32));
        if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize) && OS_MEM_NODE_GET_USED_FLAG(gapSize)) {
			//此刻不允许2个标志位都有效
            PRINT_ERR("[%s:%d]gapSize:0x%x error\n", __FUNCTION__, __LINE__, gapSize);
            goto OUT;
        }

		//先假定没有指针对齐的内存申请，那么内存节点控制头部做一个简单的计算
        node = (LosMemDynNode *)((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE);

        if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize)) {
			//如果是对齐申请后的内存，则需要取出对齐造成的指针偏移量
            gapSize = OS_MEM_NODE_GET_ALIGNED_GAPSIZE(gapSize);
            if ((gapSize & (OS_MEM_ALIGN_SIZE - 1)) || (gapSize > ((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE))) {
                PRINT_ERR("illegal gapSize: 0x%x\n", gapSize);
                break;
            }
			//重新计算内存节点控制头部位置
            node = (LosMemDynNode *)((UINTPTR)ptr - gapSize - OS_MEM_NODE_HEAD_SIZE);
        }
#ifndef LOSCFG_MEM_HEAD_BACKUP
        ret = OsDoMemFree(pool, ptr, node);  //直接做内存释放动作(无错误检测版本)
#endif
    } while (0);
#ifdef LOSCFG_MEM_HEAD_BACKUP
    ret = OsMemBackupCheckAndRetore(pool, ptr, node); //先检测和恢复
    if (!ret) {
        ret = OsDoMemFree(pool, ptr, node); //再释放
    }
#endif
OUT:
    if (ret == LOS_NOK) {
        OsMemRecordFree(ptr, 0);
    }
    MEM_UNLOCK(intSave);
    return ret;
}


//获取真实的数据区首地址，含对齐处理之前的数据区
STATIC VOID *OsGetRealPtr(const VOID *pool, VOID *ptr)
{
    VOID *realPtr = ptr;
    UINT32 gapSize = *((UINT32 *)((UINTPTR)ptr - sizeof(UINT32)));

    if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize) && OS_MEM_NODE_GET_USED_FLAG(gapSize)) {
#ifdef LOSCFG_MEM_RECORDINFO
        OsMemRecordFree(ptr, 0);
#endif
        PRINT_ERR("[%s:%d]gapSize:0x%x error\n", __FUNCTION__, __LINE__, gapSize);
        return NULL;
    }
    if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize)) {
        gapSize = OS_MEM_NODE_GET_ALIGNED_GAPSIZE(gapSize);
        if ((gapSize & (OS_MEM_ALIGN_SIZE - 1)) ||
            (gapSize > ((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE - (UINTPTR)pool))) {
            PRINT_ERR("[%s:%d]gapSize:0x%x error\n", __FUNCTION__, __LINE__, gapSize);
#ifdef LOSCFG_MEM_RECORDINFO
            OsMemRecordFree(ptr, 0);
#endif
            return NULL;
        }
        realPtr = (VOID *)((UINTPTR)ptr - (UINTPTR)gapSize);
    }
    return realPtr;
}

#ifdef LOSCFG_MEM_RECORDINFO
//内存重申请之前的运维记录
STATIC INLINE VOID OsMemReallocNodeRecord(LosMemDynNode *node, UINT32 size, const VOID *ptr)
{
    node->selfNode.originSize = size;
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(node);
#endif
    OsMemRecordMalloc(ptr, size);
}
#else
STATIC INLINE VOID OsMemReallocNodeRecord(LosMemDynNode *node, UINT32 size, const VOID *ptr)
{
    return;
}
#endif


//内存重申请
STATIC VOID *OsMemRealloc(VOID *pool, const VOID *ptr, LosMemDynNode *node, UINT32 size, UINT32 intSave)
{
    LosMemDynNode *nextNode = NULL;
	//需要重申请的内存要额外加上控制头部，并且4字节对齐
    UINT32 allocSize = OS_MEM_ALIGN(size + OS_MEM_NODE_HEAD_SIZE, OS_MEM_ALIGN_SIZE);
	//当前正在使用的节点尺寸
    UINT32 nodeSize = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);
    VOID *tmpPtr = NULL;
    const VOID *originPtr = ptr;
#ifdef LOSCFG_MEM_RECORDINFO
    UINT32 originSize = node->selfNode.originSize; //正在使用的节点用户原来使用的尺寸
#else
    UINT32 originSize = 0;
#endif
    if (nodeSize >= allocSize) {
		//如果当前正在使用的节点尺寸已经满足重申请需求
        OsMemRecordFree(originPtr, originSize);  //运维统计
		//那么从当前节点中切割一部分放回空闲队列
        OsMemReAllocSmaller(pool, allocSize, node, nodeSize);
        OsMemReallocNodeRecord(node, size, ptr); //运维统计
        return (VOID *)ptr;
    }

	//需要重申请的节点比当前正在用的节点尺寸大
    nextNode = OS_MEM_NEXT_NODE(node); //考察相邻的下一个节点
    if (!OS_MEM_NODE_GET_USED_FLAG(nextNode->selfNode.sizeAndFlag) &&
        ((nextNode->selfNode.sizeAndFlag + nodeSize) >= allocSize)) {
        //下一个节点空闲，并且和我合并后，能满足需求
        OsMemRecordFree(originPtr, originSize);
		//那么合并后再分配，当然内部会处理切割逻辑
        OsMemMergeNodeForReAllocBigger(pool, allocSize, node, nodeSize, nextNode);
        OsMemReallocNodeRecord(node, size, ptr);
        return (VOID *)ptr;
    }

	//其它情况，则采用普通的申请内存方式了
    tmpPtr = OsMemAllocWithCheck(pool, size, intSave);
    if (tmpPtr != NULL) {
        OsMemRecordMalloc(tmpPtr, size);
		//拷贝数据到重申请后的内存
        if (memcpy_s(tmpPtr, size, ptr, (nodeSize - OS_MEM_NODE_HEAD_SIZE)) != EOK) {
            MEM_UNLOCK(intSave);
            (VOID)LOS_MemFree((VOID *)pool, (VOID *)tmpPtr);
            MEM_LOCK(intSave);
            return NULL;
        }
        OsMemRecordFree(originPtr, originSize);
        OsMemFreeNode(node, pool); //释放原内存
    }
    return tmpPtr;
}


//内存重申请
LITE_OS_SEC_TEXT_MINOR VOID *LOS_MemRealloc(VOID *pool, VOID *ptr, UINT32 size)
{
    UINT32 intSave;
    VOID *newPtr = NULL;
    LosMemDynNode *node = NULL;
#ifdef LOSCFG_MEM_RECORDINFO
    VOID *originPtr = ptr;
#endif

    if (OS_MEM_NODE_GET_USED_FLAG(size) || OS_MEM_NODE_GET_ALIGNED_FLAG(size) || (pool == NULL)) {
        return NULL; //尺寸过大
    }

    if (ptr == NULL) {
        newPtr = LOS_MemAlloc(pool, size); //常规申请
        goto OUT;
    }

    if (size == 0) {
        (VOID)LOS_MemFree(pool, ptr);  //内存释放
        goto OUT;
    }

    MEM_LOCK(intSave);

    ptr = OsGetRealPtr(pool, ptr);  //原内存节点数据区起始地址(对齐处理前)
    if (ptr == NULL) {
        goto OUT_UNLOCK;
    }

	//原内存节点
    node = (LosMemDynNode *)((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE);
    if (OsMemCheckUsedNode(pool, node) != LOS_OK) {
		//检查原内存节点有效性
#ifdef LOSCFG_MEM_RECORDINFO
        OsMemRecordFree(originPtr, 0);
#endif
        goto OUT_UNLOCK;
    }

	//重申请
    newPtr = OsMemRealloc(pool, ptr, node, size, intSave);

OUT_UNLOCK:
    MEM_UNLOCK(intSave);
OUT:
    return newPtr;
}

//内存池中总内存用量
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemTotalUsedGet(VOID *pool)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *endNode = NULL;
    UINT32 memUsed = 0;
    UINT32 size;
    UINT32 intSave;

    if (pool == NULL) {
        return LOS_NOK;
    }

    MEM_LOCK(intSave);

    endNode = OS_MEM_END_NODE(pool, poolInfo->poolSize);
	//遍历内存池中的数据节点和哨兵节点
    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode <= endNode;) {
        if (tmpNode == endNode) {
            memUsed += OS_MEM_NODE_HEAD_SIZE; //哨兵节点只有控制头，无实际数据
            if (OsMemIsLastSentinelNode(endNode) == FALSE) {
				//更新内存块，后面再遍历这里面的数据节点和哨兵节点
                size = OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
                tmpNode = OsMemSentinelNodeGet(endNode);
                endNode = OS_MEM_END_NODE(tmpNode, size);
                continue;
            } else {
                break;
            }
        } else {
            if (OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
				//已使用节点，统计其节点尺寸
                memUsed += OS_MEM_NODE_GET_SIZE(tmpNode->selfNode.sizeAndFlag);
            }
            tmpNode = OS_MEM_NEXT_NODE(tmpNode);
        }
    }

    MEM_UNLOCK(intSave);

    return memUsed;
}


//内存池中当前内存节点数目统计
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemUsedBlksGet(VOID *pool)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    UINT32 blkNums = 0;
    UINT32 intSave;

    if (pool == NULL) {
        return LOS_NOK;
    }

    MEM_LOCK(intSave);

    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
        if (OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
            blkNums++; //统计正在使用的内存节点数目
        }
    }

    MEM_UNLOCK(intSave);

    return blkNums;
}


//获取使用此内存块的任务ID，目前这个代码没有考虑扩展内存块
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemTaskIdGet(VOID *ptr)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)(VOID *)m_aucSysMem1;
    UINT32 intSave;
#ifdef LOSCFG_EXC_INTERACTION
    if (ptr < (VOID *)m_aucSysMem1) {
        poolInfo = (LosMemPoolInfo *)(VOID *)m_aucSysMem0;  //独立的内存池
    }
#endif
    if ((ptr == NULL) ||
        (ptr < (VOID *)OS_MEM_FIRST_NODE(poolInfo)) ||
        (ptr > (VOID *)OS_MEM_END_NODE(poolInfo, poolInfo->poolSize))) {
        PRINT_ERR("input ptr %p is out of system memory range[%p, %p]\n", ptr, OS_MEM_FIRST_NODE(poolInfo),
                  OS_MEM_END_NODE(poolInfo, poolInfo->poolSize));
        return OS_INVALID;
    }

    MEM_LOCK(intSave);

    for (tmpNode = OS_MEM_FIRST_NODE(poolInfo); tmpNode < OS_MEM_END_NODE(poolInfo, poolInfo->poolSize);
         tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
        if ((UINTPTR)ptr < (UINTPTR)tmpNode) {
            if (OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.preNode->selfNode.sizeAndFlag)) {
                MEM_UNLOCK(intSave);
				//找到对应的内存块，获取本内存块所属的任务ID
                return (UINT32)((UINTPTR)(tmpNode->selfNode.preNode->selfNode.freeNodeInfo.pstNext));
            } else {
                MEM_UNLOCK(intSave);
                PRINT_ERR("input ptr %p is belong to a free mem node\n", ptr);
                return OS_INVALID;
            }
        }
    }

    MEM_UNLOCK(intSave);
    return OS_INVALID;
}

//内存池中空闲内存块数目
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemFreeBlksGet(VOID *pool)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    UINT32 blkNums = 0;
    UINT32 intSave;

    if (pool == NULL) {
        return LOS_NOK;
    }

    MEM_LOCK(intSave);

    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
        if (!OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
            blkNums++; //统计空闲内存块数目
        }
    }

    MEM_UNLOCK(intSave);

    return blkNums;
}


//这个函数目前意义不大
LITE_OS_SEC_TEXT_MINOR UINTPTR LOS_MemLastUsedGet(VOID *pool)
{
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *node = NULL;

    if (pool == NULL) {
        return LOS_NOK;
    }

    node = OS_MEM_END_NODE(pool, poolInfo->poolSize)->selfNode.preNode;
    if (OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
        return (UINTPTR)((CHAR *)node + OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag) + sizeof(LosMemDynNode));
    } else {
        return (UINTPTR)((CHAR *)node + sizeof(LosMemDynNode));
    }
}

/*
 * Description : reset "end node"
 * Input       : pool    --- Pointer to memory pool
 *               preAddr --- Pointer to the pre Pointer of end node
 * Output      : endNode --- pointer to "end node"
 * Return      : the number of free node
 */
LITE_OS_SEC_TEXT_MINOR VOID OsMemResetEndNode(VOID *pool, UINTPTR preAddr)
{
    LosMemDynNode *endNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, ((LosMemPoolInfo *)pool)->poolSize);
    endNode->selfNode.sizeAndFlag = OS_MEM_NODE_HEAD_SIZE;
    if (preAddr != 0) {
        endNode->selfNode.preNode = (LosMemDynNode *)(preAddr - sizeof(LosMemDynNode));
    }
    OS_MEM_NODE_SET_USED_FLAG(endNode->selfNode.sizeAndFlag);
    OsMemSetMagicNumAndTaskID(endNode);

#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(endNode);
#endif
}

//获取内存池以及所有扩展内存的尺寸之和
UINT32 LOS_MemPoolSizeGet(const VOID *pool)
{
    UINT32 count = 0;
    UINT32 size;
    LosMemDynNode *node = NULL;
    LosMemDynNode *sentinel = NULL;

    if (pool == NULL) {
        return LOS_NOK;
    }

    count += ((LosMemPoolInfo *)pool)->poolSize; //内存池的尺寸
    sentinel = OS_MEM_END_NODE(pool, count);

	//遍历所有扩展内存块
    while (OsMemIsLastSentinelNode(sentinel) == FALSE) {
        size = OS_MEM_NODE_GET_SIZE(sentinel->selfNode.sizeAndFlag);
        node = OsMemSentinelNodeGet(sentinel);
        sentinel = OS_MEM_END_NODE(node, size);
        count += size;  //统计其尺寸
    }

    return count; //返回尺寸之和
}


//打印输出内存池简要信息
LITE_OS_SEC_TEXT_MINOR VOID OsMemInfoPrint(VOID *pool)
{
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LOS_MEM_POOL_STATUS status = {0};

    if (LOS_MemInfoGet(pool, &status) == LOS_NOK) {
        return;
    }

#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    PRINTK("pool addr          pool size    used size     free size    "
           "max free node size   used node num     free node num      UsageWaterLine\n");
    PRINTK("---------------    --------     -------       --------     "
           "--------------       -------------      ------------      ------------\n");
    PRINTK("%-16p   0x%-8x   0x%-8x    0x%-8x   0x%-16x   0x%-13x    0x%-13x    0x%-13x\n",
           poolInfo->pool, LOS_MemPoolSizeGet(pool), status.uwTotalUsedSize,
           status.uwTotalFreeSize, status.uwMaxFreeNodeSize, status.uwUsedNodeNum,
           status.uwFreeNodeNum, status.uwUsageWaterLine);

#else
    PRINTK("pool addr          pool size    used size     free size    "
           "max free node size   used node num     free node num\n");
    PRINTK("---------------    --------     -------       --------     "
           "--------------       -------------      ------------\n");
    PRINTK("%-16p   0x%-8x   0x%-8x    0x%-8x   0x%-16x   0x%-13x    0x%-13x\n",
           poolInfo->pool, LOS_MemPoolSizeGet(pool), status.uwTotalUsedSize,
           status.uwTotalFreeSize, status.uwMaxFreeNodeSize, status.uwUsedNodeNum,
           status.uwFreeNodeNum);
#endif
}


//读取内存池的一些统计值
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemInfoGet(VOID *pool, LOS_MEM_POOL_STATUS *poolStatus)
{
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *tmpNode = NULL;
    UINT32 totalUsedSize = 0;
    UINT32 totalFreeSize = 0;
    UINT32 maxFreeNodeSize = 0;
    UINT32 usedNodeNum = 0;
    UINT32 freeNodeNum = 0;
    UINT32 intSave;

    if (poolStatus == NULL) {
        PRINT_ERR("can't use NULL addr to save info\n");
        return LOS_NOK;
    }

    if ((poolInfo == NULL) || ((UINTPTR)pool != (UINTPTR)poolInfo->pool)) {
        PRINT_ERR("wrong mem pool addr: %p, line:%d\n", poolInfo, __LINE__);
        return LOS_NOK;
    }

    tmpNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize);
    tmpNode = (LosMemDynNode *)OS_MEM_ALIGN(tmpNode, OS_MEM_ALIGN_SIZE);

    if (!OS_MEM_MAGIC_VALID(tmpNode->selfNode.freeNodeInfo.pstPrev)) {
        PRINT_ERR("wrong mem pool addr: %p\n, line:%d", poolInfo, __LINE__);
        return LOS_NOK;
    }

    MEM_LOCK(intSave);

    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
        if (!OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
            ++freeNodeNum;
            totalFreeSize += OS_MEM_NODE_GET_SIZE(tmpNode->selfNode.sizeAndFlag);
            if (maxFreeNodeSize < OS_MEM_NODE_GET_SIZE(tmpNode->selfNode.sizeAndFlag)) {
                maxFreeNodeSize = OS_MEM_NODE_GET_SIZE(tmpNode->selfNode.sizeAndFlag);
            }
        } else {
            ++usedNodeNum;
            totalUsedSize += OS_MEM_NODE_GET_SIZE(tmpNode->selfNode.sizeAndFlag);
        }
    }

    MEM_UNLOCK(intSave);

    poolStatus->uwTotalUsedSize = totalUsedSize;
    poolStatus->uwTotalFreeSize = totalFreeSize;
    poolStatus->uwMaxFreeNodeSize = maxFreeNodeSize;
    poolStatus->uwUsedNodeNum = usedNodeNum;
    poolStatus->uwFreeNodeNum = freeNodeNum;
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    poolStatus->uwUsageWaterLine = poolInfo->poolWaterLine;
#endif
    return LOS_OK;
}


//将各统计值显示出来
STATIC INLINE VOID OsShowFreeNode(UINT32 index, UINT32 length, const UINT32 *countNum)
{
    UINT32 count = 0;
    PRINTK("\n    block size:  ");
    while (count < length) {
        PRINTK("2^%-5u", (index + OS_MIN_MULTI_DLNK_LOG2 + count));
        count++;
    }
    PRINTK("\n    node number: ");
    count = 0;
    while (count < length) {
        PRINTK("  %-5u", countNum[count + index]);
        count++;
    }
}

//显示内存池中的空闲状况
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemFreeNodeShow(VOID *pool)
{
    LOS_DL_LIST *listNodeHead = NULL;
    LosMultipleDlinkHead *headAddr = (LosMultipleDlinkHead *)((UINTPTR)pool + sizeof(LosMemPoolInfo));
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    UINT32 linkHeadIndex;
    UINT32 countNum[OS_MULTI_DLNK_NUM] = { 0 };
    UINT32 intSave;

    if ((poolInfo == NULL) || ((UINTPTR)pool != (UINTPTR)poolInfo->pool)) {
        PRINT_ERR("wrong mem pool addr: %p, line:%d\n", poolInfo, __LINE__);
        return LOS_NOK;
    }

    PRINTK("\n   ************************ left free node number**********************");
    MEM_LOCK(intSave);

    for (linkHeadIndex = 0; linkHeadIndex <= (OS_MULTI_DLNK_NUM - 1);
         linkHeadIndex++) {
        listNodeHead = headAddr->listHead[linkHeadIndex].pstNext;
        while (listNodeHead != &(headAddr->listHead[linkHeadIndex])) {
            listNodeHead = listNodeHead->pstNext;
            countNum[linkHeadIndex]++;
        }
    }

    linkHeadIndex = 0;
    while (linkHeadIndex < OS_MULTI_DLNK_NUM) {
        if (linkHeadIndex + COLUMN_NUM < OS_MULTI_DLNK_NUM) {
            OsShowFreeNode(linkHeadIndex, COLUMN_NUM, countNum);
            linkHeadIndex += COLUMN_NUM;
        } else {
            OsShowFreeNode(linkHeadIndex, (OS_MULTI_DLNK_NUM - 1 - linkHeadIndex), countNum);
            break;
        }
    }

    MEM_UNLOCK(intSave);
    PRINTK("\n   ********************************************************************\n\n");

    return LOS_OK;
}
#ifdef LOSCFG_MEM_LEAKCHECK
LITE_OS_SEC_TEXT_MINOR VOID OsMemUsedNodeShow(VOID *pool)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *endNode = NULL;
    UINT32 size;
    UINT32 intSave;
    UINT32 count;

    if (pool == NULL) {
        PRINTK("input param is NULL\n");
        return;
    }
    if (LOS_MemIntegrityCheck(pool)) {
        PRINTK("LOS_MemIntegrityCheck error\n");
        return;
    }
    MEM_LOCK(intSave);
#ifdef __LP64__
    PRINTK("\n\rnode                ");
#else
    PRINTK("\n\rnode        ");
#endif
    for (count = 0; count < LOS_RECORD_LR_CNT; count++) {
#ifdef __LP64__
        PRINTK("        LR[%u]       ", count);
#else
        PRINTK("    LR[%u]   ", count);
#endif
    }
    PRINTK("\n");

    endNode = OS_MEM_END_NODE(pool, poolInfo->poolSize);
    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode <= endNode;) {
        if (tmpNode == endNode) {
            if (OsMemIsLastSentinelNode(endNode) == FALSE) {
                size = OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
                tmpNode = OsMemSentinelNodeGet(endNode);
                endNode = OS_MEM_END_NODE(tmpNode, size);
                continue;
            } else {
                break;
            }
        } else {
            if (OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
#ifdef __LP64__
                PRINTK("%018p: ", tmpNode);
#else
                PRINTK("%010p: ", tmpNode);
#endif
                for (count = 0; count < LOS_RECORD_LR_CNT; count++) {
#ifdef __LP64__
                    PRINTK(" %018p ", tmpNode->selfNode.linkReg[count]);
#else
                    PRINTK(" %010p ", tmpNode->selfNode.linkReg[count]);
#endif
                }
                PRINTK("\n");
            }
            tmpNode = OS_MEM_NEXT_NODE(tmpNode);
        }
    }
    MEM_UNLOCK(intSave);
    return;
}
#endif

#ifdef LOSCFG_BASE_MEM_NODE_SIZE_CHECK

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemNodeSizeCheck(VOID *pool, VOID *ptr, UINT32 *totalSize, UINT32 *availSize)
{
    const VOID *head = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    UINT8 *endPool = NULL;

    if (g_memCheckLevel == LOS_MEM_CHECK_LEVEL_DISABLE) {
        return LOS_ERRNO_MEMCHECK_DISABLED;
    }

    if ((pool == NULL) || (ptr == NULL) || (totalSize == NULL) || (availSize == NULL)) {
        return LOS_ERRNO_MEMCHECK_PARA_NULL;
    }

    endPool = (UINT8 *)pool + poolInfo->poolSize;
    if (!(OS_MEM_MIDDLE_ADDR_OPEN_END(pool, ptr, endPool))) {
        return LOS_ERRNO_MEMCHECK_OUTSIDE;
    }

    if (g_memCheckLevel == LOS_MEM_CHECK_LEVEL_HIGH) {
        head = OsMemFindNodeCtrl(pool, ptr);
        if ((head == NULL) ||
            (OS_MEM_NODE_GET_SIZE(((LosMemDynNode *)head)->selfNode.sizeAndFlag) < ((UINTPTR)ptr - (UINTPTR)head))) {
            return LOS_ERRNO_MEMCHECK_NO_HEAD;
        }
        *totalSize = OS_MEM_NODE_GET_SIZE(((LosMemDynNode *)head)->selfNode.sizeAndFlag - sizeof(LosMemDynNode));
        *availSize = OS_MEM_NODE_GET_SIZE(((LosMemDynNode *)head)->selfNode.sizeAndFlag - ((UINTPTR)ptr -
                                          (UINTPTR)head));
        return LOS_OK;
    }
    if (g_memCheckLevel == LOS_MEM_CHECK_LEVEL_LOW) {
        if (ptr != (VOID *)OS_MEM_ALIGN(ptr, OS_MEM_ALIGN_SIZE)) {
            return LOS_ERRNO_MEMCHECK_NO_HEAD;
        }
        head = (const VOID *)((UINTPTR)ptr - sizeof(LosMemDynNode));
        if (OS_MEM_MAGIC_VALID(((LosMemDynNode *)head)->selfNode.freeNodeInfo.pstPrev)) {
            *totalSize = OS_MEM_NODE_GET_SIZE(((LosMemDynNode *)head)->selfNode.sizeAndFlag - sizeof(LosMemDynNode));
            *availSize = OS_MEM_NODE_GET_SIZE(((LosMemDynNode *)head)->selfNode.sizeAndFlag - sizeof(LosMemDynNode));
            return LOS_OK;
        } else {
            return LOS_ERRNO_MEMCHECK_NO_HEAD;
        }
    }

    return LOS_ERRNO_MEMCHECK_WRONG_LEVEL;
}

/*
 * Description : get a pool's memCtrl
 * Input       : ptr -- point to source ptr
 * Return      : search forward for ptr's memCtrl or "NULL"
 * attention : this func couldn't ensure the return memCtrl belongs to ptr it just find forward the most nearly one
 */
LITE_OS_SEC_TEXT_MINOR const VOID *OsMemFindNodeCtrl(const VOID *pool, const VOID *ptr)
{
    const VOID *head = ptr;

    if (ptr == NULL) {
        return NULL;
    }

    head = (const VOID *)OS_MEM_ALIGN(head, OS_MEM_ALIGN_SIZE);
    while (!OS_MEM_MAGIC_VALID(((LosMemDynNode *)head)->selfNode.freeNodeInfo.pstPrev)) {
        head = (const VOID *)((UINT8 *)head - sizeof(CHAR *));
        if (head <= pool) {
            return NULL;
        }
    }
    return head;
}

LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemCheckLevelSet(UINT8 checkLevel)
{
    if (checkLevel == LOS_MEM_CHECK_LEVEL_LOW) {
        PRINTK("%s: LOS_MEM_CHECK_LEVEL_LOW \n", __FUNCTION__);
    } else if (checkLevel == LOS_MEM_CHECK_LEVEL_HIGH) {
        PRINTK("%s: LOS_MEM_CHECK_LEVEL_HIGH \n", __FUNCTION__);
    } else if (checkLevel == LOS_MEM_CHECK_LEVEL_DISABLE) {
        PRINTK("%s: LOS_MEM_CHECK_LEVEL_DISABLE \n", __FUNCTION__);
    } else {
        PRINTK("%s: wrong param, setting failed !! \n", __FUNCTION__);
        return LOS_ERRNO_MEMCHECK_WRONG_LEVEL;
    }
    g_memCheckLevel = checkLevel;
    return LOS_OK;
}

LITE_OS_SEC_TEXT_MINOR UINT8 LOS_MemCheckLevelGet(VOID)
{
    return g_memCheckLevel;
}


UINT32 OsMemSysNodeCheck(VOID *dstAddr, VOID *srcAddr, UINT32 nodeLength, UINT8 pos)
{
    UINT32 ret;
    UINT32 totalSize = 0;
    UINT32 availSize = 0;
    UINT8 *pool = m_aucSysMem1;
#ifdef LOSCFG_EXC_INTERACTION
    if ((UINTPTR)dstAddr < ((UINTPTR)m_aucSysMem0 + OS_EXC_INTERACTMEM_SIZE)) {
        pool = m_aucSysMem0;
    }
#endif
    if (pos == 0) { /* if this func was called by memset */
        ret = LOS_MemNodeSizeCheck(pool, dstAddr, &totalSize, &availSize);
        if ((ret == LOS_OK) && (nodeLength > availSize)) {
            PRINT_ERR("---------------------------------------------\n");
            PRINT_ERR("memset: dst inode availSize is not enough"
                      " availSize = 0x%x, memset length = 0x%x\n", availSize, nodeLength);
            OsBackTrace();
            PRINT_ERR("---------------------------------------------\n");
            return LOS_NOK;
        }
    } else if (pos == 1) { /* if this func was called by memcpy */
        ret = LOS_MemNodeSizeCheck(pool, dstAddr, &totalSize, &availSize);
        if ((ret == LOS_OK) && (nodeLength > availSize)) {
            PRINT_ERR("---------------------------------------------\n");
            PRINT_ERR("memcpy: dst inode availSize is not enough"
                      " availSize = 0x%x, memcpy length = 0x%x\n", availSize, nodeLength);
            OsBackTrace();
            PRINT_ERR("---------------------------------------------\n");
            return LOS_NOK;
        }
#ifdef LOSCFG_EXC_INTERACTION
        if ((UINTPTR)srcAddr < ((UINTPTR)m_aucSysMem0 + OS_EXC_INTERACTMEM_SIZE)) {
            pool = m_aucSysMem0;
        } else {
            pool = m_aucSysMem1;
        }
#endif
        ret = LOS_MemNodeSizeCheck(pool, srcAddr, &totalSize, &availSize);
        if ((ret == LOS_OK) && (nodeLength > availSize)) {
            PRINT_ERR("---------------------------------------------\n");
            PRINT_ERR("memcpy: src inode availSize is not enough"
                      " availSize = 0x%x, memcpy length = 0x%x\n",
                      availSize, nodeLength);
            OsBackTrace();
            PRINT_ERR("---------------------------------------------\n");
            return LOS_NOK;
        }
    }
    return LOS_OK;
}
#endif /* LOSCFG_BASE_MEM_NODE_SIZE_CHECK */

#ifdef LOSCFG_MEM_MUL_MODULE
STATIC INLINE UINT32 OsMemModCheck(UINT32 moduleID)
{
    if (moduleID > MEM_MODULE_MAX) {
        PRINT_ERR("error module ID input!\n");
        return LOS_NOK;
    }
    return LOS_OK;
}

STATIC INLINE VOID *OsMemPtrToNode(VOID *ptr)
{
    UINT32 gapSize;

    if ((UINTPTR)ptr & (OS_MEM_ALIGN_SIZE - 1)) {
        PRINT_ERR("[%s:%d]ptr:%p not align by 4byte\n", __FUNCTION__, __LINE__, ptr);
        return NULL;
    }

    gapSize = *((UINT32 *)((UINTPTR)ptr - sizeof(UINT32)));
    if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize) && OS_MEM_NODE_GET_USED_FLAG(gapSize)) {
        PRINT_ERR("[%s:%d]gapSize:0x%x error\n", __FUNCTION__, __LINE__, gapSize);
        return NULL;
    }
    if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize)) {
        gapSize = OS_MEM_NODE_GET_ALIGNED_GAPSIZE(gapSize);
        if ((gapSize & (OS_MEM_ALIGN_SIZE - 1)) || (gapSize > ((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE))) {
            PRINT_ERR("[%s:%d]gapSize:0x%x error\n", __FUNCTION__, __LINE__, gapSize);
            return NULL;
        }

        ptr = (VOID *)((UINTPTR)ptr - gapSize);
    }

    return (VOID *)((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE);
}

STATIC INLINE UINT32 OsMemNodeSizeGet(VOID *ptr)
{
    LosMemDynNode *node = (LosMemDynNode *)OsMemPtrToNode(ptr);
    if (node == NULL) {
        return 0;
    }

    return OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);
}

VOID *LOS_MemMalloc(VOID *pool, UINT32 size, UINT32 moduleID)
{
    UINT32 intSave;
    VOID *ptr = NULL;
    VOID *node = NULL;
    if (OsMemModCheck(moduleID) == LOS_NOK) {
        return NULL;
    }
    ptr = LOS_MemAlloc(pool, size);
    if (ptr != NULL) {
        MEM_LOCK(intSave);
        g_moduleMemUsedSize[moduleID] += OsMemNodeSizeGet(ptr);
        node = OsMemPtrToNode(ptr);
        if (node != NULL) {
            OS_MEM_MODID_SET(node, moduleID);
        }
        MEM_UNLOCK(intSave);
    }
    return ptr;
}

VOID *LOS_MemMallocAlign(VOID *pool, UINT32 size, UINT32 boundary, UINT32 moduleID)
{
    UINT32 intSave;
    VOID *ptr = NULL;
    VOID *node = NULL;
    if (OsMemModCheck(moduleID) == LOS_NOK) {
        return NULL;
    }
    ptr = LOS_MemAllocAlign(pool, size, boundary);
    if (ptr != NULL) {
        MEM_LOCK(intSave);
        g_moduleMemUsedSize[moduleID] += OsMemNodeSizeGet(ptr);
        node = OsMemPtrToNode(ptr);
        if (node != NULL) {
            OS_MEM_MODID_SET(node, moduleID);
        }
        MEM_UNLOCK(intSave);
    }
    return ptr;
}

UINT32 LOS_MemMfree(VOID *pool, VOID *ptr, UINT32 moduleID)
{
    UINT32 intSave;
    UINT32 ret;
    UINT32 size;
    LosMemDynNode *node = NULL;

    if ((OsMemModCheck(moduleID) == LOS_NOK) || (ptr == NULL) || (pool == NULL)) {
        return LOS_NOK;
    }

    node = (LosMemDynNode *)OsMemPtrToNode(ptr);
    if (node == NULL) {
        return LOS_NOK;
    }

    size = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);

    if (moduleID != OS_MEM_MODID_GET(node)) {
        PRINT_ERR("node[%p] alloced in module %lu, but free in module %u\n node's taskID: 0x%x\n",
                  ptr, OS_MEM_MODID_GET(node), moduleID, OS_MEM_TASKID_GET(node));
        moduleID = OS_MEM_MODID_GET(node);
    }

    ret = LOS_MemFree(pool, ptr);
    if (ret == LOS_OK) {
        MEM_LOCK(intSave);
        g_moduleMemUsedSize[moduleID] -= size;
        MEM_UNLOCK(intSave);
    }
    return ret;
}

VOID *LOS_MemMrealloc(VOID *pool, VOID *ptr, UINT32 size, UINT32 moduleID)
{
    VOID *newPtr = NULL;
    UINT32 oldNodeSize;
    UINT32 intSave;
    LosMemDynNode *node = NULL;
    UINT32 oldModuleID = moduleID;

    if ((OsMemModCheck(moduleID) == LOS_NOK) || (pool == NULL)) {
        return NULL;
    }

    if (ptr == NULL) {
        return LOS_MemMalloc(pool, size, moduleID);
    }

    node = (LosMemDynNode *)OsMemPtrToNode(ptr);
    if (node == NULL) {
        return NULL;
    }

    if (moduleID != OS_MEM_MODID_GET(node)) {
        PRINT_ERR("a node[%p] alloced in module %lu, but realloc in module %u\n node's taskID: %lu\n",
                  ptr, OS_MEM_MODID_GET(node), moduleID, OS_MEM_TASKID_GET(node));
        oldModuleID = OS_MEM_MODID_GET(node);
    }

    if (size == 0) {
        (VOID)LOS_MemMfree(pool, ptr, oldModuleID);
        return NULL;
    }

    oldNodeSize = OsMemNodeSizeGet(ptr);
    newPtr = LOS_MemRealloc(pool, ptr, size);
    if (newPtr != NULL) {
        MEM_LOCK(intSave);
        g_moduleMemUsedSize[moduleID] += OsMemNodeSizeGet(newPtr);
        g_moduleMemUsedSize[oldModuleID] -= oldNodeSize;
        node = (LosMemDynNode *)OsMemPtrToNode(newPtr);
        OS_MEM_MODID_SET(node, moduleID);
        MEM_UNLOCK(intSave);
    }
    return newPtr;
}

UINT32 LOS_MemMusedGet(UINT32 moduleID)
{
    if (OsMemModCheck(moduleID) == LOS_NOK) {
        return OS_NULL_INT;
    }
    return g_moduleMemUsedSize[moduleID];
}
#endif

STATUS_T OsKHeapInit(size_t size)
{
    STATUS_T ret;
    VOID *ptr = NULL;
    /*
     * roundup to MB aligned in order to set kernel attributes. kernel text/code/data attributes
     * should page mapping, remaining region should section mapping. so the boundary should be
     * MB aligned.
     */
     //在代码和全局数据区结束后，即为堆空间，堆的结尾地址选择1M字节对齐，这样方便后面的内存映射到1级页表
    UINTPTR end = ROUNDUP(g_vmBootMemBase + size, MB); //堆结尾地址
    size = end - g_vmBootMemBase;  //内核堆空间总大小

    ptr = OsVmBootMemAlloc(size);  //堆空间内存块
    if (!ptr) {
        PRINT_ERR("vmm_kheap_init boot_alloc_mem failed! %d\n", size);
        return -1;
    }

	//记录内核堆空间起始地址
    m_aucSysMem0 = m_aucSysMem1 = ptr;
    ret = LOS_MemInit(m_aucSysMem0, size); //初始化内核堆空间
    if (ret != LOS_OK) {
        PRINT_ERR("vmm_kheap_init LOS_MemInit failed!\n");
        g_vmBootMemBase -= size;
        return ret;
    }
    LOS_MemExpandEnable(OS_SYS_MEM_ADDR); //TBD
    return LOS_OK;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
