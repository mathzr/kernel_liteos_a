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
// 上文可以了解MTD, NOR FLASH的关系

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

partition_param *g_spinorPartParam = NULL;  //分区参数
mtd_partition *g_spinorPartitionHead = NULL; //分区表头部

#define RWE_RW_RW 0755

mtd_partition *GetSpinorPartitionHead(VOID) //获取分区表头部
{
    return g_spinorPartitionHead;
}

//初始化并行flash分区参数
static VOID MtdNorParamAssign(partition_param *spinorParam, const struct MtdDev *spinorMtd)
{
    LOS_ListInit(&g_spinorPartitionHead->node_info);
    /*
     * If the user do not want to use block mtd or char mtd ,
     * you can change the SPIBLK_NAME or SPICHR_NAME to NULL.
     */
    spinorParam->flash_mtd = (struct MtdDev *)spinorMtd;
#ifndef LOSCFG_PLATFORM_QEMU_ARM_VIRT_CA7  //QEMU模拟器
    spinorParam->flash_ops = GetDevSpinorOps();
    spinorParam->char_ops = GetMtdCharFops();
    spinorParam->blockname = SPIBLK_NAME;  //mtd并行spi接口flash块设备
    spinorParam->charname = SPICHR_NAME;   //mtd并行spi接口flash字符设备
#else  //其它设备
    extern struct block_operations *GetCfiBlkOps(void);
    spinorParam->flash_ops = GetCfiBlkOps(); //块设备函数集
    spinorParam->char_ops = NULL;
    spinorParam->blockname = "/dev/cfiflash";  //并行普通接口flash块设备
    spinorParam->charname = NULL;
#endif
    spinorParam->partition_head = g_spinorPartitionHead;  //分区头
    spinorParam->block_size = spinorMtd->eraseSize;  //块尺寸为擦除单位尺寸
}

static VOID MtdDeinitSpinorParam(VOID)
{
    if (JffsLockDeinit != NULL) {
        JffsLockDeinit();
    }
}

//初始化并行flash的分区参数
static partition_param *MtdInitSpinorParam(partition_param *spinorParam)
{
#ifndef LOSCFG_PLATFORM_QEMU_ARM_VIRT_CA7
    struct MtdDev *spinorMtd = GetMtd("spinor");  //读取mtd设备
#else
    extern struct MtdDev *GetCfiMtdDev(void);
    struct MtdDev *spinorMtd = GetCfiMtdDev();   //读取mtd设备
#endif
    if (spinorMtd == NULL) {
        return NULL;
    }
    if (spinorParam == NULL) {
		//参数不存在，则构造参数
        if (JffsLockInit != NULL) {
			//先初始化互斥锁
            if (JffsLockInit() != 0) { /* create jffs2 lock failed */
                return NULL;
            }
        }
		//申请内存
        spinorParam = (partition_param *)zalloc(sizeof(partition_param));
        if (spinorParam == NULL) {
            PRINT_ERR("%s, partition_param malloc failed\n", __FUNCTION__);
            MtdDeinitSpinorParam();
            return NULL;
        }
		//构造首分区
        g_spinorPartitionHead = (mtd_partition *)zalloc(sizeof(mtd_partition));
        if (g_spinorPartitionHead == NULL) {
            PRINT_ERR("%s, mtd_partition malloc failed\n", __FUNCTION__);
            MtdDeinitSpinorParam();
            free(spinorParam);
            return NULL;
        }

		//初始化分区参数
        MtdNorParamAssign(spinorParam, spinorMtd);
    }

    return spinorParam;  //返回创建好的分区参数
}

/* According the flash-type to init the param of the partition. */
//初始化分区参数，只支持spi和cfi的并行flash
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
//删除分区参数
static INT32 MtdDeinitFsparParam(const CHAR *type)
{
    if (strcmp(type, "spinor") == 0  || strcmp(type, "cfi-flash") == 0) {
        MtdDeinitSpinorParam(); //删除分区参数
        g_spinorPartParam = NULL;
    } else {
        return -EINVAL;
    }

    return ENOERR;
}

//添加分区时的参数检查
static INT32 AddParamCheck(UINT32 startAddr,
                           const partition_param *param,
                           UINT32 partitionNum,
                           UINT32 length)
{
    UINT32 startBlk, endBlk;
    mtd_partition *node = NULL;
    if ((param->blockname == NULL) && (param->charname == NULL)) {
        return -EINVAL;  //至少需要指定其中一个名称
    }

    if ((length == 0) || (length < param->block_size) ||
        (((UINT64)(startAddr) + length) > param->flash_mtd->size)) {
        return -EINVAL; //分区至少包含1个块，且分区地址范围不能越界
    }

	//地址和尺寸都对齐到block_size上
    ALIGN_ASSIGN(length, startAddr, startBlk, endBlk, param->block_size);

    if (startBlk > endBlk) {
        return -EINVAL;  //起始块编号不能大于结束块编号
    }
	//遍历已有分区
    LOS_DL_LIST_FOR_EACH_ENTRY(node, &param->partition_head->node_info, mtd_partition, node_info) {
        if ((node->start_block != 0) && (node->patitionnum == partitionNum)) {
            return -EINVAL;  //该分区已存在
        }
        if ((startBlk > node->end_block) || (endBlk < node->start_block)) {
            continue; //地址范围不能和已有分区重叠
        }
        return -EINVAL;
    }

    return ENOERR;  //允许添加参数所描述的新分区
}

//注册块设备驱动的操作
static INT32 BlockDriverRegisterOperate(mtd_partition *newNode,
                                        const partition_param *param,
                                        UINT32 partitionNum)
{
    INT32 ret;
    size_t driverNameSize;

	//必须指定块设备名
    if (param->blockname != NULL) {
		//申请驱动文件名称字符串
        driverNameSize = strlen(param->blockname) + DRIVER_NAME_ADD_SIZE;
        newNode->blockdriver_name = (CHAR *)malloc(driverNameSize);
        if (newNode->blockdriver_name == NULL) {
            return -ENOMEM;
        }

		//构造驱动文件名
        ret = snprintf_s(newNode->blockdriver_name, driverNameSize,
            driverNameSize - 1, "%s%u", param->blockname, partitionNum);
        if (ret < 0) {
            free(newNode->blockdriver_name);
            newNode->blockdriver_name = NULL;
            return -ENAMETOOLONG;
        }

		//注册块设备驱动
        ret = register_blockdriver(newNode->blockdriver_name, param->flash_ops,
            RWE_RW_RW, newNode);
        if (ret) {
            free(newNode->blockdriver_name);
            newNode->blockdriver_name = NULL;
            PRINT_ERR("register blkdev partion error\n");
            return ret;
        }
    } else {
        newNode->blockdriver_name = NULL;  //无块设备名时，也不会有块设备驱动文件名
    }
    return ENOERR;
}


//字符驱动注册操作
static INT32 CharDriverRegisterOperate(mtd_partition *newNode,
                                       const partition_param *param,
                                       UINT32 partitionNum)
{
    INT32 ret;
    size_t driverNameSize;

    if (param->charname != NULL) {
		//存在字符设备名称才注册对应的驱动
        driverNameSize = strlen(param->charname) + DRIVER_NAME_ADD_SIZE;
        newNode->chardriver_name = (CHAR *)malloc(driverNameSize);
        if (newNode->chardriver_name == NULL) {
            return -ENOMEM;
        }

		//构造字符设备驱动文件名称
        ret = snprintf_s(newNode->chardriver_name, driverNameSize,
            driverNameSize - 1, "%s%u", param->charname, partitionNum);
        if (ret < 0) {
            free(newNode->chardriver_name);
            newNode->chardriver_name = NULL;
            return -ENAMETOOLONG;
        }

		//注册驱动
        ret = register_driver(newNode->chardriver_name, param->char_ops, RWE_RW_RW, newNode);
        if (ret) {
            PRINT_ERR("register chardev partion error\n");
            free(newNode->chardriver_name);
            newNode->chardriver_name = NULL;
            return ret;
        }
    } else {
        newNode->chardriver_name = NULL; //无设备明时，也不会有设备驱动文件名
    }
    return ENOERR;
}


//块设备驱动注销
static INT32 BlockDriverUnregister(mtd_partition *node)
{
    INT32 ret;

    if (node->blockdriver_name != NULL) {
		//根据驱动文件名称注销驱动
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

//字符设备驱动注销
static INT32 CharDriverUnregister(mtd_partition *node)
{
    INT32 ret;

    if (node->chardriver_name != NULL) {
		//根据字符设备驱动文件名称来注销驱动
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
 //添加flash分区
INT32 add_mtd_partition(const CHAR *type, UINT32 startAddr,
                        UINT32 length, UINT32 partitionNum)
{
    INT32 ret;
    mtd_partition *newNode = NULL;
    partition_param *param = NULL;

    if ((partitionNum >= CONFIG_MTD_PATTITION_NUM) || (type == NULL)) {
        return -EINVAL; //分区号越界，或者类型未指定
    }

    ret = pthread_mutex_lock(&g_mtdPartitionLock);
    if (ret != ENOERR) {
        PRINT_ERR("%s %d, mutex lock failed, error:%d\n", __FUNCTION__, __LINE__, ret);
    }

    ret = MtdInitFsparParam(type, &param);  //根据分区类型获取分区参数
    if (ret != ENOERR) {
        goto ERROR_OUT;
    }

    ret = AddParamCheck(startAddr, param, partitionNum, length); //检查分区参数，以及地址范围和分区号
    if (ret != ENOERR) {
        goto ERROR_OUT;
    }

	//创建新分区
    newNode = (mtd_partition *)zalloc(sizeof(mtd_partition));
    if (newNode == NULL) {
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return -ENOMEM;
    }

	//初始化新分区
    PAR_ASSIGNMENT(newNode, length, startAddr, partitionNum, param->flash_mtd, param->block_size);

	//注册对应的块设备驱动
    ret = BlockDriverRegisterOperate(newNode, param, partitionNum);
    if (ret) {
        goto ERROR_OUT1;
    }

	//注册对应的字符设备驱动
    ret = CharDriverRegisterOperate(newNode, param, partitionNum);
    if (ret) {
        goto ERROR_OUT2;
    }

	//将分区加入分区列表
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

//删除分区前的参数检查
static INT32 DeleteParamCheck(UINT32 partitionNum,
                              const CHAR *type,
                              partition_param **param)
{
    if (strcmp(type, "spinor") == 0  || strcmp(type, "cfi-flash") == 0) {
        *param = g_spinorPartParam;  //根据类型获得参数
    } else {
        PRINT_ERR("type error \n");  //不支持的类型
        return -EINVAL;
    }

    if ((partitionNum >= CONFIG_MTD_PATTITION_NUM) || //分区号不合法
        ((*param) == NULL) || ((*param)->flash_mtd == NULL)) { //类型参数获得的信息不合法
        return -EINVAL;
    }
    return ENOERR;
}


//注销分区对应的驱动
static INT32 DeletePartitionUnregister(mtd_partition *node)
{
    INT32 ret;

	//注销分区的块设备驱动
    ret = BlockDriverUnregister(node);
    if (ret == -EBUSY) {
        return ret;
    }

	//注销分区的字符设备驱动
    ret = CharDriverUnregister(node);
    if (ret == -EBUSY) {
        return ret;
    }

    return ENOERR;
}

//根据分区号获得分区节点
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

//释放分区相关资源
static INT32 OsResourceRelease(mtd_partition *node, const CHAR *type, partition_param *param)
{
    (VOID)LOS_MuxDestroy(&node->lock);  //删除分区节点互斥锁
    LOS_ListDelete(&node->node_info); //分区节点从链表移除
    (VOID)memset_s(node, sizeof(mtd_partition), 0, sizeof(mtd_partition));
    free(node); //释放分区节点
    (VOID)FreeMtd(param->flash_mtd);  //释放mtd设备
    if (LOS_ListEmpty(&param->partition_head->node_info)) {
		//所有分区已删除
        free(param->partition_head);  //释放分区队列头部
        param->partition_head = NULL;
        free(param);  //释放分区参数

        if (MtdDeinitFsparParam(type) != ENOERR) {
            return -EINVAL; 
        }
    }
    return ENOERR;
}


//删除flash分区
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

	//删除分区前检查
    ret = DeleteParamCheck(partitionNum, type, &param);
    if (ret) {
        PRINT_ERR("delete_mtd_partition param invalid\n");
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return ret;
    }

	//根据分区号查询分区
    ret = OsNodeGet(&node, partitionNum, param);
    if (ret) {
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return ret; //分区不存在
    }

	//注销分区对应的驱动
    ret = DeletePartitionUnregister(node);
    if (ret) {
        PRINT_ERR("DeletePartitionUnregister error:%d\n", ret);
        (VOID)pthread_mutex_unlock(&g_mtdPartitionLock);
        return ret;
    }

	//释放分区资源
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
