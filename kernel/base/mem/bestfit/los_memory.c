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
//�ڴ�����ݽṹ
typedef struct {
	//�ڴ����ʼ��ַ
    VOID *pool;      /* Starting address of a memory pool */
	//�ڴ�سߴ�
    UINT32 poolSize; /* Memory pool size */
	//�ڴ���Ƿ�֧������
    UINT32 flag;     /* Whether the memory pool supports expansion */
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//�ڴ��ʹ��ˮ�ߣ�����ʷ�����ʹ����
    UINT32 poolWaterLine;   /* Maximum usage size in a memory pool */
	//�ڴ�ص�ǰʹ����
    UINT32 poolCurUsedSize; /* Current usage size in a memory pool */
#endif
#ifdef LOSCFG_MEM_MUL_POOL
    VOID *nextPool; //��һ���ڴ�أ���ϵͳ֧�ֶ��ڴ��ʱ��Ч
#endif
} LosMemPoolInfo;

/* Memory linked list control node structure */
//�ڴ�ڵ����ͷ������Ϊ2���ڴ�ڵ�
/*
 *  1. ���ݽڵ�(������ͷ������)�������ṩ���û�ʹ�ã����ݽڵ���һ���������ڴ���п����и�����
 *  2. �ڱ��ڵ�(ֻ������ͷ)�����ڸ��������ر��ǽ���ɢ���ڴ������������
 */
typedef struct {
	//�������ݽڵ㣬�����ǰ���У����ô��ֶ����ӿ�������
	//              ��������У�pstNext��¼������ģ��ID���߳�ID
	//�����ڱ��ڵ㣬pstNext����������һ���ڴ�� ��pstPrev������ǵ�ǰ�ڵ����ڱ�
    LOS_DL_LIST freeNodeInfo;         /* Free memory node */
	
	//��ǰ�ڵ����һ���ڵ�
	//�������ݽڵ㣬��һ���ڵ�ָ�����ڴ��ַ��С���Ǹ��ھ����ݽڵ㡣
	//   �����ǰ�Ǳ��ڴ��ĵ�һ�����ݽڵ㣬ǰ�治�������ݽڵ��ˣ���ô�ַ�2�����
	//      1. �ڴ���е�1�����ݽڵ㣬preNodeΪNULLֵ
	//      2. �����ڴ���еĵ�1�����ݽڵ㣬preNodeΪ���ڴ���е��ڱ��ڵ��ַ
	//      ��ô�����Ϊ�˸����ڴ���������ڴ�Ļ���
    struct tagLosMemDynNode *preNode; /* Pointer to the previous memory node */

#ifdef LOSCFG_MEM_HEAD_BACKUP //���ڴ�ڵ�ͷ����Ϣ����
	//�����ж���Ҫ����ڴ����룬�������������ڴ�����㲻�˶���Ҫ��
	//����Ҫ�ƶ�ָ��ʹ��������Ҫ����ô�ƶ����ֽ���Ŀ�ͼ�¼��gapSize��
    UINT32 gapSize;  
	//ÿһ���ڵ���Լ�ͷ����У����������Ȼ���������ݵ�ǰһ���ڵ���
    UINTPTR checksum; /* magic = xor checksum */ 
#endif

#ifdef LOSCFG_MEM_RECORDINFO  //��Ҫ������άͳ��
    UINT32 originSize; //��¼�û���������ĳߴ�
#ifdef LOSCFG_AARCH64
	//����64λARM�ܹ������һ���ֶΣ�ʹ�ú�������������8�ֽڱ߽���
    UINT32 reserve1; /* 64-bit alignment */
#endif
#endif

#ifdef LOSCFG_MEM_LEAKCHECK //֧���ڴ�й¶���ʱ
	//�����ڴ�й¶���ʱ����Ҫ�˽�����ڴ�����ô����ġ�
	//������Ǽ�¼�ڴ�����ʱ�ĺ�������ջ��Ӧ��LR�Ĵ�����ֵ
	//��Ϸ��ű��Ϳ��Ի�ö�Ӧ�ĺ���
    UINTPTR linkReg[LOS_RECORD_LR_CNT];
#endif

#ifdef LOSCFG_AARCH64
	//64λARM����8�ֽڶ���
    UINT32 reserve2; /* 64-bit alignment */
#endif
	//��ǰ�ڴ�ڵ�ĳߴ磬���2λ��������ǣ�ʣ��λ������ʾ�ߴ�
    /* Size and flag of the current node (the high two bits represent a flag,and the rest bits specify the size) */
    UINT32 sizeAndFlag;
} LosMemCtlNode;

/* Memory linked list node structure */
typedef struct tagLosMemDynNode {
#ifdef LOSCFG_MEM_HEAD_BACKUP
    LosMemCtlNode backupNode;  //����������һ���ڵ�
#endif
    LosMemCtlNode selfNode;    //��¼��ǰ�ڵ����Ϣ
} LosMemDynNode;

#ifdef LOSCFG_MEM_MUL_POOL
VOID *g_poolHead = NULL;  //���ڴ������µ�����ͷ��������¼�ڴ������
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
//���ڴ�ڵ㲻����ʱ�����ǾͿ�������pstNext�ֶ����洢��Ϣ
//������pstNext��16λ���洢ʹ�ô��ڴ�ڵ������ID/�߳�ID
//������Ҫ���±��ݽڵ�ͷ��Ϣ��ǰһ���ڵ��У���ȻУ���ҲҪˢ��
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

#ifdef LOSCFG_MEM_MUL_MODULE  //��ģ��֧�֣�����Ҫ��¼����ڴ������ĸ�ģ��ʹ�õ�
#define BITS_NUM_OF_TYPE_SHORT    16
#ifdef LOSCFG_MEM_HEAD_BACKUP
//��¼ģ��ID����pstNext��16λ
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

//�ƶ�ָ�룬ʹ�����������Ҫ��
#define OS_MEM_ALIGN(p, alignSize) (((UINTPTR)(p) + (alignSize) - 1) & ~((UINTPTR)((alignSize) - 1)))
//�ڴ�ڵ�ͷ����Ϣ�ߴ�
#define OS_MEM_NODE_HEAD_SIZE      sizeof(LosMemDynNode)
//��С���ڴ�سߴ�
//�����ڴ�ؽṹ�壬2����ʵ�����ݵ��ڴ�ڵ㣬�Լ�һ�������˫������ͷ����
#define OS_MEM_MIN_POOL_SIZE       (OS_DLNK_HEAD_SIZE + (2 * OS_MEM_NODE_HEAD_SIZE) + sizeof(LosMemPoolInfo))
//�Ƿ�2����ָ��
#define IS_POW_TWO(value)          ((((UINTPTR)(value)) & ((UINTPTR)(value) - 1)) == 0)
//�ڴ�ص�ַ��Ҫ������64�ֽڵ�������
#define POOL_ADDR_ALIGNSIZE        64
#ifdef LOSCFG_AARCH64
//64λARM CPU������������Ҫ������8�ֽ�
#define OS_MEM_ALIGN_SIZE 8
#else
//32λARM CPU������������Ҫ������4�ֽ�
#define OS_MEM_ALIGN_SIZE 4
#endif
//���λ��1��ʾ��ǰ�ڴ�ڵ�����ʹ��
#define OS_MEM_NODE_USED_FLAG             0x80000000U
//�θ�λ��1��ʾ��ǰ�ڴ�ڵ�������ڴ���봦��
#define OS_MEM_NODE_ALIGNED_FLAG          0x40000000U
//���2λ��Ϊ1���ʾ����2���߼�ͬʱ����
#define OS_MEM_NODE_ALIGNED_AND_USED_FLAG (OS_MEM_NODE_USED_FLAG | OS_MEM_NODE_ALIGNED_FLAG)

//�ڴ�ڵ��Ƿ����˶��봦��
#define OS_MEM_NODE_GET_ALIGNED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) & OS_MEM_NODE_ALIGNED_FLAG)
//�����ڴ�ڵ�Ķ����־
#define OS_MEM_NODE_SET_ALIGNED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) = ((sizeAndFlag) | OS_MEM_NODE_ALIGNED_FLAG))
//��ȡ�ڴ�ڵ��ʵ�ʳߴ�
#define OS_MEM_NODE_GET_ALIGNED_GAPSIZE(sizeAndFlag) \
    ((sizeAndFlag) & ~OS_MEM_NODE_ALIGNED_FLAG)
//��ȡ�ڴ�ڵ��ʹ�ñ�־
#define OS_MEM_NODE_GET_USED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) & OS_MEM_NODE_USED_FLAG)
//�����ڴ�ڵ��ʹ�ñ�־
#define OS_MEM_NODE_SET_USED_FLAG(sizeAndFlag) \
    ((sizeAndFlag) = ((sizeAndFlag) | OS_MEM_NODE_USED_FLAG))
//��ȡ�ڴ�ڵ�ĳߴ�
#define OS_MEM_NODE_GET_SIZE(sizeAndFlag) \
    ((sizeAndFlag) & ~OS_MEM_NODE_ALIGNED_AND_USED_FLAG)
//���ݽڵ�ߴ���ڴ�ػ�ȡ�ڴ���Ϣ����ͷ
#define OS_MEM_HEAD(pool, size) \
    OsDLnkMultiHead(OS_MEM_HEAD_ADDR(pool), size)
//��ȡ����ͷ�����׵�ַ������ͷ�������ڴ�ؿ��ƿ�֮��
#define OS_MEM_HEAD_ADDR(pool) \
    ((VOID *)((UINTPTR)(pool) + sizeof(LosMemPoolInfo)))
//���ݵ�ǰ�ڵ��ȡ��һ���ڵ�
//��ǰ�ڵ��ַ+��ǰ�ڵ�ߴ�
#define OS_MEM_NEXT_NODE(node) \
    ((LosMemDynNode *)(VOID *)((UINT8 *)(node) + OS_MEM_NODE_GET_SIZE((node)->selfNode.sizeAndFlag)))
//��ȡ��һ���ڵ�ĵ�ַ����һ���ڵ�������ͷ����֮��
#define OS_MEM_FIRST_NODE(pool) \
    ((LosMemDynNode *)(VOID *)((UINT8 *)OS_MEM_HEAD_ADDR(pool) + OS_DLNK_HEAD_SIZE))
//��ȡĩβ�ڵ�ĵ�ַ��ĩβ�ڵ����ڴ�ص�ĩβ����ֻ���ڴ�ڵ�ͷ���������ݣ������ڴ�ع���
#define OS_MEM_END_NODE(pool, size) \
    ((LosMemDynNode *)(VOID *)(((UINT8 *)(pool) + (size)) - OS_MEM_NODE_HEAD_SIZE))
//middleAddr�Ƿ��ڰ뿪����[startAddr, endAddr)��
#define OS_MEM_MIDDLE_ADDR_OPEN_END(startAddr, middleAddr, endAddr) \
    (((UINT8 *)(startAddr) <= (UINT8 *)(middleAddr)) && ((UINT8 *)(middleAddr) < (UINT8 *)(endAddr)))
//middleAddr�Ƿ��ڱ�����[startAddr, endAddr]��
#define OS_MEM_MIDDLE_ADDR(startAddr, middleAddr, endAddr) \
    (((UINT8 *)(startAddr) <= (UINT8 *)(middleAddr)) && ((UINT8 *)(middleAddr) <= (UINT8 *)(endAddr)))
//����ħ���������뵱ǰ�����ĵ�ַ��أ��������������
//��Ϊ����value��λ���޷���ǰȷ�������غ��֪�������Ծ��������
#define OS_MEM_SET_MAGIC(value) \
    (value) = (LOS_DL_LIST *)(((UINTPTR)&(value)) ^ (UINTPTR)(-1))
//�ж�ħ���Ƿ���ȷ�������ж��ڴ��Ƿ񱻸�д
#define OS_MEM_MAGIC_VALID(value) \
    (((UINTPTR)(value) ^ ((UINTPTR)&(value))) == (UINTPTR)(-1))

UINT8 *m_aucSysMem0 = NULL;  //�ں��ڴ��0
UINT8 *m_aucSysMem1 = NULL;  //�ں��ڴ��1

#ifdef LOSCFG_BASE_MEM_NODE_SIZE_CHECK  //���ڴ�ڵ���кϷ��Լ��
//�ֳɼ�����鼶��ȱʡ�رռ��
STATIC UINT8 g_memCheckLevel = LOS_MEM_CHECK_LEVEL_DEFAULT;
#endif

#ifdef LOSCFG_MEM_MUL_MODULE
//�������ģ����ڴ�ռ����ͳ��
UINT32 g_moduleMemUsedSize[MEM_MODULE_MAX + 1] = { 0 };
#endif

VOID OsMemInfoPrint(VOID *pool);
#ifdef LOSCFG_BASE_MEM_NODE_SIZE_CHECK
const VOID *OsMemFindNodeCtrl(const VOID *pool, const VOID *ptr);
#endif
#ifdef LOSCFG_MEM_HEAD_BACKUP
//���ڸ�������У���
#define CHECKSUM_MAGICNUM    0xDEADBEEF 
//������Ҫ���ݵ���Ϣ��У���
#define OS_MEM_NODE_CHECKSUN_CALCULATE(ctlNode)    \
    (((UINTPTR)(ctlNode)->freeNodeInfo.pstPrev) ^  \
    ((UINTPTR)(ctlNode)->freeNodeInfo.pstNext) ^   \
    ((UINTPTR)(ctlNode)->preNode) ^                \
    (ctlNode)->gapSize ^                           \
    (ctlNode)->sizeAndFlag ^                       \
    CHECKSUM_MAGICNUM)

//��ӡ����ڴ�ڵ�ͷ������Ч��Ϣ
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


//��ӡ����ڴ�ڵ�ĸ���ϸ��Ϣ
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


//��ӡ���Ұָ����ص���Ϣ���������������ջ
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


//���㲢�����ڴ�ڵ�ͷ����Ϣ��У���
STATIC INLINE VOID OsMemChecksumSet(LosMemCtlNode *ctlNode)
{
    ctlNode->checksum = OS_MEM_NODE_CHECKSUN_CALCULATE(ctlNode);
}

//����У���
STATIC INLINE BOOL OsMemChecksumVerify(const LosMemCtlNode *ctlNode)
{
    return ctlNode->checksum == OS_MEM_NODE_CHECKSUN_CALCULATE(ctlNode);
}

//���ڴ�ڵ�ͷ����Ϣ���б���
//���䱸�ݵ�ǰһ���ڵ�
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

//��ȡ��ǰ�ڵ����һ���ڵ�
//ע�⣬β���ڵ����һ���ڵ��ǵ�һ���ڵ�
LosMemDynNode *OsMemNodeNextGet(const VOID *pool, const LosMemDynNode *node)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;

    if (node == OS_MEM_END_NODE(pool, poolInfo->poolSize)) {
        return OS_MEM_FIRST_NODE(pool);
    } else {
        return OS_MEM_NEXT_NODE(node);
    }
}


//����һ���ڵ��ͷ����Ϣ���ݵ����ڵ�
STATIC INLINE UINT32 OsMemBackupSetup4Next(const VOID *pool, LosMemDynNode *node)
{
    LosMemDynNode *nodeNext = OsMemNodeNextGet(pool, node);

    if (!OsMemChecksumVerify(&nodeNext->selfNode)) {
		//����ֻ���������Ľڵ㣬����ڵ��ѱ��ٻ�����ô���ٱ���
		//ʹ��У�������֤�����Ƿ��ѻٻ�
        PRINT_ERR("[%s]the next node is broken!!\n", __FUNCTION__);
        OsMemDispCtlNode(&(nodeNext->selfNode));
        PRINT_ERR("Current node details:\n");
        OsMemDispMoreDetails(node);

        return LOS_NOK;
    }

    if (!OsMemChecksumVerify(&node->backupNode)) {
		//ԭ�����ݵ���Ϣ�Ѿ���׼ȷ����ô��Ҫ�ٴα���
		//����һ���ڵ��ͷ����Ϣ���ݵ����ڵ�
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


//������һ���ڵ�洢�ı�����Ϣ�ָ���ǰ�ڵ�ͷ��
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
	//�ڵ�ǰ�ڵ�ָ��Ժ���Ҫ�ٴα���һ����һ���ڵ��ͷ����Ϣ
	//��Ϊ��ǰ�ڵ�֮ǰ�������ƻ����ܴ������Ҳ�ƻ��˴洢�ڵ�ǰ�ڵ��е���һ���ڵ�ı�����Ϣ
    return OsMemBackupSetup4Next(pool, node);
}


//��ȡ�׽ڵ��ǰһ���ڵ�(��β�ڵ�)����˳�������ȷ��
STATIC LosMemDynNode *OsMemFirstNodePrevGet(const LosMemPoolInfo *poolInfo)
{
    LosMemDynNode *nodePre = NULL;

	//��һ���ڴ�ڵ��ǰһ���ڵ����(β�ڵ�)
    nodePre = OS_MEM_END_NODE(poolInfo, poolInfo->poolSize);
    if (!OsMemChecksumVerify(&(nodePre->selfNode))) {
		//��β�ڵ��м��У��ͣ�����β�ڵ��ѱ��ƻ�
        PRINT_ERR("the current node is THE FIRST NODE !\n");
        PRINT_ERR("[%s]: the node information of previous node is bad !!\n", __FUNCTION__);
        OsMemDispCtlNode(&(nodePre->selfNode));
        return nodePre;
    }
    if (!OsMemChecksumVerify(&(nodePre->backupNode))) {
		//β�ڵ��еı�����Ϣ�ѱ��ƻ�
        PRINT_ERR("the current node is THE FIRST NODE !\n");
        PRINT_ERR("[%s]: the backup node information of current node in previous Node is bad !!\n", __FUNCTION__);
        OsMemDispCtlNode(&(nodePre->backupNode));
        return nodePre;
    }

    return NULL;
}


//��ȡ��ǰ�ڵ��ǰһ���ڵ�
LosMemDynNode *OsMemNodePrevGet(VOID *pool, const LosMemDynNode *node)
{
    LosMemDynNode *nodeCur = NULL;
    LosMemDynNode *nodePre = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;

    if (node == OS_MEM_FIRST_NODE(pool)) {
		//�����ǰ�ڵ����׽ڵ㣬��ô��ȡβ�ڵ�
        return OsMemFirstNodePrevGet(poolInfo);
    }

	//���򣬱������нڵ㣬����β�ڵ�
    for (nodeCur = OS_MEM_FIRST_NODE(pool);
         nodeCur < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         nodeCur = OS_MEM_NEXT_NODE(nodeCur)) {
        if (!OsMemChecksumVerify(&(nodeCur->selfNode))) {
			//�����ǰ�ڵ�ͷ���ѱ��ƻ�
            PRINT_ERR("[%s]: the node information of current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->selfNode));

            if (nodePre == NULL) {
                return NULL;
            }

            PRINT_ERR("the detailed information of previous node:\n");
            OsMemDispMoreDetails(nodePre);

            /* due to the every step's checksum verify, nodePre is trustful */
			//�����޸���ǰ�ڵ�
			//��Ϊ�����Ǵ�ǰ�����޸��ģ����Ե�ǰ�ڵ��ǰһ���ڵ����ǿ����ε�
            if (OsMemBackupDoRestore(pool, nodePre, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

        if (!OsMemChecksumVerify(&(nodeCur->backupNode))) {
			//Ȼ���鵱ǰ�ڵ��д洢�ı�����Ϣ�������Ч
            PRINT_ERR("[%s]: the backup node information in current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->backupNode));

            if (nodePre != NULL) {
                PRINT_ERR("the detailed information of previous node:\n");
                OsMemDispMoreDetails(nodePre);
            }

			//���ؽ�������Ϣ
            if (OsMemBackupSetup4Next(pool, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

        if (OS_MEM_NEXT_NODE(nodeCur) == node) {
            return nodeCur; //�ҵ�node��ǰһ���ڵ�
        }

        if (OS_MEM_NEXT_NODE(nodeCur) > node) {
            break; //û���ҵ�node��ǰһ���ڵ�
        }

        nodePre = nodeCur; //����Ѱ��
    }

    return NULL; //û���ҵ�������� ˵��node����������
}


//���Ի�ȡ��ǰ�ڵ����һ���ڵ�
LosMemDynNode *OsMemNodePrevTryGet(VOID *pool, LosMemDynNode **node, const VOID *ptr)
{
    UINTPTR nodeShoudBe = 0;
    LosMemDynNode *nodeCur = NULL;
    LosMemDynNode *nodePre = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;

    if (ptr == OS_MEM_FIRST_NODE(pool)) {
		//�׽ڵ����һ���ڵ���β�ڵ�
        return OsMemFirstNodePrevGet(poolInfo);
    }

	//�������нڵ㣬β�ڵ����
    for (nodeCur = OS_MEM_FIRST_NODE(pool);
         nodeCur < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         nodeCur = OS_MEM_NEXT_NODE(nodeCur)) {
        if (!OsMemChecksumVerify(&(nodeCur->selfNode))) {
			//��ǰ�ڵ�ͷ����
            PRINT_ERR("[%s]: the node information of current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->selfNode));

            if (nodePre == NULL) {
                return NULL;  //����׽ڵ��𻵣����޷��޸�
            }

            PRINT_ERR("the detailed information of previous node:\n");
            OsMemDispMoreDetails(nodePre);

            /* due to the every step's checksum verify, nodePre is trustful */
			//�����޸�
            if (OsMemBackupDoRestore(pool, nodePre, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

        if (!OsMemChecksumVerify(&(nodeCur->backupNode))) {
			//��ǰ�ڵ��еı�����Ϣ��
            PRINT_ERR("[%s]: the backup node information in current node is bad !!\n", __FUNCTION__);
            OsMemDispCtlNode(&(nodeCur->backupNode));

            if (nodePre != NULL) {
                PRINT_ERR("the detailed information of previous node:\n");
                OsMemDispMoreDetails(nodePre);
            }

			//���ؽ�������Ϣ
            if (OsMemBackupSetup4Next(pool, nodeCur) != LOS_OK) {
                return NULL;
            }
        }

		//��ǰ�ڵ��Ƿ�����û�ָ��ptr���ڵĽڵ�
        nodeShoudBe = (UINTPTR)nodeCur + nodeCur->selfNode.gapSize + sizeof(LosMemDynNode);
        if (nodeShoudBe == (UINTPTR)ptr) {
			//����ǣ���ô��¼�µ�ǰ�ڵ㣬��������һ���ڵ�
            *node = nodeCur;
            return nodePre;
        }

        if (OS_MEM_NEXT_NODE(nodeCur) > (LosMemDynNode *)ptr) {
            OsMemDispWildPointerMsg(nodeCur, ptr);
            break; //�޷����ҵ��û�ָ�����ڵĽڵ㣬�û����ܴ�����һ��Ұָ��
        }

        nodePre = nodeCur;
    }

    return NULL;
}


//���ݱ�����Ϣ�ָ��ڵ�
STATIC INLINE UINT32 OsMemBackupTryRestore(VOID *pool, LosMemDynNode **node, const VOID *ptr)
{
    LosMemDynNode *nodeHead = NULL;
	//���Ի�ȡָ���Ӧ�ĵ�ǰ�ڵ����һ���ڵ�
    LosMemDynNode *nodePre = OsMemNodePrevTryGet(pool, &nodeHead, ptr); 
    if (nodePre == NULL) {
        return LOS_NOK; //û����һ���ڵ������£�û���ָ���ǰ�ڵ�
    }

    *node = nodeHead;  //��¼��ǰ�ڵ�
    //������һ���ڵ�ָ���ǰ�ڵ�
    return OsMemBackupDoRestore(pool, nodePre, *node);
}


//�ָ�ĳ�ڵ�ͷ����Ϣ
STATIC INLINE UINT32 OsMemBackupRestore(VOID *pool, LosMemDynNode *node)
{
	//��ȡ��һ���ڵ�
    LosMemDynNode *nodePre = OsMemNodePrevGet(pool, node);
    if (nodePre == NULL) {
        return LOS_NOK;
    }

	//�ָ���ǰ�ڵ�
    return OsMemBackupDoRestore(pool, nodePre, node);
}

//���ö��������µ�ָ��ƫ���ֽ���
STATIC INLINE VOID OsMemSetGapSize(LosMemCtlNode *ctlNode, UINT32 gapSize)
{
    ctlNode->gapSize = gapSize;
}

//��ͨ�ڵ����Ϣ����
STATIC VOID OsMemNodeSave(LosMemDynNode *node)
{
	//�޶�������
    OsMemSetGapSize(&node->selfNode, 0);
	//����У���
    OsMemChecksumSet(&node->selfNode);
	//���ݵ�ǰһ���ڵ�
    OsMemBackupSetup(node);
}


//�ж���������ڴ�ڵ�ı���
STATIC VOID OsMemNodeSaveWithGapSize(LosMemDynNode *node, UINT32 gapSize)
{
	//���ö��������ָ��ƫ���ֽ���
    OsMemSetGapSize(&node->selfNode, gapSize);
    OsMemChecksumSet(&node->selfNode); //����У���
    OsMemBackupSetup(node); //����
}

//�ӿ���������ɾ���ڴ�ڵ�
STATIC VOID OsMemListDelete(LOS_DL_LIST *node, const VOID *firstNode)
{
    LosMemDynNode *dynNode = NULL;

	//��˫������������
    node->pstNext->pstPrev = node->pstPrev;
    node->pstPrev->pstNext = node->pstNext;

    if ((VOID *)(node->pstNext) >= firstNode) {
		//�����һ���ڵ㲻���׽ڵ㣬��ô������һ���ڵ�
        dynNode = LOS_DL_LIST_ENTRY(node->pstNext, LosMemDynNode, selfNode.freeNodeInfo);
        OsMemNodeSave(dynNode);
    }

    if ((VOID *)(node->pstPrev) >= firstNode) {
		//�����һ���ڵ㲻���׽ڵ㣬��ô������һ���ڵ�
        dynNode = LOS_DL_LIST_ENTRY(node->pstPrev, LosMemDynNode, selfNode.freeNodeInfo);
        OsMemNodeSave(dynNode);
    }

	//�˽ڵ㲻�ڿ�����������
    node->pstNext = NULL;
    node->pstPrev = NULL;

    dynNode = LOS_DL_LIST_ENTRY(node, LosMemDynNode, selfNode.freeNodeInfo);
    OsMemNodeSave(dynNode); //���ݵ�ǰ�ڵ�
}


//��ָ���ڵ������һ���ڵ�֮��
STATIC VOID OsMemListAdd(LOS_DL_LIST *listNode, LOS_DL_LIST *node, const VOID *firstNode)
{
    LosMemDynNode *dynNode = NULL;

	//node����listNode֮��
    node->pstNext = listNode->pstNext;
    node->pstPrev = listNode;

    dynNode = LOS_DL_LIST_ENTRY(node, LosMemDynNode, selfNode.freeNodeInfo);
    OsMemNodeSave(dynNode); //���ݵ�ǰ�ڵ�

    listNode->pstNext->pstPrev = node;
    if ((VOID *)(listNode->pstNext) >= firstNode) {
		//���ݺ�һ���ڵ�
        dynNode = LOS_DL_LIST_ENTRY(listNode->pstNext, LosMemDynNode, selfNode.freeNodeInfo);
        OsMemNodeSave(dynNode);
    }

    listNode->pstNext = node;
}


//��ӡ����쳣�ڵ����Ϣ
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

	//���������ڴ�ڵ㣬β�ڵ����
    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode < OS_MEM_END_NODE(pool, poolInfo->poolSize);
         tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
        OsMemDispCtlNode(&tmpNode->selfNode);  //��ӡ��ǰ�ڵ�

        if (OsMemChecksumVerify(&tmpNode->selfNode)) {
            continue; //��ǰ�ڵ�û����
        }

		//��ȡ��һ���ڵ�
        nodePre = OsMemNodePrevGet(pool, tmpNode);
        if (nodePre == NULL) {
            PRINT_ERR("the current node is invalid, but cannot find its previous Node\n");
            continue; //��������һ���ڵ�
        }

        PRINT_ERR("the detailed information of previous node:\n");
        OsMemDispMoreDetails(nodePre); //��ӡ��һ���ڵ����ϸ��Ϣ��������ϵ�ǰ�ڵ������
    }

    MEM_UNLOCK(intSave);
    PRINTK("check finish\n");
}

#else  /* without LOSCFG_MEM_HEAD_BACKUP */

//û�нڵ�ͷ����Ϣ��������µ���ز���

//�ӿ����������Ƴ�
STATIC VOID OsMemListDelete(LOS_DL_LIST *node, const VOID *firstNode)
{
    (VOID)firstNode;
    LOS_ListDelete(node);
}

//��ӵ�����������
STATIC VOID OsMemListAdd(LOS_DL_LIST *listNode, LOS_DL_LIST *node, const VOID *firstNode)
{
    (VOID)firstNode;
    LOS_ListHeadInsert(listNode, node);
}

#endif

#ifdef LOSCFG_EXC_INTERACTION  //֧��ϵͳ�쳣״̬�£�shell��Ȼ������ʹ��
//�����ڴ�ض����������Ƿ��������ϸ��룬��Ϊ����һ���ڴ���𻵸��ʴ���������ڴ��
//��������ĵ���Դ��Խ���
LITE_OS_SEC_TEXT_INIT UINT32 OsMemExcInteractionInit(UINTPTR memStart)
{
    UINT32 ret;
    UINT32 poolSize;
    m_aucSysMem0 = (UINT8 *)((memStart + (POOL_ADDR_ALIGNSIZE - 1)) & ~((UINTPTR)(POOL_ADDR_ALIGNSIZE - 1)));
    poolSize = OS_EXC_INTERACTMEM_SIZE;
    ret = LOS_MemInit(m_aucSysMem0, poolSize); //��ʼ���ڴ��
    PRINT_INFO("LiteOS kernel exc interaction memory address:%p,size:0x%x\n", m_aucSysMem0, poolSize);
    return ret;
}
#endif

//�ڴ�ϵͳ��ʼ��
LITE_OS_SEC_TEXT_INIT UINT32 OsMemSystemInit(UINTPTR memStart)
{
    UINT32 ret;
    UINT32 poolSize;

    m_aucSysMem1 = (UINT8 *)((memStart + (POOL_ADDR_ALIGNSIZE - 1)) & ~((UINTPTR)(POOL_ADDR_ALIGNSIZE - 1)));
    poolSize = OS_SYS_MEM_SIZE;
    ret = LOS_MemInit(m_aucSysMem1, poolSize); //�ں��ڴ�س�ʼ��
    PRINT_INFO("LiteOS system heap memory address:%p,size:0x%x\n", m_aucSysMem1, poolSize);
#ifndef LOSCFG_EXC_INTERACTION
    m_aucSysMem0 = m_aucSysMem1; //�����֧���쳣�����Ļ����ں��о�1���ڴ�أ������Ϊ2���ڴ��
#endif
    return ret;
}

#ifdef LOSCFG_MEM_LEAKCHECK
//�ڴ�й¶��⣬��¼�����ڴ�ʱ�ĺ�������ջ
STATIC INLINE VOID OsMemLinkRegisterRecord(LosMemDynNode *node)
{
    UINT32 count = 0;
    UINT32 index = 0;
    UINTPTR framePtr, tmpFramePtr, linkReg;

    (VOID)memset_s(node->selfNode.linkReg, (LOS_RECORD_LR_CNT * sizeof(UINTPTR)), 0,
        (LOS_RECORD_LR_CNT * sizeof(UINTPTR)));
    framePtr = Get_Fp();  //��ȡ��ǰջ֡��ַ
    while ((framePtr > OS_SYS_FUNC_ADDR_START) && (framePtr < OS_SYS_FUNC_ADDR_END)) {
        tmpFramePtr = framePtr;
#ifdef __LP64__
        framePtr = *(UINTPTR *)framePtr;
        linkReg = *(UINTPTR *)(tmpFramePtr + sizeof(UINTPTR)); //��ȡջ����洢��LR��ֵ
#else
        linkReg = *(UINTPTR *)framePtr;
        framePtr = *(UINTPTR *)(tmpFramePtr - sizeof(UINTPTR)); //��ȡջ����洢��LR��ֵ
#endif
        if (index >= LOS_OMIT_LR_CNT) {  //LOS_MemAlloc�Ⱥ����ļ�¼û�����壬Ӧ�ü�¼������������ĺ���
            node->selfNode.linkReg[count++] = linkReg;  //��LR��¼����
            if (count == LOS_RECORD_LR_CNT) {
                break;  //Ŀǰ��¼3��LR, ����Ӧ3��������һ����˵�㹻ʶ���ڴ�����ô�������
            }
        }
        index++; //�����ջ֡
    }
}
#endif

//��ָ����
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
 //Ѱ�Һ��ʵĿ����ڴ��
STATIC INLINE LosMemDynNode *OsMemFindSuitableFreeBlock(VOID *pool, UINT32 allocSize)
{
    LOS_DL_LIST *listNodeHead = NULL;
    LosMemDynNode *tmpNode = NULL;
	//����Ѱ�Ҵ�������̫�࣬��Ϊ��������𻵣����±���������ѭ��
	//��Ҫ�ܿ���ѭ������
    UINT32 maxCount = (LOS_MemPoolSizeGet(pool) / allocSize) << 1;
    UINT32 count;
#ifdef LOSCFG_MEM_HEAD_BACKUP
    UINT32 ret = LOS_OK;
#endif
	//�Ӻ��ʳߴ�����ʼ�����α��������������ֱ�����������
    for (listNodeHead = OS_MEM_HEAD(pool, allocSize); listNodeHead != NULL;
         listNodeHead = OsDLnkNextMultiHead(OS_MEM_HEAD_ADDR(pool), listNodeHead)) {
        count = 0;
		//���ÿһ���������б���
        LOS_DL_LIST_FOR_EACH_ENTRY(tmpNode, listNodeHead, LosMemDynNode, selfNode.freeNodeInfo) {
            if (count++ >= maxCount) {
				//������ѭ���ˣ���ʱֹ���˳�
                PRINT_ERR("[%s:%d]node: execute too much time\n", __FUNCTION__, __LINE__);
                break;
            }

#ifdef LOSCFG_MEM_HEAD_BACKUP
            if (!OsMemChecksumVerify(&tmpNode->selfNode)) {
				//��ǰ�ڵ���
                PRINT_ERR("[%s]: the node information of current node is bad !!\n", __FUNCTION__);
                OsMemDispCtlNode(&tmpNode->selfNode);
				//�����޸�
                ret = OsMemBackupRestore(pool, tmpNode);
            }
            if (ret != LOS_OK) {
                break; //�޸�ʧ��
            }
#endif

            if (((UINTPTR)tmpNode & (OS_MEM_ALIGN_SIZE - 1)) != 0) {
				//��ǰ�ڵ㲻����CPU�Ķ���Ҫ��
                LOS_Panic("[%s:%d]Mem node data error:OS_MEM_HEAD_ADDR(pool)=%p, listNodeHead:%p,"
                          "allocSize=%u, tmpNode=%p\n",
                          __FUNCTION__, __LINE__, OS_MEM_HEAD_ADDR(pool), listNodeHead, allocSize, tmpNode);
                break;
            }
            if (tmpNode->selfNode.sizeAndFlag >= allocSize) {
                return tmpNode; //�ҵ�������Ҫ����ڴ�ڵ�
            }
        }
    }

    return NULL; //û���ҵ�����Ҫ����ڴ�ڵ�
}

/*
 * Description : clear a mem node, set every member to NULL
 * Input       : node    --- Pointer to the mem node which will be cleared up
 */
 //����ڴ�ڵ�ͷ����Ϣ
STATIC INLINE VOID OsMemClearNode(LosMemDynNode *node)
{
    (VOID)memset_s((VOID *)node, sizeof(LosMemDynNode), 0, sizeof(LosMemDynNode));
}

/*
 * Description : merge this node and pre node, then clear this node info
 * Input       : node    --- Pointer to node which will be merged
 */
 //����ǰ�ڵ�(����)�ϲ���ǰһ���ڵ���
STATIC INLINE VOID OsMemMergeNode(LosMemDynNode *node)
{
    LosMemDynNode *nextNode = NULL;

	//����ǰ�ڵ�ĳߴ���ܵ�ǰһ���ڵ���
    node->selfNode.preNode->selfNode.sizeAndFlag += node->selfNode.sizeAndFlag;
	//��ȡ��һ���ڵ�
    nextNode = (LosMemDynNode *)((UINTPTR)node + node->selfNode.sizeAndFlag);
	//ˢ����һ���ڵ��ǰ���ڵ�
    nextNode->selfNode.preNode = node->selfNode.preNode;
#ifdef LOSCFG_MEM_HEAD_BACKUP
	//������һ���ڵ�
    OsMemNodeSave(node->selfNode.preNode);
	//������һ���ڵ�
    OsMemNodeSave(nextNode);
#endif
	//�����ǰ�ڵ�
    OsMemClearNode(node);
}

//�˽ڵ��Ƿ����ⲿ����Ľڵ�(����ͨ����ǰ�ڴ������)
STATIC INLINE BOOL IsExpandPoolNode(VOID *pool, LosMemDynNode *node)
{
    UINTPTR start = (UINTPTR)pool;
    UINTPTR end = start + ((LosMemPoolInfo *)pool)->poolSize;
	//�ڵ��ַ�ڵ�ǰ�ڴ��֮��
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
 //��һ������ڴ�ڵ�ָ��2���ڵ㣬��������һ���黹��ϵͳ��
 //�黹�����д�����ʵĺϲ��߼�
STATIC INLINE VOID OsMemSplitNode(VOID *pool,
                                  LosMemDynNode *allocNode, UINT32 allocSize)
{
    LosMemDynNode *newFreeNode = NULL;
    LosMemDynNode *nextNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;
	//�׽ڵ�
    const VOID *firstNode = (const VOID *)OS_MEM_FIRST_NODE(pool);

	//������Ҫʹ�õĲ��֣����µľ��ǿ��в��֣�����һ�����нڵ�
    newFreeNode = (LosMemDynNode *)(VOID *)((UINT8 *)allocNode + allocSize);
	//���нڵ��ڵ�ǰ�ڵ�֮��
    newFreeNode->selfNode.preNode = allocNode;
	//���нڵ�ĳߴ����
    newFreeNode->selfNode.sizeAndFlag = allocNode->selfNode.sizeAndFlag - allocSize;
	//��Ҫʹ�õ�����ڵ�ĳߴ�����
    allocNode->selfNode.sizeAndFlag = allocSize;
	//ԭ���ڴ�ڵ����һ���ڵ�
    nextNode = OS_MEM_NEXT_NODE(newFreeNode);
	//��Ϊ���нڵ����һ���ڵ�
    nextNode->selfNode.preNode = newFreeNode;
    if (!OS_MEM_NODE_GET_USED_FLAG(nextNode->selfNode.sizeAndFlag)) {
		//�����һ���ڵ�Ҳ���еĻ�����ô�úͱ����и��ȥ�Ŀ��нڵ���кϲ�
        OsMemListDelete(&nextNode->selfNode.freeNodeInfo, firstNode); //�������п��������Ƴ�
        OsMemMergeNode(nextNode); //Ȼ��ϲ������Ƿָ������Ŀ��нڵ���
#ifdef LOSCFG_MEM_HEAD_BACKUP
    } else {
        OsMemNodeSave(nextNode);  //��һ���ڵ���ʹ�ã�������preNode�ֶη����˱仯��������Ҫ���±���
#endif
    }
	//���µĿ��нڵ������ʵĿ�������--���ݳߴ�
    listNodeHead = OS_MEM_HEAD(pool, newFreeNode->selfNode.sizeAndFlag);
    OS_CHECK_NULL_RETURN(listNodeHead);
    /* add expand node to tail to make sure origin pool used first */
    if (IsExpandPoolNode(pool, newFreeNode)) {
		//����ڵ㲻�Ǳ��ڴ������ģ���ô������ж��е�β��������δ������ʹ�ñ��ڴ���еĽڵ�
        OsMemListAdd(listNodeHead->pstPrev, &newFreeNode->selfNode.freeNodeInfo, firstNode);
    } else {
        OsMemListAdd(listNodeHead, &newFreeNode->selfNode.freeNodeInfo, firstNode);
    }

#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(newFreeNode);  //���¿��нڵ���б���
#endif
}

STATIC INLINE LosMemDynNode *PreSentinelNodeGet(const VOID *pool, const LosMemDynNode *node);
STATIC INLINE BOOL OsMemIsLastSentinelNode(LosMemDynNode *node);

//���ڴ�ڵ���ͷ�
UINT32 OsMemLargeNodeFree(const VOID *ptr)
{
	//���ڴ�ڵ��������ҳΪ��λ������ֱ�Ӱ�ҳ�ͷ�
    LosVmPage *page = OsVmVaddrToPage((VOID *)ptr);
    if ((page == NULL) || (page->nPages == 0)) {
        return LOS_NOK;
    }
    LOS_PhysPagesFreeContiguous((VOID *)ptr, page->nPages); //�ͷ����뵽�������ڴ�ҳ

    return LOS_OK;
}

//�����ͷ���ɢ�ڴ�ڵ�
STATIC INLINE BOOL TryShrinkPool(const VOID *pool, const LosMemDynNode *node)
{
    LosMemDynNode *mySentinel = NULL;
    LosMemDynNode *preSentinel = NULL;
	//���ڴ������ݽڵ��С
    size_t totalSize = (UINTPTR)node->selfNode.preNode - (UINTPTR)node;
	//���ڴ�ڵ�Ĵ�С
    size_t nodeSize = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);

    if (nodeSize != totalSize) {
        return FALSE;  //˵�����ڴ����Ŀǰ��ֻһ���ڴ�ڵ㣬�����ܻ���
    }

	//�����ݽڵ��Ǳ��ڴ�����һ���ڵ�
	//���ڵ��ͷź������ڴ��Ϳ����ͷ���
	//�ҳ��������ڴ����ڱ��ڵ�
    preSentinel = PreSentinelNodeGet(pool, node);
    if (preSentinel == NULL) {
        return FALSE;
    }

	//���ڴ���Ӧ���ڱ��ڵ�
    mySentinel = node->selfNode.preNode;
    if (OsMemIsLastSentinelNode(mySentinel)) {
		//�����ڴ�����������һ���ڴ��
		//��ô��һ���ڴ�鼴��������һ�������������Ϣ����һ���ڱ��ڵ���
		//�ڱ��ڵ��м�¼�ߴ�Ϊ0
        preSentinel->selfNode.sizeAndFlag = OS_MEM_NODE_USED_FLAG;
		//�ڱ��ڵ��м�¼��ַҲΪ0
        preSentinel->selfNode.freeNodeInfo.pstNext = NULL;
    } else {
		//�����м�ĳ�ڴ��(�϶����ǵ�һ���ڴ��(��1���ڴ��Ϊ��ʼ�����ڴ�أ�һֱռ�в����ͷ�))
		//ͨ���޸���һ���ڴ����ڱ��ڵ���Ϣ�������Ƴ�����
        preSentinel->selfNode.sizeAndFlag = mySentinel->selfNode.sizeAndFlag;
        preSentinel->selfNode.freeNodeInfo.pstNext = mySentinel->selfNode.freeNodeInfo.pstNext;
    }
	//�ͷ������ڵ��ڴ��
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
 //�ͷ��ڴ�ڵ�
STATIC INLINE VOID OsMemFreeNode(LosMemDynNode *node, VOID *pool)
{
    LosMemDynNode *preNode = NULL;
    LosMemDynNode *nextNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;
    const VOID *firstNode = (const VOID *)OS_MEM_FIRST_NODE(pool);
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;

#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//���ڴ�ڵ��ͷź��ڴ�����ڴ�ʹ��������
    poolInfo->poolCurUsedSize -= OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);
#endif
    if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
		//��������ռ�õ��ڴ�ͳ��
        OS_MEM_REDUCE_USED(OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag), OS_MEM_TASKID_GET(node));
    }

	//����ڴ�ڵ�Ϊ����
    node->selfNode.sizeAndFlag = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(node);  //�ȱ���һ���ڴ�ڵ�
#endif
#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemLinkRegisterRecord(node); //��¼һ�´��ڴ��ͷŹ��̵ĵ���ջ,��������ڴ�й¶
#endif
    preNode = node->selfNode.preNode; /* merage preNode */
    if ((preNode != NULL) && !OS_MEM_NODE_GET_USED_FLAG(preNode->selfNode.sizeAndFlag)) {
		//�����һ���ڵ�Ҳ���У���ô���Ժϲ�����һ���ڵ�
        OsMemListDelete(&(preNode->selfNode.freeNodeInfo), firstNode);
        OsMemMergeNode(node);
        node  = preNode; //�ϲ���Ľڵ�
    }

    nextNode = OS_MEM_NEXT_NODE(node); /* merage nextNode */
    if ((nextNode != NULL) && !OS_MEM_NODE_GET_USED_FLAG(nextNode->selfNode.sizeAndFlag)) {
		//�����һ���ڵ�Ҳ���У���ô�����ϲ�
        OsMemListDelete(&nextNode->selfNode.freeNodeInfo, firstNode);
        OsMemMergeNode(nextNode);
    }

    if (poolInfo->flag & MEM_POOL_EXPAND_ENABLE) {
        /* if this is a expand head node, and all unused, free it to pmm */
        if ((node->selfNode.preNode > node) && (node != firstNode)) {
			//˵������ڴ�ڵ����ڴ����ĳһ�����ڴ���еĵ�һ���ڴ�ڵ�	
			//���ʱ����Գ����ͷ�������ڴ��
            if (TryShrinkPool(pool, node)) {
                return;
            }
        }
    }

	//���ͷź�Ŀ������ݽڵ������ʿ�������
    listNodeHead = OS_MEM_HEAD(pool, node->selfNode.sizeAndFlag);
    OS_CHECK_NULL_RETURN(listNodeHead);
    /* add expand node to tail to make sure origin pool used first */
    if (IsExpandPoolNode(pool, node)) {
		//�����ڴ������չ�ڴ���е����ݽڵ㲻����ʹ�ã����ǻ�������ʹ���ڴ���ڵĽڵ㡣
		//���������ԼӴ������ϲ��Ļ��ᣬ�Ӷ���һ�������������ڴ�鱻���յĻ���
        OsMemListAdd(listNodeHead->pstPrev, &node->selfNode.freeNodeInfo, firstNode);
    } else {
    	//�ڴ���ڵĽڵ������������ǰ������������ʹ��
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
//���ָ���ڵ��Ƿ��Ǳ����е�����ʹ�õĽڵ㣬��ȷ�ж�
STATIC INLINE UINT32 OsMemCheckUsedNode(const VOID *pool, const LosMemDynNode *node)
{
    LosMemDynNode *tmpNode = OS_MEM_FIRST_NODE(pool); //�ڴ�ص�1���ڵ�(����)
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    LosMemDynNode *endNode = (const LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize); //�ڴ�����һ���ڵ�(�ڱ�)

    do {
		//�������ڴ���е��������ݽڵ�
        for (; tmpNode < endNode; tmpNode = OS_MEM_NEXT_NODE(tmpNode)) {
            if ((tmpNode == node) &&
                OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
                return LOS_OK; //�ҵ���Ӧ�Ľڵ㣬���䲻�ǿ���״̬
            }
        }

		
        if (OsMemIsLastSentinelNode(endNode) == FALSE) {
			//����������һ���ڴ�飬��ô��������һ���ڴ��
            tmpNode = OsMemSentinelNodeGet(endNode);
			//�Լ����е��ڱ��ڵ�
            endNode = OS_MEM_END_NODE(tmpNode, OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag));
        } else {
            break;
        }
    } while (1);

    return LOS_NOK;  //���ڵ㲻���ڶѿռ�ĺϷ����ݽڵ�
}

#elif defined(LOS_DLNK_SIMPLE_CHECK)
//�ж�ĳ�ڵ��Ƿ񱾶��е�����ʹ�õĽڵ㣬ģ���ж�
STATIC INLINE UINT32 OsMemCheckUsedNode(const VOID *pool, const LosMemDynNode *node)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    const LosMemDynNode *startNode = (const LosMemDynNode *)OS_MEM_FIRST_NODE(pool);
    const LosMemDynNode *endNode = (const LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize);
    if (!OS_MEM_MIDDLE_ADDR_OPEN_END(startNode, node, endNode)) {
		//�����ڴ����
        return LOS_NOK;
    }

    if (!OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
		//���нڵ�
        return LOS_NOK;
    }

    if (!OS_MEM_MAGIC_VALID(node->selfNode.freeNodeInfo.pstPrev)) {
		//����ʱ���õ�ħ������д
        return LOS_NOK;
    }

    return LOS_OK;
}

#else
//��ĳ�ڱ��ڵ㿪ʼ�������һ���ڱ��ڵ�
STATIC LosMemDynNode *OsMemLastSentinelNodeGet(LosMemDynNode *sentinelNode)
{
    LosMemDynNode *node = NULL;
    VOID *ptr = sentinelNode->selfNode.freeNodeInfo.pstNext; //��һ���ڴ����ʼ��ַ
    UINT32 size = OS_MEM_NODE_GET_SIZE(sentinelNode->selfNode.sizeAndFlag); //�ߴ�

    while ((ptr != NULL) && (size != 0)) {
		//��һ���ڴ����ڵ�����£�����Ѱ������һ���ڴ��
        node = OS_MEM_END_NODE(ptr, size); //��һ���ڱ��ڵ�
        ptr = node->selfNode.freeNodeInfo.pstNext; //�����ڴ����ʼ��ַ
        size = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag); //�����ڴ��ߴ�
    }

    return node; //���һ���ڱ�����û���ڴ��
}

//�ڱ��ڵ������Լ��
STATIC INLINE BOOL OsMemSentinelNodeCheck(LosMemDynNode *node)
{
    if (!OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
        return FALSE; //�ڱ��ڵ�һ�����ǿ��е�
    }

    if (!OS_MEM_MAGIC_VALID(node->selfNode.freeNodeInfo.pstPrev)) {
        return FALSE; //�ڱ��ڵ�ħ��һ��Ҫ��ȷ
    }

    return TRUE;
}

//��ǰ�ڵ��Ƿ����һ���ڱ��ڵ�
STATIC INLINE BOOL OsMemIsLastSentinelNode(LosMemDynNode *node)
{
    if (OsMemSentinelNodeCheck(node) == FALSE) {
		//�����ڱ��ڵ�
        PRINT_ERR("%s %d, The current sentinel node is invalid\n", __FUNCTION__, __LINE__);
        return TRUE;
    }

    if ((OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag) == 0) ||
        (node->selfNode.freeNodeInfo.pstNext == NULL)) {
        return TRUE; //���һ���ڱ��ڵ��¼�ĳߴ�Ϊ0����¼�ĵ�ַҲΪ0
    } else {
        return FALSE; //�������һ���ڱ�
    }
}


//���ڱ��ڵ��м�¼��һ���ڴ�����ʼ��ַ�ͳߴ�
STATIC INLINE VOID OsMemSentinelNodeSet(LosMemDynNode *sentinelNode, VOID *newNode, UINT32 size)
{
    if (sentinelNode->selfNode.freeNodeInfo.pstNext != NULL) {
		//�µ��ڴ����Ҫ�ŵ�β����������Ҫ�����һ���ڱ�����¼
        sentinelNode = OsMemLastSentinelNodeGet(sentinelNode);
    }

	//�����ļ�ħ��
    OS_MEM_SET_MAGIC(sentinelNode->selfNode.freeNodeInfo.pstPrev);
	//��¼���ڴ��ߴ�
    sentinelNode->selfNode.sizeAndFlag = size;
	//�����ڱ��ڵ���ʹ�ñ��
    OS_MEM_NODE_SET_USED_FLAG(sentinelNode->selfNode.sizeAndFlag);
	//�������ڴ����ʼ��ַ
    sentinelNode->selfNode.freeNodeInfo.pstNext = newNode;
}

//�����ڱ��ڵ��ȡ��һ���ڴ���׵�ַ
STATIC INLINE VOID *OsMemSentinelNodeGet(LosMemDynNode *node)
{
    if (OsMemSentinelNodeCheck(node) == FALSE) {
        return NULL;  //�������ڱ��ڵ���ܶ�ȡ��һ���ڴ��
    }

    return node->selfNode.freeNodeInfo.pstNext;
}

//��ȡ��ǰ�ڵ��ǰһ���ڱ��ڵ�
STATIC INLINE LosMemDynNode *PreSentinelNodeGet(const VOID *pool, const LosMemDynNode *node)
{
    UINT32 nextSize;
    LosMemDynNode *nextNode = NULL;
    LosMemDynNode *sentinelNode = NULL;

	//��ʼ�ڱ��ڵ����ڴ��β�ڵ�
    sentinelNode = OS_MEM_END_NODE(pool, ((LosMemPoolInfo *)pool)->poolSize);
    while (sentinelNode != NULL) {
        if (OsMemIsLastSentinelNode(sentinelNode)) {
			//������������һ���ڱ��ڵ㣬��δ�ҵ�����ֱ�ӷ���
            PRINT_ERR("PreSentinelNodeGet can not find node %p\n", node);
            return NULL;
        }
        nextNode = OsMemSentinelNodeGet(sentinelNode); //��һ���ڴ��
        if (nextNode == node) {
            return sentinelNode; //��һ���ڱ��ڵ����һ���ڵ����Լ�
        }
		//��һ���ڴ��ĳߴ�
        nextSize = OS_MEM_NODE_GET_SIZE(sentinelNode->selfNode.sizeAndFlag);
		//��һ���ڴ���е��ڱ��ڵ�
        sentinelNode = OS_MEM_END_NODE(nextNode, nextSize);
    }

    return NULL;
}

//�жϴ˵�ַ�Ƿ�Ϊ�ѿռ��еĽڵ��ַ
BOOL OsMemIsHeapNode(const VOID *ptr)
{
    LosMemPoolInfo *pool = (LosMemPoolInfo *)m_aucSysMem1;
    LosMemDynNode *firstNode = OS_MEM_FIRST_NODE(pool);
    LosMemDynNode *endNode = OS_MEM_END_NODE(pool, pool->poolSize);
    UINT32 intSave;
    UINT32 size;

    if (OS_MEM_MIDDLE_ADDR(firstNode, ptr, endNode)) {
        return TRUE; //������ڴ���У��϶��Ƕѿռ���
    }

    MEM_LOCK(intSave);
	//Ȼ������ڱ��ڵ�ά�����ڴ������
    while (OsMemIsLastSentinelNode(endNode) == FALSE) {
        size = OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
        firstNode = OsMemSentinelNodeGet(endNode);
        endNode = OS_MEM_END_NODE(firstNode, size);
        if (OS_MEM_MIDDLE_ADDR(firstNode, ptr, endNode)) {
			//�����ַ����Щ�ڴ���У�Ҳ��
            MEM_UNLOCK(intSave);
            return TRUE;
        }
    }
    MEM_UNLOCK(intSave);

    return FALSE;
}


//���ڴ��ַ���кϷ��Լ��
STATIC BOOL OsMemAddrValidCheck(const LosMemPoolInfo *pool, const VOID *addr)
{
    UINT32 size;
    LosMemDynNode *node = NULL;
    LosMemDynNode *sentinel = NULL;

    size = ((LosMemPoolInfo *)pool)->poolSize;
    if (OS_MEM_MIDDLE_ADDR_OPEN_END(pool + 1, addr, (UINTPTR)pool + size)) {
        return TRUE; //��ַ���ڴ���У��Ϸ�
    }

	//�����ڱ��ڵ�ά�����ڴ������
    sentinel = OS_MEM_END_NODE(pool, size);
    while (OsMemIsLastSentinelNode(sentinel) == FALSE) {
        size = OS_MEM_NODE_GET_SIZE(sentinel->selfNode.sizeAndFlag);
        node = OsMemSentinelNodeGet(sentinel);
        sentinel = OS_MEM_END_NODE(node, size);
        if (OS_MEM_MIDDLE_ADDR_OPEN_END(node, addr, (UINTPTR)node + size)) {
			//��ַ���ڴ���У�Ҳ�Ϸ�
            return TRUE;
        }
    }

    return FALSE;
}


//node�Ƿ�Ϸ��Ľڵ�
STATIC INLINE BOOL OsMemIsNodeValid(const LosMemDynNode *node, const LosMemDynNode *startNode,
                                    const LosMemDynNode *endNode,
                                    const LosMemPoolInfo *poolInfo)
{
    if (!OS_MEM_MIDDLE_ADDR(startNode, node, endNode)) {
        return FALSE; //node������start��endNode֮��
    }

    if (OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
		//�ǿ���node
        if (!OS_MEM_MAGIC_VALID(node->selfNode.freeNodeInfo.pstPrev)) {
            return FALSE; //��ô��ħ��һ��Ҫ��ȷ
        }
        return TRUE;
    }

	//���нڵ�
    if (!OsMemAddrValidCheck(poolInfo, node->selfNode.freeNodeInfo.pstPrev)) {
        return FALSE;  //���нڵ��ǰһ���ڵ�Ҳ����Ϸ�
    }

    return TRUE;
}

//���һ���ǿ��еĽڵ�ĺϷ���
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
                break; //������Ľڵ���Ҫ����ʼ�ڵ�ͽ����ڵ�֮��
            }

            if (!OS_MEM_NODE_GET_USED_FLAG(node->selfNode.sizeAndFlag)) {
                break; //������Ľڵ㲻���ǿ��нڵ�
            }

            nextNode = OS_MEM_NEXT_NODE(node);
            if (!OsMemIsNodeValid(nextNode, startNode, endNode, poolInfo)) {
                break; //������һ���ڵ�ҲҪ��һ���Ϸ��ڵ�
            }

            if (nextNode->selfNode.preNode != node) {
                break; //nextNode��ǰһ���ڵ�Ӧ����node�Ŷ�
            }

            if ((node != startNode) &&
                ((!OsMemIsNodeValid(node->selfNode.preNode, startNode, endNode, poolInfo)) ||
                (OS_MEM_NEXT_NODE(node->selfNode.preNode) != node))) {
                //��������׽ڵ㣬��ô������һ���ڵ�Ӧ��Ҳ�ǺϷ���
                //����һ���ڵ��Ӧ����һ���ڵ��Ӧ������
                break;
            }
			//���ڴ�ڵ�ͨ���˸�����
            doneFlag = TRUE;
        } while (0);

        if (!doneFlag) {
			//����������û��ͨ������ô��������������������ڴ����ȥ���ҿ���
            if (OsMemIsLastSentinelNode(endNode) == FALSE) {
				//����һ���ڴ��
				//��һ���ڴ���е���ʼ�ڵ�
                startNode = OsMemSentinelNodeGet(endNode);
				//��һ���ڴ���еĽ����ڵ�(�ڱ��ڵ�)
                endNode = OS_MEM_END_NODE(startNode, OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag));
                continue;  //������һ���ж�
            }
			//�����ڴ�鶼�鿴����û��ͨ����飬��ô˵������ڵ㲻�Ϸ�
            return LOS_NOK;
        }
    } while (!doneFlag);

    return LOS_OK;  //����ĳ�ڴ�飬ʹ�������ڴ�ڵ�Ϸ�
}

#endif

/*
 * Description : set magic & taskid
 * Input       : node -- the node which will be set magic & taskid
 */
STATIC INLINE VOID OsMemSetMagicNumAndTaskID(LosMemDynNode *node)
{
    LosTaskCB *runTask = OsCurrTaskGet();

	//��ǰ�ڵ㲻�ڿ��������ˣ���������pstPrev������ħ��
    OS_MEM_SET_MAGIC(node->selfNode.freeNodeInfo.pstPrev);

    /*
     * If the operation occured before task initialization(runTask was not assigned)
     * or in interrupt, make the value of taskid of node to 0xffffffff
     */
    if ((runTask != NULL) && OS_INT_INACTIVE) {
		//�����������ģ���ô���ýڵ�ʹ���ߵ�����ID������pstNext�ֶ�
        OS_MEM_TASKID_SET(node, runTask->taskID);
    } else {
        /* If the task mode does not initialize, the field is the 0xffffffff */
		//�ж������Ļ�����task����������ó�����ֵ
        node->selfNode.freeNodeInfo.pstNext = (LOS_DL_LIST *)OS_NULL_INT;
    }
}


//����ڴ���е�˫����ĺϷ���
LITE_OS_SEC_TEXT_MINOR STATIC INLINE UINT32 OsMemPoolDlinkcheck(const LosMemPoolInfo *pool, LOS_DL_LIST listHead)
{
    if (!OsMemAddrValidCheck(pool, listHead.pstPrev) ||
        !OsMemAddrValidCheck(pool, listHead.pstNext)) {
        return LOS_NOK; //����ͷ����β���ڵ㶼�������ڴ����
    }

    if (!IS_ALIGNED(listHead.pstPrev, sizeof(VOID *)) ||
        !IS_ALIGNED(listHead.pstNext, sizeof(VOID *))) {
        return LOS_NOK; //���ұ�����뵽ָ�������
    }

    return LOS_OK;
}

/*
 * Description : show mem pool header info
 * Input       : pool --Pointer to memory pool
 */
 //��ӡ����ڴ�ظ���������ͷ��Ϣ
LITE_OS_SEC_TEXT_MINOR VOID OsMemPoolHeadInfoPrint(const VOID *pool)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    UINT32 dlinkNum;
    UINT32 flag = 0;
    const LosMultipleDlinkHead *dlinkHead = NULL;

    if (!IS_ALIGNED(poolInfo, sizeof(VOID *))) {
		//�ڴ�ؿ��ƿ��׵�ַ���밴ָ��Ҫ����Ȼ����
        PRINT_ERR("wrong mem pool addr: %p, func:%s,line:%d\n", poolInfo, __FUNCTION__, __LINE__);
#ifdef LOSCFG_SHELL_EXCINFO
        WriteExcInfoToBuf("wrong mem pool addr: %p, func:%s,line:%d\n", poolInfo, __FUNCTION__, __LINE__);
#endif
        return;
    }

	//�������ж���
    dlinkHead = (const LosMultipleDlinkHead *)(VOID *)(poolInfo + 1);
    for (dlinkNum = 0; dlinkNum < OS_MULTI_DLNK_NUM; dlinkNum++) {
        if (OsMemPoolDlinkcheck(pool, dlinkHead->listHead[dlinkNum])) {
            flag = 1;
			//������ж���ͷ���쳣����ô��Ҫ���������־
            PRINT_ERR("DlinkHead[%u]: pstPrev:%p, pstNext:%p\n",
                      dlinkNum, dlinkHead->listHead[dlinkNum].pstPrev, dlinkHead->listHead[dlinkNum].pstNext);
#ifdef LOSCFG_SHELL_EXCINFO
            WriteExcInfoToBuf("DlinkHead[%u]: pstPrev:%p, pstNext:%p\n",
                              dlinkNum, dlinkHead->listHead[dlinkNum].pstPrev, dlinkHead->listHead[dlinkNum].pstNext);
#endif
        }
    }
    if (flag) {
		//���ڿ��ж����쳣�������
		//��������ڴ�ص�������Ϣ
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


//ħ�����ʧ�ܣ������������־
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

//�ڴ�ڵ������Լ��
STATIC UINT32 DoOsMemIntegrityCheck(LosMemDynNode **tmpNode, const VOID *pool, const LosMemDynNode *endPool)
{
    if (OS_MEM_NODE_GET_USED_FLAG((*tmpNode)->selfNode.sizeAndFlag)) {
		//�ѷ���ڵ�
        if (!OS_MEM_MAGIC_VALID((*tmpNode)->selfNode.freeNodeInfo.pstPrev)) {
			//ħ������
            OsMemMagicCheckPrint(tmpNode);
            return LOS_NOK;
        }
    } else { /* is free node, check free node range */
    	//���нڵ�
        if (OsMemAddrValidCheckPrint(pool, endPool, tmpNode) == LOS_NOK) {
			//���нڵ��next , prev��Ҫ�Ϸ�
            return LOS_NOK;
        }
    }
    return LOS_OK;
}


//�ڵ������Լ��
STATIC UINT32 OsMemIntegrityCheck(const VOID *pool, LosMemDynNode **tmpNode, LosMemDynNode **preNode)
{
    const LosMemPoolInfo *poolInfo = (const LosMemPoolInfo *)pool;
    LosMemDynNode *endPool = (LosMemDynNode *)((UINT8 *)pool + poolInfo->poolSize - OS_MEM_NODE_HEAD_SIZE);

    OsMemPoolHeadInfoPrint(pool); //�ڴ��ͷ������������ͷ����飬�����쳣���ӡ������־
    *preNode = OS_MEM_FIRST_NODE(pool); //��ʼ��Ϊ����ͷ��

    do {
		//�������ڴ���е����ݽڵ�
        for (*tmpNode = *preNode; *tmpNode < endPool; *tmpNode = OS_MEM_NEXT_NODE(*tmpNode)) {
			//������ݽڵ������ԣ�����ӡ������־
            if (DoOsMemIntegrityCheck(tmpNode, pool, endPool) == LOS_NOK) {
				//���ݽڵ�����򷵻�
                return LOS_NOK;
            }
            *preNode = *tmpNode;
        }

		//����������һ���ڴ��
        if (OsMemIsLastSentinelNode(*tmpNode) == FALSE) {
			//���ȡ��һ���ڴ��
            *preNode = OsMemSentinelNodeGet(*tmpNode);
            endPool = OS_MEM_END_NODE(*preNode, OS_MEM_NODE_GET_SIZE((*tmpNode)->selfNode.sizeAndFlag));
        } else {
            break; //�����������е��ڴ��
        }
    } while (1);

    return LOS_OK;
}

#ifdef LOSCFG_MEM_LEAKCHECK
//����ڴ�й¶�������ص���ϸ�����Ϣ�����˹��ж�
STATIC VOID OsMemNodeBacktraceInfo(const LosMemDynNode *tmpNode,
                                   const LosMemDynNode *preNode)
{
    int i;
    PRINTK("\n broken node head LR info: \n");
	//��ٽڵ��Ӧ�ĵ���ջ����
    for (i = 0; i < LOS_RECORD_LR_CNT; i++) {
        PRINTK(" LR[%d]:%p\n", i, tmpNode->selfNode.linkReg[i]);
    }
    PRINTK("\n pre node head LR info: \n");
	//��һ���ڵ�ĵ���ջ����
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


//�쳣�ڵ���Ϣ���
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
 //�ڴ����ȷ�Լ�飬�����е����ݽڵ�
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


//�ڴ����û���ҵ����ʵĽڵ㣬ȥ�ڴ��������ڵ�
//�൱���������ڴ��
STATIC INLINE INT32 OsMemPoolExpand(VOID *pool, UINT32 size, UINT32 intSave)
{
    UINT32 tryCount = MAX_SHRINK_PAGECACHE_TRY;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *newNode = NULL;
    LosMemDynNode *endNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;

	//�������������OS_MEM_NODE_HEAD_SIZE�ֽڣ���������Ĳ��������䵱�ڱ��ڵ�
	//�ڱ��ڵ����Ҫ������Ϊ�˼�¼���ݽڵ�(�û�ʵ��ʹ�õ�)��Ԫ���ݡ���ʵ�����ݽڵ����ʼ��ַ�ͳߴ�
	//������ɢ���ڴ�ڵ����������γɵ�����(ǰ���ϵ)
    size = ROUNDUP(size + OS_MEM_NODE_HEAD_SIZE, PAGE_SIZE);  //��������ڴ��СҪΪ�����ڴ�ҳ
    //ԭ�ڴ�ص�β�ڵ�
    endNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, poolInfo->poolSize);

RETRY:
	//������һ���ڴ�(�����ڴ�ҳ)
    newNode = (LosMemDynNode *)LOS_PhysPagesAllocContiguous(size >> PAGE_SHIFT);
    if (newNode == NULL) {
		//����ʧ�ܣ����ͷ�һЩ����ҳ�����Լ���
        if (tryCount > 0) {
            tryCount--;
            MEM_UNLOCK(intSave);
            OsTryShrinkMemory(size >> PAGE_SHIFT);
            MEM_LOCK(intSave);
            goto RETRY;
        }

		//ȷʵ�޷����뵽�ˣ��ڴ�ľ�״̬����size̫��
        PRINT_ERR("OsMemPoolExpand alloc failed size = %u\n", size);
        return -1;
    }
	//�ɹ����뵽�Ժ�
	//�½ڵ�ռ�������ڴ�ҳ������ͷ����Ϣ���ǿ��õ����ݲ���
    newNode->selfNode.sizeAndFlag = (size - OS_MEM_NODE_HEAD_SIZE);	
	//������ɢ�洢���ڴ�ڵ㣬��ȡ��ǰ�ڵ��ǰһ���ڵ�û��ʲôʵ�ʵ�����
	//����ͨ�������ֶ����óɱ��ڵ����ڱ��ڵ�(�ᵼ��preNode > newNode)
	//��Ŀ����ͨ��node�д洢��preNode�ֶ�������node���������ڴ��У���������ɢ�ڴ���
	//�����ڱ��ڵ�����廹�Ƿǳ���ġ����⣬ͨ���ڱ��ڵ㣬���Խ���ɢ���ڴ�ڵ�����������
    newNode->selfNode.preNode = (LosMemDynNode *)OS_MEM_END_NODE(newNode, size);
	//Ѱ�Һ��ʵ��������
    listNodeHead = OS_MEM_HEAD(pool, newNode->selfNode.sizeAndFlag);
    if (listNodeHead == NULL) {
        return -1;
    }
	//�Ƚ����ڵ���Ϣ�����ڱ�����
    OsMemSentinelNodeSet(endNode, newNode, size);
	//Ȼ�󽫱��ڵ�����������
    LOS_ListTailInsert(listNodeHead, &(newNode->selfNode.freeNodeInfo));

	//���ڵ��ĩβ���������䵱�ڱ�
    endNode = (LosMemDynNode *)OS_MEM_END_NODE(newNode, size);
    (VOID)memset_s(endNode, sizeof(*endNode), 0, sizeof(*endNode));

	//�ڱ��ڵ��ǰһ���ڵ���Ǳ��ڵ�	
    endNode->selfNode.preNode = newNode;
	//���ڱ��ڵ����û�нڵ�
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

//�����ڴ��Ϊ����չ��
VOID LOS_MemExpandEnable(VOID *pool)
{
    if (pool == NULL) {
        return;
    }

    ((LosMemPoolInfo *)pool)->flag = MEM_POOL_EXPAND_ENABLE;
}

#ifdef LOSCFG_BASE_MEM_NODE_INTEGRITY_CHECK
//�����ڴ�ǰ������һ���ڴ��飬���ܽϵͣ��ɿ��Խϸ�
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
 //���ڴ��pool������size�ֽڵ��ڴ�ڵ�
STATIC INLINE VOID *OsMemAllocWithCheck(VOID *pool, UINT32 size, UINT32 intSave)
{
    LosMemDynNode *allocNode = NULL;
    UINT32 allocSize;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    const VOID *firstNode = (const VOID *)((UINT8 *)OS_MEM_HEAD_ADDR(pool) + OS_DLNK_HEAD_SIZE);
    INT32 ret;

	//����������ڴ��⹦�ܣ������ڴ�ص���������ȷ��
    if (OsMemAllocCheck(pool, intSave) == LOS_NOK) {
        return NULL;
    }

	//������ڴ�ڵ���Ҫ�������һ������ͷ������Ҫ������4�ֽڱ߽�(32λCPU��)
    allocSize = OS_MEM_ALIGN(size + OS_MEM_NODE_HEAD_SIZE, OS_MEM_ALIGN_SIZE);
    if (allocSize == 0) {
        return NULL;
    }
retry:

	//���Դӿ��ж�������ȡһ������Ҫ����ڴ�ڵ�
    allocNode = OsMemFindSuitableFreeBlock(pool, allocSize);
    if (allocNode == NULL) {
		//�����ǰû������Ҫ��Ľڵ�		
        if (poolInfo->flag & MEM_POOL_EXPAND_ENABLE) {
			//��ô����ڴ��֧�����ݵĻ��������������ɢ���ڴ��
			//���뵽���ڴ�������ж���
            ret = OsMemPoolExpand(pool, allocSize, intSave);
            if (ret == 0) {
                goto retry; //Ȼ������
            }
        }
		//�޷����뵽��Ҫ���ڴ�飬��ӡ������־
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
		//���ӿ��ж�����ȡ�����ڴ��ϴ󣬳���������������и����Ĳ��ַŻؿ��ж���
        OsMemSplitNode(pool, allocNode, allocSize);
    }
	//�ӿ��ж�����ȡ���պ�����������ڴ�ڵ�
    OsMemListDelete(&allocNode->selfNode.freeNodeInfo, firstNode);
	//����ħ��������ID
    OsMemSetMagicNumAndTaskID(allocNode);
	//�����ڴ�ڵ�Ϊ��ʹ��
    OS_MEM_NODE_SET_USED_FLAG(allocNode->selfNode.sizeAndFlag);
    if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
		//������������ڴ�ռ����άͳ��
        OS_MEM_ADD_USED(OS_MEM_NODE_GET_SIZE(allocNode->selfNode.sizeAndFlag), OS_MEM_TASKID_GET(allocNode));
    }
	//������ڴ�ڵ���Ժ���ά��ص��߼�
    OsMemNodeDebugOperate(pool, allocNode, size);
    return (allocNode + 1); //���ش��ڴ�ڵ����ݲ����׵�ַ
}

/*
 * Description : reAlloc a smaller memory node
 * Input       : pool      --- Pointer to memory pool
 *               allocSize --- the size of new node which will be alloced
 *               node      --- the node which wille be realloced
 *               nodeSize  --- the size of old node
 * Output      : node      --- pointer to the new node after realloc
 */
 //��������һ����С���ڴ��
STATIC INLINE VOID OsMemReAllocSmaller(VOID *pool, UINT32 allocSize, LosMemDynNode *node, UINT32 nodeSize)
{
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
#endif
    if ((allocSize + OS_MEM_NODE_HEAD_SIZE + OS_MEM_ALIGN_SIZE) <= nodeSize) {
		//���ڴ�ڵ��ԭ�ڴ�ڵ�С
        node->selfNode.sizeAndFlag = nodeSize;
		//ֱ����ԭ�ڴ�ڵ����и���ಿ�ַŻؿ��ж���
        OsMemSplitNode(pool, node, allocSize);
		//����Ҫ�������ڵ����ó���ʹ��
        OS_MEM_NODE_SET_USED_FLAG(node->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_HEAD_BACKUP
        OsMemNodeSave(node); //���ݽڵ�ͷ����Ϣ
#endif
        if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
			//����ռ�õ��ڴ�ڵ��С
            OS_MEM_REDUCE_USED(nodeSize - allocSize, OS_MEM_TASKID_GET(node));
        }
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
		//�ڴ��ռ�õ��ڴ����
        poolInfo->poolCurUsedSize -= nodeSize - allocSize;
#endif
    }
#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemLinkRegisterRecord(node); //��¼����ջ������Ϣ
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
 //������һ��������ڴ�ڵ�
STATIC INLINE VOID OsMemMergeNodeForReAllocBigger(VOID *pool, UINT32 allocSize, LosMemDynNode *node,
                                                  UINT32 nodeSize, LosMemDynNode *nextNode)
{
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
#endif
	//�ڴ���е�һ���ڵ�
    const VOID *firstNode = (const VOID *)((UINT8 *)OS_MEM_HEAD_ADDR(pool) + OS_DLNK_HEAD_SIZE);

	//�Ƚ�ԭ�ڵ��óɿ��нڵ�
    node->selfNode.sizeAndFlag = nodeSize;
	//����һ���ڵ�ӿ��ж���ȡ��
    OsMemListDelete(&nextNode->selfNode.freeNodeInfo, firstNode);
	//Ȼ��2���ڵ�ϲ�
    OsMemMergeNode(nextNode);
    if ((allocSize + OS_MEM_NODE_HEAD_SIZE + OS_MEM_ALIGN_SIZE) <= node->selfNode.sizeAndFlag) {
		//�ϲ���Ľڵ�ߴ糬�������󣬰Ѷ���Ĳ��ֲü������Żؿ��ж���
        OsMemSplitNode(pool, node, allocSize);
    }
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//��¼�ڴ��ռ������
    poolInfo->poolCurUsedSize += (node->selfNode.sizeAndFlag - nodeSize);
    if (poolInfo->poolCurUsedSize > poolInfo->poolWaterLine) {
		//ˢ��ˮ�߷�ֵ
        poolInfo->poolWaterLine = poolInfo->poolCurUsedSize;
    }
#endif
    if ((pool == (VOID *)OS_SYS_MEM_ADDR) || (pool == (VOID *)m_aucSysMem0)) {
		//ͳ������ռ��
        OS_MEM_ADD_USED(node->selfNode.sizeAndFlag - nodeSize, OS_MEM_TASKID_GET(node));
    }
	//���ڵ��ǳ���ʹ��
    OS_MEM_NODE_SET_USED_FLAG(node->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(node); //���ݽڵ�ͷ����Ϣ
#endif
#ifdef LOSCFG_MEM_LEAKCHECK
    OsMemLinkRegisterRecord(node); //��¼����ջ����
#endif
}

#ifdef LOSCFG_MEM_MUL_POOL
//֧�ֶ��ڴ��ʱ������µ��ڴ��
STATIC UINT32 OsMemPoolAdd(VOID *pool, UINT32 size)
{
    VOID *nextPool = g_poolHead;
    VOID *curPool = g_poolHead;
    UINTPTR poolEnd;
    while (nextPool != NULL) {
        poolEnd = (UINTPTR)nextPool + LOS_MemPoolSizeGet(nextPool);
        if (((pool <= nextPool) && (((UINTPTR)pool + size) > (UINTPTR)nextPool)) ||
            (((UINTPTR)pool < poolEnd) && (((UINTPTR)pool + size) >= poolEnd))) {
            //����ӵ��ڴ�ز��ܺ������ڴ�س�ͻ
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
        ((LosMemPoolInfo *)curPool)->nextPool = pool; //���ڴ����ӵ�����β��
    }

    ((LosMemPoolInfo *)pool)->nextPool = NULL;
    return LOS_OK;
}
#endif

//��ʼ���ڴ��
STATIC UINT32 OsMemInit(VOID *pool, UINT32 size)
{
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)pool;
    LosMemDynNode *newNode = NULL;
    LosMemDynNode *endNode = NULL;
    LOS_DL_LIST *listNodeHead = NULL;

    poolInfo->pool = pool; //�ڴ����ʼ��ַ
    poolInfo->poolSize = size; //�ڴ�ؾ�̬�ߴ�
    poolInfo->flag = MEM_POOL_EXPAND_DISABLE; //�տ�ʼ�ڴ�ز�֧����ʽ��չ
    OsDLnkInitMultiHead(OS_MEM_HEAD_ADDR(pool)); //��ʼ�����ж�������
    newNode = OS_MEM_FIRST_NODE(pool); //��ȡ��һ���ڴ�ڵ��ַ������������֮��
    //��һ���ڵ�ĳߴ�Ϊ�ڴ����ʣ��Ŀռ����ų���һ���սڵ�
    newNode->selfNode.sizeAndFlag = (size - (UINT32)((UINTPTR)newNode - (UINTPTR)pool) - OS_MEM_NODE_HEAD_SIZE);
	//��һ���ڵ��ǰһ���ڵ����ó�β�ڵ�(�������ڴ��β���������ݵĿսڵ�)
    newNode->selfNode.preNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, size);
	//����һ���ڵ����ĳ�����ж���(������ߴ��ú��ʵĶ���)
    listNodeHead = OS_MEM_HEAD(pool, newNode->selfNode.sizeAndFlag);
    if (listNodeHead == NULL) {
        return LOS_NOK;
    }

	//������ж���
    LOS_ListTailInsert(listNodeHead, &(newNode->selfNode.freeNodeInfo));
	//�ڴ���е�β�ڵ㴦����ĩβ
    endNode = (LosMemDynNode *)OS_MEM_END_NODE(pool, size);
	//���β�ڵ������
    (VOID)memset_s(endNode, sizeof(*endNode), 0, sizeof(*endNode));
	//β�ڵ����һ���ڵ��ʼ��Ϊ��һ���ڵ�
    endNode->selfNode.preNode = newNode;
	//β�ڵ����ó��ڱ��ڵ㣬�Һ������ڴ��
    OsMemSentinelNodeSet(endNode, NULL, 0);
#if defined(OS_MEM_WATERLINE) && (OS_MEM_WATERLINE == YES)
	//��ǰ�ڴ������ʹ�õ��ڴ溬���ڴ�ؿ���ͷ�������ɸ�����ͷ�����Լ�β�ڵ�(�ڱ��ڵ�)��
    poolInfo->poolCurUsedSize = sizeof(LosMemPoolInfo) + OS_MULTI_DLNK_HEAD_SIZE +
                                OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
    poolInfo->poolWaterLine = poolInfo->poolCurUsedSize;
#endif
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSave(newNode); //�����׽ڵ�
    OsMemNodeSave(endNode); //����β�ڵ�
#endif

    return LOS_OK;
}

//��ʼ���ڴ��
LITE_OS_SEC_TEXT_INIT UINT32 LOS_MemInit(VOID *pool, UINT32 size)
{
    UINT32 intSave;

    if ((pool == NULL) || (size < OS_MEM_MIN_POOL_SIZE)) {
        return OS_ERROR;
    }

    if (!IS_ALIGNED(size, OS_MEM_ALIGN_SIZE)) { //�ڴ�سߴ���Ҫ��4�ֽڵ�������
        PRINT_WARN("pool [%p, %p) size 0x%x sholud be aligned with OS_MEM_ALIGN_SIZE\n",
                   pool, (UINTPTR)pool + size, size);
        size = OS_MEM_ALIGN(size, OS_MEM_ALIGN_SIZE) - OS_MEM_ALIGN_SIZE;
    }

    MEM_LOCK(intSave);
#ifdef LOSCFG_MEM_MUL_POOL
	//֧�ֶ��ڴ�ص�����£������ڴ�ؼ���ϵͳ��
    if (OsMemPoolAdd(pool, size)) {
        MEM_UNLOCK(intSave);
        return OS_ERROR;
    }
#endif

    if (OsMemInit(pool, size)) { //��ʼ�����ڴ��
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
//���ڴ�ش�ϵͳ��ɾ��
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
			//�ڴ��������ͷ����ֱ���Ƴ�
            g_poolHead = ((LosMemPoolInfo *)g_poolHead)->nextPool;
            ret = LOS_OK;
            break;
        }

        curPool = g_poolHead;
        nextPool = g_poolHead;
        while (nextPool != NULL) {
            if (pool == nextPool) {
				//�������в��Ƴ�
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

//�����ڴ��������ӡ���ÿ���ڴ�صļ�Ҫ��Ϣ
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

//�����ڴ�ڵ�
LITE_OS_SEC_TEXT VOID *LOS_MemAlloc(VOID *pool, UINT32 size)
{
    VOID *ptr = NULL;
    UINT32 intSave;

    if ((pool == NULL) || (size == 0)) {
		//�ڴ�ػ�û�д���ʱ��ʹ����򵥵��ڴ����뷽ʽ����֧���ͷ�
        return (size > 0) ? OsVmBootMemAlloc(size) : NULL;
    }

    MEM_LOCK(intSave);
    do {
        if (OS_MEM_NODE_GET_USED_FLAG(size) || OS_MEM_NODE_GET_ALIGNED_FLAG(size)) {
            break; //size�������Ϸ������������
        }

		//Ȼ�����ʵ�ʵ��ڴ��������
        ptr = OsMemAllocWithCheck(pool, size, intSave);
    } while (0);

#ifdef LOSCFG_MEM_RECORDINFO
    OsMemRecordMalloc(ptr, size); //��¼�ڴ�������̵ĵ���ջ������Ϣ
#endif
    MEM_UNLOCK(intSave);

    return ptr;
}


//�ж���Ҫ����ڴ�����
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

	//Ϊ�˴ﵽ����Ҫ�󣬶�����boundary-sizeof(gapSize)�ֽ��ڴ�
	//��Ϊ�ڴ��������뵽sizeof(gapSize)
    useSize = (size + boundary) - sizeof(gapSize); 
    if (OS_MEM_NODE_GET_USED_FLAG(useSize) || OS_MEM_NODE_GET_ALIGNED_FLAG(useSize)) {
        goto out; //useSize̫�󣬲�֧��
    }

	//��ʵ�ʵ����붯��
    ptr = OsMemAllocWithCheck(pool, useSize, intSave);

	//��ָ�����һ��������ʹ���䰴boundary�ֽڶ���
    alignedPtr = (VOID *)OS_MEM_ALIGN(ptr, boundary);
    if (ptr == alignedPtr) {
        goto out; //������붯��û�е���ָ�룬��ֱ�ӽ���
    }

    /* store gapSize in address (ptr -4), it will be checked while free */
	//��������������ָ��ƫ�Ƶ��ֽ���Ŀ
    gapSize = (UINT32)((UINTPTR)alignedPtr - (UINTPTR)ptr);
    allocNode = (LosMemDynNode *)ptr - 1;  //��ȡ�ڴ�ڵ�ͷ���ƿ�
    //���ñ��ڴ�ڵ㺬���봦��ն���־
    OS_MEM_NODE_SET_ALIGNED_FLAG(allocNode->selfNode.sizeAndFlag);
#ifdef LOSCFG_MEM_RECORDINFO
    allocNode->selfNode.originSize = size; //��¼�û�ԭʼ����ߴ�
#endif
#ifdef LOSCFG_MEM_HEAD_BACKUP
    OsMemNodeSaveWithGapSize(allocNode, gapSize); //�����ڴ�ڵ�ͷ
#endif
    OS_MEM_NODE_SET_ALIGNED_FLAG(gapSize);
	//��ָ��ƫ���ֽ����Լ���Ӧ�ı�Ǽ�¼��gap�ռ�ĩβ
    *(UINT32 *)((UINTPTR)alignedPtr - sizeof(gapSize)) = gapSize; 
    ptr = alignedPtr; //�����������Ҫ����ڴ�
out:
#ifdef LOSCFG_MEM_RECORDINFO
    OsMemRecordMalloc(ptr, size);
#endif
    MEM_UNLOCK(intSave);

    return ptr;
}


//�ͷ��ڴ�
LITE_OS_SEC_TEXT STATIC INLINE UINT32 OsDoMemFree(VOID *pool, const VOID *ptr, LosMemDynNode *node)
{
	//�ȼ�鼴�����ͷŵ��ڴ���Ƿ�Ϸ�
    UINT32 ret = OsMemCheckUsedNode(pool, node);
    if (ret == LOS_OK) {
#ifdef LOSCFG_MEM_RECORDINFO
		//����άͳ��
        OsMemRecordFree(ptr, node->selfNode.originSize);
#endif
		//�ͷ��ڴ�ڵ�
        OsMemFreeNode(node, pool);
    }
    return ret;
}

#ifdef LOSCFG_MEM_HEAD_BACKUP
//���ڵ���ȷ�ԣ���������⣬�����޸�
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


//�ͷ��ڴ浽�ڴ����
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
		//������ָ�����������ڴ����룬LosMemCtlNode�����һ���ֶξ���sizeAndFlag
		//������ָ�����������ڴ����룬������gap��ĩβʹ����4�ֽ����洢������Ϣ
		//�����������������ͨ���û�ptr��ǰ�ƶ�4�ֽڶ������ҵ�sizeAndFlag
        gapSize = *(UINT32 *)((UINTPTR)ptr - sizeof(UINT32));
        if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize) && OS_MEM_NODE_GET_USED_FLAG(gapSize)) {
			//�˿̲�����2����־λ����Ч
            PRINT_ERR("[%s:%d]gapSize:0x%x error\n", __FUNCTION__, __LINE__, gapSize);
            goto OUT;
        }

		//�ȼٶ�û��ָ�������ڴ����룬��ô�ڴ�ڵ����ͷ����һ���򵥵ļ���
        node = (LosMemDynNode *)((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE);

        if (OS_MEM_NODE_GET_ALIGNED_FLAG(gapSize)) {
			//����Ƕ����������ڴ棬����Ҫȡ��������ɵ�ָ��ƫ����
            gapSize = OS_MEM_NODE_GET_ALIGNED_GAPSIZE(gapSize);
            if ((gapSize & (OS_MEM_ALIGN_SIZE - 1)) || (gapSize > ((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE))) {
                PRINT_ERR("illegal gapSize: 0x%x\n", gapSize);
                break;
            }
			//���¼����ڴ�ڵ����ͷ��λ��
            node = (LosMemDynNode *)((UINTPTR)ptr - gapSize - OS_MEM_NODE_HEAD_SIZE);
        }
#ifndef LOSCFG_MEM_HEAD_BACKUP
        ret = OsDoMemFree(pool, ptr, node);  //ֱ�����ڴ��ͷŶ���(�޴�����汾)
#endif
    } while (0);
#ifdef LOSCFG_MEM_HEAD_BACKUP
    ret = OsMemBackupCheckAndRetore(pool, ptr, node); //�ȼ��ͻָ�
    if (!ret) {
        ret = OsDoMemFree(pool, ptr, node); //���ͷ�
    }
#endif
OUT:
    if (ret == LOS_NOK) {
        OsMemRecordFree(ptr, 0);
    }
    MEM_UNLOCK(intSave);
    return ret;
}


//��ȡ��ʵ���������׵�ַ�������봦��֮ǰ��������
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
//�ڴ�������֮ǰ����ά��¼
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


//�ڴ�������
STATIC VOID *OsMemRealloc(VOID *pool, const VOID *ptr, LosMemDynNode *node, UINT32 size, UINT32 intSave)
{
    LosMemDynNode *nextNode = NULL;
	//��Ҫ��������ڴ�Ҫ������Ͽ���ͷ��������4�ֽڶ���
    UINT32 allocSize = OS_MEM_ALIGN(size + OS_MEM_NODE_HEAD_SIZE, OS_MEM_ALIGN_SIZE);
	//��ǰ����ʹ�õĽڵ�ߴ�
    UINT32 nodeSize = OS_MEM_NODE_GET_SIZE(node->selfNode.sizeAndFlag);
    VOID *tmpPtr = NULL;
    const VOID *originPtr = ptr;
#ifdef LOSCFG_MEM_RECORDINFO
    UINT32 originSize = node->selfNode.originSize; //����ʹ�õĽڵ��û�ԭ��ʹ�õĳߴ�
#else
    UINT32 originSize = 0;
#endif
    if (nodeSize >= allocSize) {
		//�����ǰ����ʹ�õĽڵ�ߴ��Ѿ���������������
        OsMemRecordFree(originPtr, originSize);  //��άͳ��
		//��ô�ӵ�ǰ�ڵ����и�һ���ַŻؿ��ж���
        OsMemReAllocSmaller(pool, allocSize, node, nodeSize);
        OsMemReallocNodeRecord(node, size, ptr); //��άͳ��
        return (VOID *)ptr;
    }

	//��Ҫ������Ľڵ�ȵ�ǰ�����õĽڵ�ߴ��
    nextNode = OS_MEM_NEXT_NODE(node); //�������ڵ���һ���ڵ�
    if (!OS_MEM_NODE_GET_USED_FLAG(nextNode->selfNode.sizeAndFlag) &&
        ((nextNode->selfNode.sizeAndFlag + nodeSize) >= allocSize)) {
        //��һ���ڵ���У����Һ��Һϲ�������������
        OsMemRecordFree(originPtr, originSize);
		//��ô�ϲ����ٷ��䣬��Ȼ�ڲ��ᴦ���и��߼�
        OsMemMergeNodeForReAllocBigger(pool, allocSize, node, nodeSize, nextNode);
        OsMemReallocNodeRecord(node, size, ptr);
        return (VOID *)ptr;
    }

	//����������������ͨ�������ڴ淽ʽ��
    tmpPtr = OsMemAllocWithCheck(pool, size, intSave);
    if (tmpPtr != NULL) {
        OsMemRecordMalloc(tmpPtr, size);
		//�������ݵ����������ڴ�
        if (memcpy_s(tmpPtr, size, ptr, (nodeSize - OS_MEM_NODE_HEAD_SIZE)) != EOK) {
            MEM_UNLOCK(intSave);
            (VOID)LOS_MemFree((VOID *)pool, (VOID *)tmpPtr);
            MEM_LOCK(intSave);
            return NULL;
        }
        OsMemRecordFree(originPtr, originSize);
        OsMemFreeNode(node, pool); //�ͷ�ԭ�ڴ�
    }
    return tmpPtr;
}


//�ڴ�������
LITE_OS_SEC_TEXT_MINOR VOID *LOS_MemRealloc(VOID *pool, VOID *ptr, UINT32 size)
{
    UINT32 intSave;
    VOID *newPtr = NULL;
    LosMemDynNode *node = NULL;
#ifdef LOSCFG_MEM_RECORDINFO
    VOID *originPtr = ptr;
#endif

    if (OS_MEM_NODE_GET_USED_FLAG(size) || OS_MEM_NODE_GET_ALIGNED_FLAG(size) || (pool == NULL)) {
        return NULL; //�ߴ����
    }

    if (ptr == NULL) {
        newPtr = LOS_MemAlloc(pool, size); //��������
        goto OUT;
    }

    if (size == 0) {
        (VOID)LOS_MemFree(pool, ptr);  //�ڴ��ͷ�
        goto OUT;
    }

    MEM_LOCK(intSave);

    ptr = OsGetRealPtr(pool, ptr);  //ԭ�ڴ�ڵ���������ʼ��ַ(���봦��ǰ)
    if (ptr == NULL) {
        goto OUT_UNLOCK;
    }

	//ԭ�ڴ�ڵ�
    node = (LosMemDynNode *)((UINTPTR)ptr - OS_MEM_NODE_HEAD_SIZE);
    if (OsMemCheckUsedNode(pool, node) != LOS_OK) {
		//���ԭ�ڴ�ڵ���Ч��
#ifdef LOSCFG_MEM_RECORDINFO
        OsMemRecordFree(originPtr, 0);
#endif
        goto OUT_UNLOCK;
    }

	//������
    newPtr = OsMemRealloc(pool, ptr, node, size, intSave);

OUT_UNLOCK:
    MEM_UNLOCK(intSave);
OUT:
    return newPtr;
}

//�ڴ�������ڴ�����
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
	//�����ڴ���е����ݽڵ���ڱ��ڵ�
    for (tmpNode = OS_MEM_FIRST_NODE(pool); tmpNode <= endNode;) {
        if (tmpNode == endNode) {
            memUsed += OS_MEM_NODE_HEAD_SIZE; //�ڱ��ڵ�ֻ�п���ͷ����ʵ������
            if (OsMemIsLastSentinelNode(endNode) == FALSE) {
				//�����ڴ�飬�����ٱ�������������ݽڵ���ڱ��ڵ�
                size = OS_MEM_NODE_GET_SIZE(endNode->selfNode.sizeAndFlag);
                tmpNode = OsMemSentinelNodeGet(endNode);
                endNode = OS_MEM_END_NODE(tmpNode, size);
                continue;
            } else {
                break;
            }
        } else {
            if (OS_MEM_NODE_GET_USED_FLAG(tmpNode->selfNode.sizeAndFlag)) {
				//��ʹ�ýڵ㣬ͳ����ڵ�ߴ�
                memUsed += OS_MEM_NODE_GET_SIZE(tmpNode->selfNode.sizeAndFlag);
            }
            tmpNode = OS_MEM_NEXT_NODE(tmpNode);
        }
    }

    MEM_UNLOCK(intSave);

    return memUsed;
}


//�ڴ���е�ǰ�ڴ�ڵ���Ŀͳ��
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
            blkNums++; //ͳ������ʹ�õ��ڴ�ڵ���Ŀ
        }
    }

    MEM_UNLOCK(intSave);

    return blkNums;
}


//��ȡʹ�ô��ڴ�������ID��Ŀǰ�������û�п�����չ�ڴ��
LITE_OS_SEC_TEXT_MINOR UINT32 LOS_MemTaskIdGet(VOID *ptr)
{
    LosMemDynNode *tmpNode = NULL;
    LosMemPoolInfo *poolInfo = (LosMemPoolInfo *)(VOID *)m_aucSysMem1;
    UINT32 intSave;
#ifdef LOSCFG_EXC_INTERACTION
    if (ptr < (VOID *)m_aucSysMem1) {
        poolInfo = (LosMemPoolInfo *)(VOID *)m_aucSysMem0;  //�������ڴ��
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
				//�ҵ���Ӧ���ڴ�飬��ȡ���ڴ������������ID
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

//�ڴ���п����ڴ����Ŀ
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
            blkNums++; //ͳ�ƿ����ڴ����Ŀ
        }
    }

    MEM_UNLOCK(intSave);

    return blkNums;
}


//�������Ŀǰ���岻��
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

//��ȡ�ڴ���Լ�������չ�ڴ�ĳߴ�֮��
UINT32 LOS_MemPoolSizeGet(const VOID *pool)
{
    UINT32 count = 0;
    UINT32 size;
    LosMemDynNode *node = NULL;
    LosMemDynNode *sentinel = NULL;

    if (pool == NULL) {
        return LOS_NOK;
    }

    count += ((LosMemPoolInfo *)pool)->poolSize; //�ڴ�صĳߴ�
    sentinel = OS_MEM_END_NODE(pool, count);

	//����������չ�ڴ��
    while (OsMemIsLastSentinelNode(sentinel) == FALSE) {
        size = OS_MEM_NODE_GET_SIZE(sentinel->selfNode.sizeAndFlag);
        node = OsMemSentinelNodeGet(sentinel);
        sentinel = OS_MEM_END_NODE(node, size);
        count += size;  //ͳ����ߴ�
    }

    return count; //���سߴ�֮��
}


//��ӡ����ڴ�ؼ�Ҫ��Ϣ
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


//��ȡ�ڴ�ص�һЩͳ��ֵ
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


//����ͳ��ֵ��ʾ����
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

//��ʾ�ڴ���еĿ���״��
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
     //�ڴ����ȫ�������������󣬼�Ϊ�ѿռ䣬�ѵĽ�β��ַѡ��1M�ֽڶ��룬�������������ڴ�ӳ�䵽1��ҳ��
    UINTPTR end = ROUNDUP(g_vmBootMemBase + size, MB); //�ѽ�β��ַ
    size = end - g_vmBootMemBase;  //�ں˶ѿռ��ܴ�С

    ptr = OsVmBootMemAlloc(size);  //�ѿռ��ڴ��
    if (!ptr) {
        PRINT_ERR("vmm_kheap_init boot_alloc_mem failed! %d\n", size);
        return -1;
    }

	//��¼�ں˶ѿռ���ʼ��ַ
    m_aucSysMem0 = m_aucSysMem1 = ptr;
    ret = LOS_MemInit(m_aucSysMem0, size); //��ʼ���ں˶ѿռ�
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
