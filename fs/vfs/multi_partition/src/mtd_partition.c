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

// https://www.cnblogs.com/jly594761082/p/10370791.html 
// ���Ŀ����˽�MTD, NOR FLASH�Ĺ�ϵ

#include "mtd_partition.h"
#include "stdlib.h"
#include "stdio.h"
#include "pthread.h"
#include "mtd_list.h"
#include "los_config.h"
#include "los_mux.h"
#include "inode/inode.h"

#if defined(LOSCFG_FS_JFFS)
#include "mtd_common.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define DRIVER_NAME_ADD_SIZE    3
pthread_mutex_t g_mtdPartitionLock = PTHREAD_MUTEX_INITIALIZER;

static INT32 JffsLockInit(VOID) __attribute__((weakref("JffsMutexCreate")));
static VOID JffsLockDeinit(VOID) __attribute__((weakref("JffsMutexDelete")));

partition_param *g_spinorPartParam = NULL;  //��������
mtd_partition *g_spinorPartitionHead = NULL; //������ͷ��

#define RWE_RW_RW 0755

mtd_partition *GetSpinorPartitionHead(VOID) //��ȡ������ͷ��
{
    return g_spinorPartitionHead;
}

//��ʼ������flash��������
static VOID MtdNorParamAssign(partition_param *spinorParam, const struct MtdDev *spinorMtd)
{
    LOS_ListInit(&g_spinorPartitionHead->node_info);
    /*
     * If the user do not want to use block mtd or char mtd ,
     * you can change the SPIBLK_NAME or SPICHR_NAME to NULL.
     */
    spinorParam->flash_mtd = (struct MtdDev *)spinorMtd;
#ifndef LOSCFG_PLATFORM_QEMU_ARM_VIRT_CA7  //QEMUģ����
    spinorParam->flash_ops = GetDevSpinorOps();
    spinorParam->char_ops = GetMtdCharFops();
    spinorParam->blockname = SPIBLK_NAME;  //mtd����spi�ӿ�flash���豸
    spinorParam->charname = SPICHR_NAME;   //mtd����spi�ӿ�flash�ַ��豸
#else  //�����豸
    extern struct block_operations *GetCfiBlkOps(void);
    spinorParam->flash_ops = GetCfiBlkOps(); //���豸������
    spinorParam->char_ops = NULL;
    spinorParam->blockname = "/dev/cfiflash";  //������ͨ�ӿ�flash���豸
    spinorParam->charname = NULL;
#endif
    spinorParam->partition_head = g_spinorPartitionHead;  //����ͷ
    spinorParam->block_size = spinorMtd->eraseSize;  //��ߴ�Ϊ������λ�ߴ�
}

static VOID MtdDeinitSpinorParam(VOID)
{
    if (JffsLockDeinit != NULL) {
        JffsLockDeinit();
    }
}

//��ʼ������flash�ķ�������
static partition_param *MtdInitSpinorParam(partition_param *spinorParam)
{
#ifndef LOSCFG_PLATFORM_QEMU_ARM_VIRT_CA7
    struct MtdDev *spinorMtd = GetMtd("spinor");  //��ȡmtd�豸
#else
    extern struct MtdDev *GetCfiMtdDev(void);
    struct MtdDev *spinorMtd = GetCfiMtdDev();   //��ȡmtd�豸
#endif
    if (spinorMtd == NULL) {
        return NULL;
    }
    if (spinorParam == NULL) {
		//���������ڣ��������
        if (JffsLockInit != NULL) {
			//�ȳ�ʼ��������
            if (JffsLockInit() != 0) { /* create jffs2 lock failed */
                return NULL;
            }
        }
		//�����ڴ�
        spinorParam = (partition_param *)zalloc(sizeof(partition_param));
        if (spinorParam == NULL) {
            PRINT_ERR("%s, partition_param malloc failed\n", __FUNCTION__);
            MtdDeinitSpinorParam();
            return NULL;
        }
		//�����׷���
        g_spinorPartitionHead = (mtd_partition *)zalloc(sizeof(mtd_partition));
        if (g_spinorPartitionHead == NULL) {
            PRINT_ERR("%s, mtd_partition malloc failed\n", __FUNCTION__);
            MtdDeinitSpinorParam();
            free(spinorParam);
            return NULL;
        }

		//��ʼ����������
        MtdNorParamAssign(spinorParam, spinorMtd);
    }

    return spinorParam;  //���ش����õķ�������
}

/* According the flash-type to init the param of the partition. */
//��ʼ������������ֻ֧��spi��cfi�Ĳ���flash
static INT32 MtdInitFsparParam(const CHAR *type, partition_param **fsparParam)
{
    if (strcmp(type, "spinor") == 0 || strcmp(type, "cfi-flash") == 0) {
        g_spinorPartParam = MtdInitSpinorParam(g_spinorPartParam);
        *fsparParam = g_spinorPartParam;
    } else {
        return -EINVAL;
    }

    if ((*fsparParam == NULL) || ((VOID *)((*fsparParam)->flash_mtd) == NULL)) {
        return -ENODEV;
    }

    return ENOERR;
}

/* According the flash-type to deinit the param of the partition. */
//ɾ����������
static INT32 MtdDeinitFsparParam(const CHAR *type)
{
    if (strcmp(type, "spinor") == 0  || strcmp(type, "cfi-flash") == 0) {
        MtdDeinitSpinorParam(); //ɾ����������
        g_spinorPartParam = NULL;
    } else {
        return -EINVAL;
    }

    return ENOERR;
}

//��ӷ���ʱ�Ĳ������
static INT32 AddParamCheck(UINT32 startAddr,
                           const partition_param *param,
                           UINT32 partitionNum,
                           UINT32 length)
{
    UINT32 startBlk, endBlk;
    mtd_partition *node = NULL;
    if ((param->blockname == NULL) && (param->charname == NULL)) {
        return -EINVAL;  //������Ҫָ������һ������
    }

    if ((length == 0) || (length < param->block_size) ||
        (((UINT64)(startAddr) + length) > param->flash_mtd->size)) {
        return -EINVAL; //�������ٰ���1���飬�ҷ�����ַ��Χ����Խ��
    }

	//��ַ�ͳߴ綼���뵽block_size��
    ALIGN_ASSIGN(length, startAddr, startBlk, endBlk, param->block_size);

    if (startBlk > endBlk) {
        return -EINVAL;  //��ʼ���Ų��ܴ��ڽ�������
    }
	//�������з���
    LOS_DL_LIST_FOR_EACH_ENTRY(node, &param->partition_head->node_info, mtd_partition, node_info) {
        if ((node->start_block != 0) && (node->patitionnum == partitionNum)) {
            return -EINVAL;  //�÷����Ѵ���
        }
        if ((startBlk > node->end_block) || (endBlk < node->start_block)) {
            continue; //��ַ��Χ���ܺ����з����ص�
        }
        return -EINVAL;
    }

    return ENOERR;  //������Ӳ������������·���
}

//ע����豸�����Ĳ���
static INT32 BlockDriverRegisterOperate(mtd_partition *newNode,
                                        const partition_param *param,
                                        UINT32 partitionNum)
{
    INT32 ret;
    size_t driverNameSize;

	//����ָ�����豸��
    if (param->blockname != NULL) {
		//���������ļ������ַ���
        driverNameSize = strlen(param->blockname) + DRIVER_NAME_ADD_SIZE;
        newNode->blockdriver_name = (CHAR *)malloc(driverNameSize);
        if (newNode->blockdriver_name == NULL) {
            return -ENOMEM;
        }

		//���������ļ���
        ret = snprintf_s(newNode->blockdriver_name, driverNameSize,
            driverNameSize - 1, "%s%u", param->blockname, partitionNum);
        if (ret < 0) {
            free(newNode->blockdriver_name);
            newNode->blockdriver_name = NULL;
            return -ENAMETOOLONG;
        }

		//ע����豸����
        ret = register_blockdriver(newNode->blockdriver_name, param->flash_ops,
            RWE_RW_RW, newNode);
        if (ret) {
            free(newNode->blockdriver_name);
            newNode->blockdriver_name = NULL;
            PRINT_ERR("register blkdev partion error\n");
            return ret;
        }
    } else {
        newNode->blockdriver_name = NULL;  //�޿��豸��ʱ��Ҳ�����п��豸�����ļ���
    }
    return ENOERR;
}


//�ַ�����ע�����
static INT32 CharDriverRegisterOperate(mtd_partition *newNode,
                                       const partition_param *param,
                                       UINT32 partitionNum)
{
    INT32 ret;
    size_t driverNameSize;

    if (param->charname != NULL) {
		//�����ַ��豸���Ʋ�ע���Ӧ������
        driverNameSize = strlen(param->charname) + DRIVER_NAME_ADD_SIZE;
        newNode->chardriver_name = (CHAR *)malloc(driverNameSize);
        if (newNode->chardriver_name == NULL) {
            return -ENOMEM;
        }

		//�����ַ��豸�����ļ�����
        ret = snprintf_s(newNode->chardriver_name, driverNameSize,
            driverNameSize - 1, "%s%u", param->charname, partitionNum);
        if (ret < 0) {
            free(newNode->chardriver_name);
            newNode->chardriver_name = NULL;
            return -ENAMETOOLONG;
        }

		//ע������
        ret = register_driver(newNode->chardriver_name, param->char_ops, RWE_RW_RW, newNode);
        if (ret) {
            PRINT_ERR("register chardev partion error\n");
            free(newNode->chardriver_name);
            newNode->chardriver_name = NULL;
            return ret;
        }
    } else {
        newNode->chardriver_name = NULL; //���豸��ʱ��Ҳ�������豸�����ļ���
    }
    return ENOERR;
}


//���豸����ע��
static INT32 BlockDriverUnregister(mtd_partition *node)
{
    INT32 ret;

    if (node->blockdriver_name != NULL) {
		//���������ļ�����ע������
        ret = unregister_blockdriver(node->blockdriver_name);
        if (ret == -EBUSY) {
            PRINT_ERR("unregister blkdev partion error:%d\n", ret);
            return ret;
        }
        free(node->blockdriver_name);
        node->blockdriver_name = NULL;
    }
    return ENOERR;
}

//�ַ��豸����ע��
static INT32 CharDriverUnregister(mtd_partition *node)
{
    INT32 ret;

    if (node->chardriver_name != NULL) {
		//�����ַ��豸�����ļ�������ע������
        ret = unregister_driver(node->chardriver_name);
        if (ret == -EBUSY) {
            PRINT_ERR("unregister chardev partion error:%d\n", ret);
            return ret;
        }
        free(node->chardriver_name);
        node->chardriver_name = NULL;
    }

    return ENOERR;
}

/*
 * Attention: both startAddr and length should be aligned with block size.
 * If not, the actual start address and length won't be what you expected.
 */
 //���flash����
INT32 add_mtd_partition(const CHAR *type, UINT32 startAddr,
                        UINT32 length, UINT32 partitionNum)
{
    INT32 ret;
    mtd_partition *newNode = NULL;
    partition_param *param = NULL;

    if ((partitionNum >= CONFIG_MTD_PATTITION_NUM) || (type == NULL)) {
        return -EINVAL; //������Խ�磬��������δָ��
    }

    ret = pthread_mutex_lock(&g_mtdPartitionLock);
    if (ret != ENOERR) {
        PRINT_ERR("%s %d, mutex lock failed, error:%d\n", __FUNCTION__, __LINE__, ret);
    }

    ret = MtdInitFsparParam(type, &param);  //���ݷ������ͻ�ȡ��������
    if (ret != ENOERR) {
        goto ERROR_OUT;
    }

    ret = AddParamCheck(startAddr, param, partitionNum, length); //�������������Լ���ַ��Χ�ͷ�����
    if (ret != ENOERR) {
        goto ERROR_OUT;
    }

	//�����·���
    newNode = (mtd_partition *)zalloc(sizeof(mtd_partition));
    if (newNode == NULL) {
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return -ENOMEM;
    }

	//��ʼ���·���
    PAR_ASSIGNMENT(newNode, length, startAddr, partitionNum, param->flash_mtd, param->block_size);

	//ע���Ӧ�Ŀ��豸����
    ret = BlockDriverRegisterOperate(newNode, param, partitionNum);
    if (ret) {
        goto ERROR_OUT1;
    }

	//ע���Ӧ���ַ��豸����
    ret = CharDriverRegisterOperate(newNode, param, partitionNum);
    if (ret) {
        goto ERROR_OUT2;
    }

	//��������������б�
    LOS_ListTailInsert(&param->partition_head->node_info, &newNode->node_info);
    (VOID)LOS_MuxInit(&newNode->lock, NULL);

    ret = pthread_mutex_unlock(&g_mtdPartitionLock);
    if (ret != ENOERR) {
        PRINT_ERR("%s %d, mutex unlock failed, error:%d\n", __FUNCTION__, __LINE__, ret);
    }

    return ENOERR;
ERROR_OUT2:
    (VOID)BlockDriverUnregister(newNode);
ERROR_OUT1:
    free(newNode);
ERROR_OUT:
    (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
    return ret;
}

//ɾ������ǰ�Ĳ������
static INT32 DeleteParamCheck(UINT32 partitionNum,
                              const CHAR *type,
                              partition_param **param)
{
    if (strcmp(type, "spinor") == 0  || strcmp(type, "cfi-flash") == 0) {
        *param = g_spinorPartParam;  //�������ͻ�ò���
    } else {
        PRINT_ERR("type error \n");  //��֧�ֵ�����
        return -EINVAL;
    }

    if ((partitionNum >= CONFIG_MTD_PATTITION_NUM) || //�����Ų��Ϸ�
        ((*param) == NULL) || ((*param)->flash_mtd == NULL)) { //���Ͳ�����õ���Ϣ���Ϸ�
        return -EINVAL;
    }
    return ENOERR;
}


//ע��������Ӧ������
static INT32 DeletePartitionUnregister(mtd_partition *node)
{
    INT32 ret;

	//ע�������Ŀ��豸����
    ret = BlockDriverUnregister(node);
    if (ret == -EBUSY) {
        return ret;
    }

	//ע���������ַ��豸����
    ret = CharDriverUnregister(node);
    if (ret == -EBUSY) {
        return ret;
    }

    return ENOERR;
}

//���ݷ����Ż�÷����ڵ�
static INT32 OsNodeGet(mtd_partition **node, UINT32 partitionNum, const partition_param *param)
{
    LOS_DL_LIST_FOR_EACH_ENTRY(*node, &param->partition_head->node_info, mtd_partition, node_info) {
        if ((*node)->patitionnum == partitionNum) {
            break;
        }
    }
    if ((*node == NULL) || ((*node)->patitionnum != partitionNum) ||
        ((*node)->mountpoint_name != NULL)) {
        return -EINVAL;
    }

    return ENOERR;
}

//�ͷŷ��������Դ
static INT32 OsResourceRelease(mtd_partition *node, const CHAR *type, partition_param *param)
{
    (VOID)LOS_MuxDestroy(&node->lock);  //ɾ�������ڵ㻥����
    LOS_ListDelete(&node->node_info); //�����ڵ�������Ƴ�
    (VOID)memset_s(node, sizeof(mtd_partition), 0, sizeof(mtd_partition));
    free(node); //�ͷŷ����ڵ�
    (VOID)FreeMtd(param->flash_mtd);  //�ͷ�mtd�豸
    if (LOS_ListEmpty(&param->partition_head->node_info)) {
		//���з�����ɾ��
        free(param->partition_head);  //�ͷŷ�������ͷ��
        param->partition_head = NULL;
        free(param);  //�ͷŷ�������

        if (MtdDeinitFsparParam(type) != ENOERR) {
            return -EINVAL; 
        }
    }
    return ENOERR;
}


//ɾ��flash����
INT32 delete_mtd_partition(UINT32 partitionNum, const CHAR *type)
{
    INT32 ret;
    mtd_partition *node = NULL;
    partition_param *param = NULL;

    if (type == NULL) {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_mtdPartitionLock);
    if (ret != ENOERR) {
        PRINT_ERR("%s %d, mutex lock failed, error:%d\n", __FUNCTION__, __LINE__, ret);
    }

	//ɾ������ǰ���
    ret = DeleteParamCheck(partitionNum, type, &param);
    if (ret) {
        PRINT_ERR("delete_mtd_partition param invalid\n");
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return ret;
    }

	//���ݷ����Ų�ѯ����
    ret = OsNodeGet(&node, partitionNum, param);
    if (ret) {
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return ret; //����������
    }

	//ע��������Ӧ������
    ret = DeletePartitionUnregister(node);
    if (ret) {
        PRINT_ERR("DeletePartitionUnregister error:%d\n", ret);
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return ret;
    }

	//�ͷŷ�����Դ
    ret = OsResourceRelease(node, type, param);
    if (ret) {
        PRINT_ERR("DeletePartitionUnregister error:%d\n", ret);
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return ret;
    }

    ret = pthread_mutex_unlock(&g_mtdPartitionLock);
    if (ret != ENOERR) {
        PRINT_ERR("%s %d, mutex unlock failed, error:%d\n", __FUNCTION__, __LINE__, ret);
    }
    return ENOERR;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
#endif
