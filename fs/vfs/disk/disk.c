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

#include "disk.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "sys/mount.h"
#include "linux/spinlock.h"
#include "inode/inode.h"

#ifdef LOSCFG_DRIVERS_MMC
#include "mmc/block.h"
#endif

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

los_disk g_sysDisk[SYS_MAX_DISK];  //磁盘数组，最多支持5个磁盘
los_part g_sysPart[SYS_MAX_PART];  //磁盘分区数组，最多支持80个分区

//每磁盘块扇区数
UINT32 g_uwFatSectorsPerBlock = CONFIG_FS_FAT_SECTOR_PER_BLOCK;
//FAT磁盘块数
UINT32 g_uwFatBlockNums = CONFIG_FS_FAT_BLOCK_NUMS;

spinlock_t g_diskSpinlock;
spinlock_t g_diskFatBlockSpinlock;

UINT32 g_usbMode = 0;  //当前采用USB方式接入的磁盘，用位图表示

#define MEM_ADDR_ALIGN_BYTE  64    //地址对齐
#define RWE_RW_RW            0755  //权限


#define DISK_LOCK(mux) do {                                              \
    if (pthread_mutex_lock(mux) != 0) {                                  \
        PRINT_ERR("%s %d, mutex lock failed\n", __FUNCTION__, __LINE__); \
    }                                                                    \
} while (0)

#define DISK_UNLOCK(mux) do {                                              \
    if (pthread_mutex_unlock(mux) != 0) {                                  \
        PRINT_ERR("%s %d, mutex unlock failed\n", __FUNCTION__, __LINE__); \
    }                                                                      \
} while (0)

typedef VOID *(*StorageHookFunction)(VOID *);

static UINT32 OsReHookFuncAddDiskRef(StorageHookFunction handler,
                                     VOID *param) __attribute__((weakref("osReHookFuncAdd")));

static UINT32 OsReHookFuncDelDiskRef(StorageHookFunction handler) __attribute__((weakref("osReHookFuncDel")));

#ifdef LOSCFG_FS_FAT_CACHE
UINT32 GetFatBlockNums(VOID)
{
    return g_uwFatBlockNums;  //FAT磁盘块数目
}

VOID SetFatBlockNums(UINT32 blockNums)
{
    g_uwFatBlockNums = blockNums; //设置FAT磁盘块数目
}

UINT32 GetFatSectorsPerBlock(VOID)
{
    return g_uwFatSectorsPerBlock; //FAT磁盘块内扇区数目
}

VOID SetFatSectorsPerBlock(UINT32 sectorsPerBlock)
{
    if (((sectorsPerBlock % UNSIGNED_INTEGER_BITS) == 0) &&
        ((sectorsPerBlock >> UNINT_LOG2_SHIFT) <= BCACHE_BLOCK_FLAGS)) {
        g_uwFatSectorsPerBlock = sectorsPerBlock; //设置FAT磁盘块内扇区数目
    }
}
#endif

//申请磁盘ID，并填入磁盘名称
INT32 los_alloc_diskid_byname(const CHAR *diskName)
{
    INT32 diskID;
    los_disk *disk = NULL;
    UINT32 intSave;
    size_t nameLen;

    if (diskName == NULL) {
        PRINT_ERR("The paramter disk_name is NULL");
        return VFS_ERROR;
    }

    nameLen = strlen(diskName);
    if (nameLen > DISK_NAME) {
        PRINT_ERR("diskName is too long!\n");
        return VFS_ERROR;
    }
    spin_lock_irqsave(&g_diskSpinlock, intSave);

	//遍历磁盘描述符
    for (diskID = 0; diskID < SYS_MAX_DISK; diskID++) {
        disk = get_disk(diskID);
        if ((disk != NULL) && (disk->disk_status == STAT_UNUSED)) {
			//寻找到空闲描述符，标记已占用，但未初始化
            disk->disk_status = STAT_UNREADY;
            break;
        }
    }

    spin_unlock_irqrestore(&g_diskSpinlock, intSave);

    if ((disk == NULL) || (diskID == SYS_MAX_DISK)) {
		//没有找到空闲磁盘描述符
        PRINT_ERR("los_alloc_diskid_byname failed %d!\n", diskID);
        return VFS_ERROR;
    }

    if (disk->disk_name != NULL) {
		//释放原有名称
        LOS_MemFree(m_aucSysMem0, disk->disk_name);
        disk->disk_name = NULL;
    }

	//记录新磁盘名称
    disk->disk_name = LOS_MemAlloc(m_aucSysMem0, (nameLen + 1));
    if (disk->disk_name == NULL) {
        PRINT_ERR("los_alloc_diskid_byname alloc disk name failed\n");
        return VFS_ERROR;
    }

    if (strncpy_s(disk->disk_name, (nameLen + 1), diskName, nameLen) != EOK) {
        PRINT_ERR("The strncpy_s failed.\n");
        return VFS_ERROR;
    }

    disk->disk_name[nameLen] = '\0';

    return diskID; //返回磁盘ID
}

//根据名称查询磁盘ID
INT32 los_get_diskid_byname(const CHAR *diskName)
{
    INT32 diskID;
    los_disk *disk = NULL;
    size_t diskNameLen;

    if (diskName == NULL) {
        PRINT_ERR("The paramter diskName is NULL");
        return VFS_ERROR;
    }

    diskNameLen = strlen(diskName);
    if (diskNameLen > DISK_NAME) {
        PRINT_ERR("diskName is too long!\n");
        return VFS_ERROR;
    }

	//遍历磁盘描述符
    for (diskID = 0; diskID < SYS_MAX_DISK; diskID++) {
        disk = get_disk(diskID);
        if ((disk != NULL) && (disk->disk_name != NULL) && (disk->disk_status == STAT_INUSED)) {
            if (strlen(disk->disk_name) != diskNameLen) {
                continue;  //磁盘名称长度不匹配
            }
            if (strcmp(diskName, disk->disk_name) == 0) {
                break; //磁盘名称匹配
            }
        }
    }
    if ((disk == NULL) || (diskID == SYS_MAX_DISK)) {
        PRINT_ERR("los_get_diskid_byname failed!\n");
        return VFS_ERROR;  //查询失败
    }
    return diskID;  //查询成功，返回ID
}

//设置磁盘通过usb连接
VOID OsSetUsbStatus(UINT32 diskID)
{
    if (diskID < SYS_MAX_DISK) {
        g_usbMode |= (1u << diskID) & UINT_MAX;
    }
}

//取消磁盘通过usb连接
VOID OsClearUsbStatus(UINT32 diskID)
{
    if (diskID < SYS_MAX_DISK) {
        g_usbMode &= ~((1u << diskID) & UINT_MAX);
    }
}

#ifdef LOSCFG_FS_FAT_CACHE
//获取磁盘是否通过usb连接
static BOOL GetDiskUsbStatus(UINT32 diskID)
{
    return (g_usbMode & (1u << diskID)) ? TRUE : FALSE;
}
#endif

//获取磁盘描述符
los_disk *get_disk(INT32 id)
{
    if ((id >= 0) && (id < SYS_MAX_DISK)) {
        return &g_sysDisk[id];
    }

    return NULL;  //id不合法
}

//获取磁盘分区，根据系统分区id
los_part *get_part(INT32 id)
{
    if ((id >= 0) && (id < SYS_MAX_PART)) {
        return &g_sysPart[id];
    }

    return NULL;  //id不合法
}

//根据某分区获取本磁盘首分区的起始扇区编号，不一定是0
static UINT64 GetFirstPartStart(const los_part *part)
{
    los_part *firstPart = NULL;
    los_disk *disk = get_disk((INT32)part->disk_id);
    firstPart = (disk == NULL) ? NULL : LOS_DL_LIST_ENTRY(disk->head.pstNext, los_part, list);
    return (firstPart == NULL) ? 0 : firstPart->sector_start;
}

//向磁盘添加分区
static VOID DiskPartAddToDisk(los_disk *disk, los_part *part)
{
    part->disk_id = disk->disk_id;  //分区中也记录磁盘id信息，才知道本分区属于哪个磁盘
    part->part_no_disk = disk->part_count; //磁盘内的分区号
    LOS_ListTailInsert(&disk->head, &part->list); //放入磁盘内分区队列
    disk->part_count++;  //磁盘内下一次添加分区时的分区号
}

//删除分区
static VOID DiskPartDelFromDisk(los_disk *disk, los_part *part)
{
    LOS_ListDelete(&part->list); //分区描述符从磁盘分区队列中移除
    disk->part_count--;  //分区数减少
}

//申请并初始化分区描述符部分信息
//dev代表了磁盘设备文件，start和count表示分区起始扇区和大小
static los_part *DiskPartAllocate(struct inode *dev, UINT64 start, UINT64 count)
{
    UINT32 i;
	//从0号分区描述符开始遍历
    los_part *part = get_part(0); /* traversing from the beginning of the array */

    for (i = 0; i < SYS_MAX_PART; i++) {
		//寻找还未使用的描述符
        if (part->dev == NULL) {
			//找到描述符
            part->part_id = i;  //记录下系统分区编号
            part->part_no_mbr = 0;  //TBD
            part->dev = dev;  //记录分区所在的磁盘设备
            part->sector_start = start;  //记录对应的磁盘起始扇区号
            part->sector_count = count;  //记录扇区数
            part->part_name = NULL;      //后面指定分区名称
            LOS_ListInit(&part->list);   //后面再加入分区队列

            return part;  //返回分区描述符
        }
        part++;  //继续寻找下一个分区
    }

    return NULL;  //没有找到空闲分区
}

//释放分区
static VOID DiskPartRelease(los_part *part)
{
    part->dev = NULL;  //不再是某磁盘的分区
    part->part_no_disk = 0;  //取消磁盘内分区编号
    part->part_no_mbr = 0;   //TBD
    if (part->part_name != NULL) {
        free(part->part_name);  //释放分区名
        part->part_name = NULL;
    }
}

/*
 * name is a combination of disk_name, 'p' and part_count, such as "/dev/mmcblk0p0"
 * disk_name : DISK_NAME + 1
 * 'p' : 1
 * part_count: 1
 */
#define DEV_NAME_BUFF_SIZE  (DISK_NAME + 3)

//添加磁盘分区，disk磁盘，sectorStart起始扇区编号,sectorCount分区所包含扇区数
static INT32 DiskAddPart(los_disk *disk, UINT64 sectorStart, UINT64 sectorCount)
{
    CHAR devName[DEV_NAME_BUFF_SIZE];
    struct inode *diskDev = NULL;
    struct inode *partDev = NULL;
    los_part *part = NULL;
    INT32 ret;
    struct inode_search_s desc;

    if ((disk == NULL) || (disk->disk_status == STAT_UNUSED) ||
        (disk->dev == NULL)) {  //需要磁盘驱动先加载，磁盘被识别出来后，才能分区
        return VFS_ERROR;  
    }

    if ((sectorCount > disk->sector_count) || ((disk->sector_count - sectorCount) < sectorStart)) {
        PRINT_ERR("DiskAddPart failed: sector start is %llu, sector count is %llu\n", sectorStart, sectorCount);
        return VFS_ERROR; //分区中的扇区数或者扇区范围越界
    }

	//根据磁盘磁盘名称构造分区名称  . 例如 从 /dev/hda构造/dev/hdap1
    ret = snprintf_s(devName, sizeof(devName), sizeof(devName) - 1, "%s%c%u",
                     (disk->disk_name == NULL ? "null" : disk->disk_name), 'p', disk->part_count);
    if (ret < 0) {
        return VFS_ERROR;
    }

    diskDev = disk->dev;  //磁盘设备驱动识别出来的磁盘设备
	//基于磁盘设备驱动来注册分区设备驱动(一个磁盘逻辑上切分成了多个磁盘)
	//除了设备名称不同，其它信息和原始磁盘一致
    if (register_blockdriver(devName, diskDev->u.i_bops, RWE_RW_RW, diskDev->i_private)) {
        PRINT_ERR("DiskAddPart : register %s fail!\n", devName);
        return VFS_ERROR;
    }

	//查找设备文件是否存在(驱动是否注册成功)
    SETUP_SEARCH(&desc, devName, false);
    ret = inode_find(&desc);
    if (ret < 0) {
        PRINT_ERR("DiskAddPart : find %s fail!\n", devName);
        return VFS_ERROR;
    }
    partDev = desc.node;  //分区设备驱动注册成功

    PRINTK("DiskAddPart : register %s ok!\n", devName);

	//分配分区描述符并初始化
    part = DiskPartAllocate(partDev, sectorStart, sectorCount);
    inode_release(partDev); //释放设备文件索引节点--引用计数

    if (part == NULL) {
		//分区分配失败，则注销分区设备驱动，会删除分区设备文件
        (VOID)unregister_blockdriver(devName);
        return VFS_ERROR;
    }

    DiskPartAddToDisk(disk, part);  //将分区加入磁盘
    if (disk->type == EMMC) {
        part->type = EMMC;  //更新分区类型
    }
    return (INT32)part->part_id;  //返回分区ID
}

//根据分区信息对磁盘进行分区
static INT32 DiskDivide(los_disk *disk, struct disk_divide_info *info)
{
    UINT32 i;
    INT32 ret;

	//磁盘类型与第一个分区的类型保持一致
    disk->type = info->part[0].type;
	//逐一创建分区
    for (i = 0; i < info->part_count; i++) {
        if (info->sector_count < info->part[i].sector_start) {
            return VFS_ERROR;  //分区的起始扇区号过大
        }
        if (info->part[i].sector_count > (info->sector_count - info->part[i].sector_start)) {
			//分区的扇区范围越界
            PRINT_ERR("Part[%u] sector_start:%llu, sector_count:%llu, exceed emmc sector_count:%llu.\n", i,
                      info->part[i].sector_start, info->part[i].sector_count,
                      (info->sector_count - info->part[i].sector_start));
			//调小分区扇区数，使得其刚好不越界
            info->part[i].sector_count = info->sector_count - info->part[i].sector_start;
            PRINT_ERR("Part[%u] sector_count change to %llu.\n", i, info->part[i].sector_count);

			//根据分区信息添加分区
            ret = DiskAddPart(disk, info->part[i].sector_start, info->part[i].sector_count);
            if (ret == VFS_ERROR) {
                return VFS_ERROR;
            }
            break;  //扇区已用完，不能再加新分区了
        }
		//根据分区信息正常添加分区
        ret = DiskAddPart(disk, info->part[i].sector_start, info->part[i].sector_count);
        if (ret == VFS_ERROR) {
            return VFS_ERROR;
        }
    }

    return ENOERR;  //根据分区信息正常完成分区
}


//文件系统类型识别
static CHAR GPTPartitionTypeRecognition(const CHAR *parBuf)
{
    const CHAR *buf = parBuf;
    const CHAR *fsType = "FAT";
    const CHAR *str = "\xEB\x52\x90" "NTFS    "; /* NTFS Boot entry point */

    if (((LD_DWORD_DISK(&buf[BS_FILSYSTEMTYPE32]) & BS_FS_TYPE_MASK) == BS_FS_TYPE_VALUE) ||
        (strncmp(&buf[BS_FILSYSTYPE], fsType, strlen(fsType)) == 0)) {
        return BS_FS_TYPE_FAT;  //FAT文件系统
    } else if (strncmp(&buf[BS_JMPBOOT], str, strlen(str)) == 0) {
        return BS_FS_TYPE_NTFS; //NTFS文件系统
    }

    return ENOERR;
}


//分配用于处理磁盘分区逻辑的内存，记录到输出参数中
static INT32 DiskPartitionMemZalloc(size_t boundary, size_t size, CHAR **gptBuf, CHAR **partitionBuf)
{
    CHAR *buffer1 = NULL;
    CHAR *buffer2 = NULL;

    buffer1 = (CHAR *)memalign(boundary, size);
    if (buffer1 == NULL) {
        PRINT_ERR("%s buffer1 malloc %lu failed! %d\n", __FUNCTION__, size, __LINE__);
        return VFS_ERROR;
    }
    buffer2 = (CHAR *)memalign(boundary, size);
    if (buffer2 == NULL) {
        PRINT_ERR("%s buffer2 malloc %lu failed! %d\n", __FUNCTION__, size, __LINE__);
        free(buffer1);
        return VFS_ERROR;
    }
    (VOID)memset_s(buffer1, size, 0, size);
    (VOID)memset_s(buffer2, size, 0, size);

    *gptBuf = buffer1;
    *partitionBuf = buffer2;

    return ENOERR;
}


//获取GPT信息
static INT32 GPTInfoGet(struct inode *blkDrv, CHAR *gptBuf)
{
    INT32 ret;

	//从磁盘第1个扇区读入数据
    ret = blkDrv->u.i_bops->read(blkDrv, (UINT8 *)gptBuf, 1, 1); /* Read the device first sector */
    if (ret != 1) { /* Read failed */
        PRINT_ERR("%s %d\n", __FUNCTION__, __LINE__);
        return -EIO;
    }

    if (!VERIFY_GPT(gptBuf)) {
        PRINT_ERR("%s %d\n", __FUNCTION__, __LINE__);
        return VFS_ERROR;  //只支持GPT
    }

    return ENOERR;
}

//构造磁盘分区信息
static INT32 OsGPTPartitionRecognitionSub(struct disk_divide_info *info, const CHAR *partitionBuf,
                                          UINT32 *partitionCount, UINT64 partitionStart, UINT64 partitionEnd)
{
    CHAR partitionType;

    if (VERIFY_FS(partitionBuf)) {
		//只支持FAT和NTFS
        partitionType = GPTPartitionTypeRecognition(partitionBuf);
        if (partitionType) {
            if (*partitionCount >= MAX_DIVIDE_PART_PER_DISK) {
                return VFS_ERROR;  //每个磁盘最多分成16个分区，当前分区号必须小于这个16
            }
            info->part[*partitionCount].type = partitionType;  //文件系统类型
            info->part[*partitionCount].sector_start = partitionStart; //起始扇区
            info->part[*partitionCount].sector_count = (partitionEnd - partitionStart) + 1; //扇区数
            (*partitionCount)++;  //下一个添加的分区对应的分区号
        } else {
            PRINT_ERR("The partition type is not allowed to use!\n");
        }
    } else {
        PRINT_ERR("Do not support the partition type!\n");
    }
    return ENOERR;
}


//构造磁盘分区信息
static INT32 OsGPTPartitionRecognition(struct inode *blkDrv, struct disk_divide_info *info,
                                       const CHAR *gptBuf, CHAR *partitionBuf, UINT32 *partitionCount)
{
    UINT32 j;
    INT32 ret = VFS_ERROR;
    UINT64 partitionStart, partitionEnd;

	//遍历扇区存储的分区信息，一个扇区最多存储4个分区信息
    for (j = 0; j < PAR_ENTRY_NUM_PER_SECTOR; j++) {
        if (!VERITY_AVAILABLE_PAR(&gptBuf[j * TABLE_SIZE])) {
            PRINTK("The partition type is ESP or MSR!\n");  //分区类型只能是ESP或者MSR
            continue;
        }

        if (!VERITY_PAR_VALID(&gptBuf[j * TABLE_SIZE])) {
            return VFS_ERROR;  //不能为0
        }

        partitionStart = LD_QWORD_DISK(&gptBuf[(j * TABLE_SIZE) + GPT_PAR_START_OFFSET]);  //分区起始扇区号
        partitionEnd = LD_QWORD_DISK(&gptBuf[(j * TABLE_SIZE) + GPT_PAR_END_OFFSET]);  //分区结束扇区号
        if ((partitionStart >= partitionEnd) || (partitionEnd > info->sector_count)) {
            PRINT_ERR("GPT partition %u recognition failed : partitionStart = %llu, partitionEnd = %llu\n",
                      j, partitionStart, partitionEnd);
            return VFS_ERROR;  //起始扇区必须小于结束扇区，结束扇区不能比磁盘最大扇区号大
        }
		
        (VOID)memset_s(partitionBuf, info->sector_size, 0, info->sector_size);
		//读入本分区的第一个扇区的内容
        ret = blkDrv->u.i_bops->read(blkDrv, (UINT8 *)partitionBuf, partitionStart, 1);
        if (ret != 1) { /* read failed */
            PRINT_ERR("%s %d\n", __FUNCTION__, __LINE__);
            return -EIO;
        }

		//根据现有信息构造磁盘分区信息
		//partitionCount分区号, partitionStart起始扇区，partitionEnd扇区数，partitionBuf内部可以提取出文件系统类型
        ret = OsGPTPartitionRecognitionSub(info, partitionBuf, partitionCount, partitionStart, partitionEnd);
        if (ret != ENOERR) {
            return VFS_ERROR;
        }
    }

    return ret;
}
//从GPT(全局分区表)中识别出分区信息
static INT32 DiskGPTPartitionRecognition(struct inode *blkDrv, struct disk_divide_info *info)
{
    CHAR *gptBuf = NULL;
    CHAR *partitionBuf = NULL;
    UINT32 tableNum, i, index;
    UINT32 partitionCount = 0;
    INT32 ret;

	//申请2个缓冲区，用于读取全局分区表和分区中第一个扇区内容
    ret = DiskPartitionMemZalloc(MEM_ADDR_ALIGN_BYTE, info->sector_size, &gptBuf, &partitionBuf);
    if (ret != ENOERR) {
        return VFS_ERROR;
    }

    ret = GPTInfoGet(blkDrv, gptBuf); //从磁盘读入全局分区表
    if (ret < 0) {
        goto OUT_WITH_MEM; //全局分区表读取失败
    }

    tableNum = LD_DWORD_DISK(&gptBuf[TABLE_NUM_OFFSET]); //分区表数目
    if (tableNum > TABLE_MAX_NUM) {
        tableNum = TABLE_MAX_NUM;  //分区表不能过大
    }

    index = (tableNum % PAR_ENTRY_NUM_PER_SECTOR) ? ((tableNum / PAR_ENTRY_NUM_PER_SECTOR) + 1) :
            (tableNum / PAR_ENTRY_NUM_PER_SECTOR);  //计算需要继续读取的扇区数，向上取整

	//访问每一个分区表项
    for (i = 0; i < index; i++) {
		//从每一个扇区读出分区表
        (VOID)memset_s(gptBuf, info->sector_size, 0, info->sector_size);
        ret = blkDrv->u.i_bops->read(blkDrv, (UINT8 *)gptBuf, TABLE_START_SECTOR + i, 1);
        if (ret != 1) { /* read failed */
            PRINT_ERR("%s %d\n", __FUNCTION__, __LINE__);
            ret = -EIO;
            goto OUT_WITH_MEM;
        }

		//根据分区表信息来构造磁盘分区信息结构info
        ret = OsGPTPartitionRecognition(blkDrv, info, gptBuf, partitionBuf, &partitionCount);
        if (ret < 0) {
            if (ret == VFS_ERROR) {
                ret = (INT32)partitionCount;
            }
            goto OUT_WITH_MEM;
        }
    }
    ret = (INT32)partitionCount;  //最后返回分区信息中总的分区数目

OUT_WITH_MEM:
    free(gptBuf);
    free(partitionBuf);
    return ret;
}


//读取MBR扇区(主引导记录)
static INT32 OsMBRInfoGet(struct inode *blkDrv, CHAR *mbrBuf)
{
    INT32 ret;

    /* read MBR, start from sector 0, length is 1 sector */
	//主引导记录在0号扇区
    ret = blkDrv->u.i_bops->read(blkDrv, (UINT8 *)mbrBuf, 0, 1);
    if (ret != 1) { /* read failed */
        PRINT_ERR("driver read return error: %d\n", ret);
        return -EIO;
    }

    /* Check boot record signature. */
    if (LD_WORD_DISK(&mbrBuf[BS_SIG55AA]) != BS_SIG55AA_VALUE) {
        return VFS_ERROR;  //检查签名确认其为主引导记录
    }

    return ENOERR;
}

//扩展引导记录获取(EBR)
static INT32 OsEBRInfoGet(struct inode *blkDrv, const struct disk_divide_info *info,
                          CHAR *ebrBuf, const CHAR *mbrBuf)
{
    INT32 ret;

    if (VERIFY_FS(mbrBuf)) {
		//对于FAT和NTFS，除了主引导记录，还有扩展引导记录
        if (info->sector_count <= LD_DWORD_DISK(&mbrBuf[PAR_OFFSET + PAR_START_OFFSET])) {
            return VFS_ERROR;  //起始扇区超过了总扇区数
        }

		//从分区的起始扇区中读入扩展引导记录
        ret = blkDrv->u.i_bops->read(blkDrv, (UINT8 *)ebrBuf,
                                     LD_DWORD_DISK(&mbrBuf[PAR_OFFSET + PAR_START_OFFSET]), 1);
        if ((ret != 1) || (!VERIFY_FS(ebrBuf))) { /* read failed */
            PRINT_ERR("OsEBRInfoGet, verify_fs error, ret = %d\n", ret);
            return -EIO;
        }
    }

    return ENOERR;
}


//主分区信息获取
static INT32 OsPrimaryPartitionRecognition(const CHAR *mbrBuf, struct disk_divide_info *info,
                                           INT32 *extendedPos, INT32 *mbrCount)
{
    INT32 i;
    CHAR mbrPartitionType;
    INT32 extendedFlag = 0;
    INT32 count = 0;

	//每个磁盘最多4个主分区
    for (i = 0; i < MAX_PRIMARY_PART_PER_DISK; i++) {
        mbrPartitionType = mbrBuf[PAR_OFFSET + PAR_TYPE_OFFSET + (i * PAR_TABLE_SIZE)]; //主分区类型
        if (mbrPartitionType) {
            info->part[i].type = mbrPartitionType; //记录分区类型
            //记录分区起始扇区
            info->part[i].sector_start = LD_DWORD_DISK(&mbrBuf[PAR_OFFSET + PAR_START_OFFSET + (i * PAR_TABLE_SIZE)]);
			//记录分区扇区数
            info->part[i].sector_count = LD_DWORD_DISK(&mbrBuf[PAR_OFFSET + PAR_COUNT_OFFSET + (i * PAR_TABLE_SIZE)]);
            if ((mbrPartitionType == EXTENDED_PAR) || (mbrPartitionType == EXTENDED_8G)) {
                extendedFlag = 1;  //扩展分区
                *extendedPos = i;  //扩展分区的序号
                continue;
            }
            count++;  //主分区数统计
        }
    }
    *mbrCount = count;  //记录主分区数

    return extendedFlag;  //是否存在扩展分区
}


//逻辑分区信息获取
static INT32 OsLogicalPartitionRecognition(struct inode *blkDrv, struct disk_divide_info *info,
                                           UINT32 extendedAddress, CHAR *ebrBuf, INT32 mbrCount)
{
    INT32 ret;
    UINT32 extendedOffset = 0;
    CHAR ebrPartitionType;
    INT32 ebrCount = 0;

    do {
        (VOID)memset_s(ebrBuf, info->sector_size, 0, info->sector_size);
        if (((UINT64)(extendedAddress) + extendedOffset) >= info->sector_count) {
            PRINT_ERR("extended partition is out of disk range: extendedAddress = %u, extendedOffset = %u\n",
                      extendedAddress, extendedOffset);  //逻辑分区扇区号越界
            break;
        }
		//读取逻辑分区扇区
        ret = blkDrv->u.i_bops->read(blkDrv, (UINT8 *)ebrBuf,
                                     extendedAddress + extendedOffset, 1);
        if (ret != 1) { /* read failed */
            PRINT_ERR("driver read return error: %d, extendedAddress = %u, extendedOffset = %u\n", ret,
                      extendedAddress, extendedOffset);
            return -EIO;
        }
        ebrPartitionType = ebrBuf[PAR_OFFSET + PAR_TYPE_OFFSET];  //读取分区类型
        if (ebrPartitionType && ((mbrCount + ebrCount) < MAX_DIVIDE_PART_PER_DISK)) {
			//每磁盘主分区和逻辑分区之和不能超过16个
            info->part[MAX_PRIMARY_PART_PER_DISK + ebrCount].type = ebrPartitionType;  //逻辑分区类型
            //起始扇区编号
            info->part[MAX_PRIMARY_PART_PER_DISK + ebrCount].sector_start = extendedAddress + extendedOffset +
                                                                            LD_DWORD_DISK(&ebrBuf[PAR_OFFSET +
                                                                                                  PAR_START_OFFSET]);
			//扇区数
            info->part[MAX_PRIMARY_PART_PER_DISK + ebrCount].sector_count = LD_DWORD_DISK(&ebrBuf[PAR_OFFSET +
                                                                                                  PAR_COUNT_OFFSET]);
            ebrCount++;  //下一个逻辑分区
        }
		//读入下一个分区的起始扇区号
        extendedOffset = LD_DWORD_DISK(&ebrBuf[PAR_OFFSET + PAR_START_OFFSET + PAR_TABLE_SIZE]);
    } while ((ebrBuf[PAR_OFFSET + PAR_TYPE_OFFSET + PAR_TABLE_SIZE] != 0) && //存在下一个分区
             ((mbrCount + ebrCount) < MAX_DIVIDE_PART_PER_DISK));  //现有分区数还没有达到容量上限

    return ebrCount;  //返回扩展分区数
}


//获取磁盘分区信息
static INT32 DiskPartitionRecognition(struct inode *blkDrv, struct disk_divide_info *info)
{
    INT32 ret;
    INT32 extendedFlag;
    INT32 extendedPos = 0;
    INT32 mbrCount = 0;
    UINT32 extendedAddress;
    CHAR *mbrBuf = NULL;
    CHAR *ebrBuf = NULL;

    if ((blkDrv == NULL) || (blkDrv->u.i_bops == NULL) || (blkDrv->u.i_bops->read == NULL)) {
        return VFS_ERROR;
    }

	//先申请2个缓冲区用于读入磁盘扇区数据
    ret = DiskPartitionMemZalloc(MEM_ADDR_ALIGN_BYTE, info->sector_size, &mbrBuf, &ebrBuf);
    if (ret != ENOERR) {
        return ret;
    }

    ret = OsMBRInfoGet(blkDrv, mbrBuf);  //读取主引导扇区数据
    if (ret < 0) {
        goto OUT_WITH_MEM;
    }

    /* The partition type is GPT */
    if (mbrBuf[PARTION_MODE_BTYE] == (CHAR)PARTION_MODE_GPT) {
		//GPT分区方法，获取分区信息
        ret = DiskGPTPartitionRecognition(blkDrv, info);
        goto OUT_WITH_MEM;
    }

	//MBR分区方法，继续获取分区信息
    ret = OsEBRInfoGet(blkDrv, info, ebrBuf, mbrBuf);  //读入扩展引导记录
    if (ret < 0) {
        ret = 0; /* no mbr */
        goto OUT_WITH_MEM;
    }

	//读入主分区数据
    extendedFlag = OsPrimaryPartitionRecognition(mbrBuf, info, &extendedPos, &mbrCount);
    if (extendedFlag) {
		//存在扩展分区的情况下
        extendedAddress = LD_DWORD_DISK(&mbrBuf[PAR_OFFSET + PAR_START_OFFSET + (extendedPos * PAR_TABLE_SIZE)]);
		//继续读入扩展分区数据
        ret = OsLogicalPartitionRecognition(blkDrv, info, extendedAddress, ebrBuf, mbrCount);
        if (ret <= 0) {
            goto OUT_WITH_MEM;
        }
    }
    ret += mbrCount;

OUT_WITH_MEM:
    free(ebrBuf);
    free(mbrBuf);
    return ret;
}


//对磁盘进行分区
INT32 DiskPartitionRegister(los_disk *disk)
{
    INT32 count;
    UINT32 i, partSize;
    los_part *part = NULL;
    struct disk_divide_info parInfo;

    /* Fill disk_divide_info structure to set partition's infomation. */
    (VOID)memset_s(parInfo.part, sizeof(parInfo.part), 0, sizeof(parInfo.part));
    partSize = sizeof(parInfo.part) / sizeof(parInfo.part[0]);  //最大分区数

    parInfo.sector_size = disk->sector_size;   //磁盘扇区尺寸
    parInfo.sector_count = disk->sector_count; //磁盘扇区数
    count = DiskPartitionRecognition(disk->dev, &parInfo); //从磁盘中读取分区信息
    if (count < 0) {
        return VFS_ERROR;  //错误
    }
    parInfo.part_count = count;  //记录分区数
    if (count == 0) {
		//无分区，即整个磁盘一个分区
        part = get_part(DiskAddPart(disk, 0, disk->sector_count));
        if (part == NULL) {
            return VFS_ERROR;
        }
        part->part_no_mbr = 0;  //所以也没有主引导分区

        PRINTK("No MBR detected.\n");
        return ENOERR;
    }

	//存在多个分区
    for (i = 0; i < partSize; i++) {
        /* Read the disk_divide_info structure to get partition's infomation. */
        if ((parInfo.part[i].type != 0) && (parInfo.part[i].type != EXTENDED_PAR) &&
            (parInfo.part[i].type != EXTENDED_8G)) {
            //不是扩展分区，则添加分区描述符
            part = get_part(DiskAddPart(disk, parInfo.part[i].sector_start, parInfo.part[i].sector_count));
            if (part == NULL) {
                return VFS_ERROR;
            }
            part->part_no_mbr = i + 1;  //并记录下主分区编号，编号从1开始
            part->filesystem_type = parInfo.part[i].type; //记录分区的文件系统类型
        }
    }

    return ENOERR;
}


//从磁盘读取若干扇区数据
//drvID, 磁盘ID, buf数据读取后存放位置，sector起始扇区，count扇区数
INT32 los_disk_read(INT32 drvID, VOID *buf, UINT64 sector, UINT32 count)
{
#ifdef LOSCFG_FS_FAT_CACHE
    UINT32 len;
#endif
    INT32 result = VFS_ERROR;
    los_disk *disk = get_disk(drvID);  //获取磁盘描述符

    if ((buf == NULL) || (count == 0)) { /* buff equal to NULL or count equal to 0 */
        return result;
    }

    if (disk == NULL) {
        return result;
    }

    DISK_LOCK(&disk->disk_mutex);

    if (disk->disk_status != STAT_INUSED) {
        goto ERROR_HANDLE;  //本磁盘还未使用
    }

    if ((count > disk->sector_count) || ((disk->sector_count - count) < sector)) {
        goto ERROR_HANDLE;  //扇区数越界，或者扇区范围越界
    }

#ifdef LOSCFG_FS_FAT_CACHE
    if (disk->bcache != NULL) {
        if (((UINT64)(disk->bcache->sectorSize) * count) > UINT_MAX) {
            goto ERROR_HANDLE; //磁盘太大，本系统不支持
        }
        len = disk->bcache->sectorSize * count;  //需要读入的字节数
        //将磁盘数据通过块缓存读入
        result = BlockCacheRead(disk->bcache, (UINT8 *)buf, &len, sector);
        if (result != ENOERR) {
            PRINT_ERR("los_disk_read read err = %d, sector = %llu, len = %u\n", result, sector, len);
        }
    } else {
#endif
	//无块缓存的系统
    if ((disk->dev != NULL) && (disk->dev->u.i_bops != NULL) && (disk->dev->u.i_bops->read != NULL)) {
		//直接使用驱动的磁盘读操作
        result = disk->dev->u.i_bops->read(disk->dev, (UINT8 *)buf, sector, count);
        if (result == (INT32)count) {
            result = ENOERR;  //成功读入用户要求的扇区数
        }
    }
#ifdef LOSCFG_FS_FAT_CACHE
    }
#endif

    if (result != ENOERR) {
        goto ERROR_HANDLE;
    }

    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return VFS_ERROR;
}


//向磁盘写入数据，主要逻辑与读磁盘类似
INT32 los_disk_write(INT32 drvID, const VOID *buf, UINT64 sector, UINT32 count)
{
#ifdef LOSCFG_FS_FAT_CACHE
    UINT32 len;
#endif
    INT32 result = VFS_ERROR;
    los_disk *disk = get_disk(drvID);
    if (disk == NULL) {
        return result;
    }

    if ((buf == NULL) || (count == 0)) { /* buff equal to NULL or count equal to 0 */
        return result;
    }

    DISK_LOCK(&disk->disk_mutex);

    if (disk->disk_status != STAT_INUSED) {
        goto ERROR_HANDLE;
    }

    if ((count > disk->sector_count) || ((disk->sector_count - count) < sector)) {
        goto ERROR_HANDLE;
    }

#ifdef LOSCFG_FS_FAT_CACHE
    if (disk->bcache != NULL) {
        if (((UINT64)(disk->bcache->sectorSize) * count) > UINT_MAX) {
            goto ERROR_HANDLE;
        }
        len = disk->bcache->sectorSize * count;
		//向块缓存写数据，后台任务再写磁盘
        result = BlockCacheWrite(disk->bcache, (const UINT8 *)buf, &len, sector);
        if (result != ENOERR) {
            PRINT_ERR("los_disk_write write err = %d, sector = %llu, len = %u\n", result, sector, len);
        }
    } else {
#endif
    if ((disk->dev != NULL) && (disk->dev->u.i_bops != NULL) && (disk->dev->u.i_bops->write != NULL)) {
        result = disk->dev->u.i_bops->write(disk->dev, (UINT8 *)buf, sector, count);
        if (result == (INT32)count) {
            result = ENOERR;
        }
    }
#ifdef LOSCFG_FS_FAT_CACHE
    }
#endif

    if (result != ENOERR) {
        goto ERROR_HANDLE;
    }

    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return VFS_ERROR;
}
//对磁盘进行自定义操作，与读写磁盘的主要逻辑类似，但不操作块缓存
INT32 los_disk_ioctl(INT32 drvID, INT32 cmd, VOID *buf)
{
    struct geometry info;
    los_disk *disk = get_disk(drvID);
    if (disk == NULL) {
        return VFS_ERROR;
    }

    DISK_LOCK(&disk->disk_mutex);

    if ((disk->dev == NULL) || (disk->disk_status != STAT_INUSED)) {
        goto ERROR_HANDLE;
    }

    if (cmd == DISK_CTRL_SYNC) {
        DISK_UNLOCK(&disk->disk_mutex); //暂时不支持
        return ENOERR;
    }

    if (buf == NULL) {
        goto ERROR_HANDLE;
    }

    (VOID)memset_s(&info, sizeof(info), 0, sizeof(info));
    if ((disk->dev->u.i_bops == NULL) || (disk->dev->u.i_bops->geometry == NULL) ||
        (disk->dev->u.i_bops->geometry(disk->dev, &info) != 0)) {
        goto ERROR_HANDLE;
    }

    if (cmd == DISK_GET_SECTOR_COUNT) {
        *(UINT64 *)buf = info.geo_nsectors;  //获取磁盘扇区数
        if (info.geo_nsectors == 0) {
            goto ERROR_HANDLE;
        }
    } else if (cmd == DISK_GET_SECTOR_SIZE) {
        *(size_t *)buf = info.geo_sectorsize;  //获取扇区尺寸
    } else if (cmd == DISK_GET_BLOCK_SIZE) { /* Get erase block size in unit of sectors (UINT32) */
        /* Block Num SDHC == 512, SD can be set to 512 or other */
        *(size_t *)buf = DISK_MAX_SECTOR_SIZE / info.geo_sectorsize; //获取磁盘块尺寸
    } else {
        goto ERROR_HANDLE;
    }

    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return VFS_ERROR;
}

//读分区数据，主要逻辑就是转换成磁盘读操作
INT32 los_part_read(INT32 pt, VOID *buf, UINT64 sector, UINT32 count)
{
    const los_part *part = get_part(pt); //获取分区描述符
    los_disk *disk = NULL;
    INT32 ret;

    if (part == NULL) {
        return VFS_ERROR;
    }

    disk = get_disk((INT32)part->disk_id);  //获取分区所在的磁盘
    if (disk == NULL) {
        return VFS_ERROR;
    }

    DISK_LOCK(&disk->disk_mutex);
    if ((part->dev == NULL) || (disk->disk_status != STAT_INUSED)) {
        goto ERROR_HANDLE; 
    }

    if (count > part->sector_count) {
        PRINT_ERR("los_part_read failed, invaild count, count = %u\n", count);
        goto ERROR_HANDLE; //需要读取数据超过了分区大小
    }

    /* Read from absolute sector. */
    if (part->type == EMMC) {
        if ((disk->sector_count - part->sector_start) > sector) {
            sector += part->sector_start; //计算读操作起始扇区号(相对于磁盘)
        } else {
            PRINT_ERR("los_part_read failed, invaild sector, sector = %llu\n", sector);
            goto ERROR_HANDLE;
        }
    }

    if ((sector >= GetFirstPartStart(part)) &&
        (((sector + count) > (part->sector_start + part->sector_count)) || (sector < part->sector_start))) {
        PRINT_ERR("los_part_read error, sector = %llu, count = %u, part->sector_start = %llu, "
                  "part->sector_count = %llu\n", sector, count, part->sector_start, part->sector_count);
        goto ERROR_HANDLE; //读扇区范围越界
    }

    ret = los_disk_read((INT32)part->disk_id, buf, sector, count);
    if (ret < 0) {
        goto ERROR_HANDLE;
    }

    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return VFS_ERROR;
}

//向分区写入数据，内部转换成向磁盘写入数据
INT32 los_part_write(INT32 pt, VOID *buf, UINT64 sector, UINT32 count)
{
    const los_part *part = get_part(pt);
    los_disk *disk = NULL;
    INT32 ret;

    if (part == NULL) {
        return VFS_ERROR;
    }

    disk = get_disk((INT32)part->disk_id);
    if (disk == NULL) {
        return VFS_ERROR;
    }

    DISK_LOCK(&disk->disk_mutex);
    if ((part->dev == NULL) || (disk->disk_status != STAT_INUSED)) {
        goto ERROR_HANDLE;
    }

    if (count > part->sector_count) {
        PRINT_ERR("los_part_write failed, invaild count, count = %u\n", count);
        goto ERROR_HANDLE;
    }

    /* Write to absolute sector. */
    if (part->type == EMMC) {
        if ((disk->sector_count - part->sector_start) > sector) {
            sector += part->sector_start;
        } else {
            PRINT_ERR("los_part_write failed, invaild sector, sector = %llu\n", sector);
            goto ERROR_HANDLE;
        }
    }

    if ((sector >= GetFirstPartStart(part)) &&
        (((sector + count) > (part->sector_start + part->sector_count)) || (sector < part->sector_start))) {
        PRINT_ERR("los_part_write, sector = %llu, count = %u, part->sector_start = %llu, "
                  "part->sector_count = %llu\n", sector, count, part->sector_start, part->sector_count);
        goto ERROR_HANDLE;
    }

    ret = los_disk_write((INT32)part->disk_id, (const VOID *)buf, sector, count);
    if (ret < 0) {
        goto ERROR_HANDLE;
    }

    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return VFS_ERROR;
}

#define GET_ERASE_BLOCK_SIZE 0x2

//对分区信息进行操作
INT32 los_part_ioctl(INT32 pt, INT32 cmd, VOID *buf)
{
    struct geometry info;
    los_part *part = get_part(pt);
    los_disk *disk = NULL;

    if (part == NULL) {
        return VFS_ERROR;
    }

    disk = get_disk((INT32)part->disk_id);
    if (disk == NULL) {
        return VFS_ERROR;
    }

    DISK_LOCK(&disk->disk_mutex);
    if ((part->dev == NULL) || (disk->disk_status != STAT_INUSED)) {
        goto ERROR_HANDLE;
    }

    if (cmd == DISK_CTRL_SYNC) {
        DISK_UNLOCK(&disk->disk_mutex);
        return ENOERR; //暂不支持
    }

    if (buf == NULL) {
        goto ERROR_HANDLE;
    }

    (VOID)memset_s(&info, sizeof(info), 0, sizeof(info));
    if ((part->dev->u.i_bops == NULL) || (part->dev->u.i_bops->geometry == NULL) ||
        (part->dev->u.i_bops->geometry(part->dev, &info) != 0)) {
        goto ERROR_HANDLE;
    }

    if (cmd == DISK_GET_SECTOR_COUNT) {
        *(UINT64 *)buf = part->sector_count;  //获取分区中扇区数
        if (*(UINT64 *)buf == 0) {
            goto ERROR_HANDLE;
        }
    } else if (cmd == DISK_GET_SECTOR_SIZE) {
        *(size_t *)buf = info.geo_sectorsize;  //获取扇区尺寸
    } else if (cmd == DISK_GET_BLOCK_SIZE) { /* Get erase block size in unit of sectors (UINT32) */
        if ((part->dev->u.i_bops->ioctl == NULL) ||
			//获取磁盘块尺寸(以扇区为单位)
            (part->dev->u.i_bops->ioctl(part->dev, GET_ERASE_BLOCK_SIZE, (UINTPTR)buf) != 0)) {
            goto ERROR_HANDLE;
        }
    } else {
        goto ERROR_HANDLE;
    }

    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return VFS_ERROR;
}

#ifdef LOSCFG_FS_FAT_CACHE
//磁盘缓存线程创建
static VOID DiskCacheThreadInit(UINT32 diskID, OsBcache *bc)
{
    bc->prereadFun = NULL;

    if (GetDiskUsbStatus(diskID) == FALSE) {
		//本磁盘没有接入USB
        if (BcacheAsyncPrereadInit(bc) == LOS_OK) { //初始化异步预读线程
            bc->prereadFun = ResumeAsyncPreread;  //设置预读函数
        }

#ifdef LOSCFG_FS_FAT_CACHE_SYNC_THREAD
        BcacheSyncThreadInit(bc, diskID);  //创建脏数据同步线程
#endif
    }

    if (OsReHookFuncAddDiskRef != NULL) {
		//注册0号和1号磁盘数据同步方法
        (VOID)OsReHookFuncAddDiskRef((StorageHookFunction)OsSdSync, (VOID *)0);
        (VOID)OsReHookFuncAddDiskRef((StorageHookFunction)OsSdSync, (VOID *)1);
    }
}

//初始化磁盘块缓存子系统
static OsBcache *DiskCacheInit(UINT32 diskID, const struct geometry *diskInfo, struct inode *blkDriver)
{
#define SECTOR_SIZE 512  //扇区尺寸为512字节

    OsBcache *bc = NULL;
    UINT32 sectorPerBlock = diskInfo->geo_sectorsize / SECTOR_SIZE;  //计算每磁盘块对应的扇区数
    if (sectorPerBlock != 0) {
		//进一步计算
        sectorPerBlock = g_uwFatSectorsPerBlock / sectorPerBlock;
        if (sectorPerBlock != 0) {
			//创建并初始化块缓存子系统
            bc = BlockCacheInit(blkDriver, diskInfo->geo_sectorsize, sectorPerBlock,
                                g_uwFatBlockNums, diskInfo->geo_nsectors / sectorPerBlock);
        }
    }

    if (bc == NULL) {
        PRINT_ERR("disk_init : disk have not init bcache cache!\n");
        return NULL;
    }

    DiskCacheThreadInit(diskID, bc);  //初始化块缓存子系统相关线程
    return bc;
}

//销毁块缓存子系统
static VOID DiskCacheDeinit(los_disk *disk)
{
    UINT32 diskID = disk->disk_id;
    if (GetDiskUsbStatus(diskID) == FALSE) {
		//磁盘没有连接usb
        if (BcacheAsyncPrereadDeinit(disk->bcache) != LOS_OK) { //注销磁盘预读线程	
            PRINT_ERR("Blib async preread deinit failed in %s, %d\n", __FUNCTION__, __LINE__);
        }
#ifdef LOSCFG_FS_FAT_CACHE_SYNC_THREAD
        BcacheSyncThreadDeinit(disk->bcache); //注销脏数据同步线程
#endif
    }

    BlockCacheDeinit(disk->bcache); //注销块缓存内存池
    disk->bcache = NULL;

    if (OsReHookFuncDelDiskRef != NULL) {
		//注销脏数据同步函数
        (VOID)OsReHookFuncDelDiskRef((StorageHookFunction)OsSdSync);
    }
}
#endif


//初始化磁盘描述符
static VOID DiskStructInit(const CHAR *diskName, INT32 diskID, const struct geometry *diskInfo,
                           struct inode *blkDriver, los_disk *disk)
{
    size_t nameLen;
    disk->disk_id = diskID; //磁盘ID
    disk->dev = blkDriver;  //磁盘设备文件
    disk->sector_start = 0; //起始扇区编号
    disk->sector_size = diskInfo->geo_sectorsize; //扇区尺寸
    disk->sector_count = diskInfo->geo_nsectors;  //扇区数

    nameLen = strlen(diskName); /* caller los_disk_init has chek name */

    if (disk->disk_name != NULL) {
        LOS_MemFree(m_aucSysMem0, disk->disk_name); //释放原名称
        disk->disk_name = NULL;
    }

	//添加新磁盘名称
    disk->disk_name = LOS_MemAlloc(m_aucSysMem0, (nameLen + 1));
    if (disk->disk_name == NULL) {
        PRINT_ERR("DiskStructInit alloc memory failed.\n");
        return;
    }

    if (strncpy_s(disk->disk_name, (nameLen + 1), diskName, nameLen) != EOK) {
        PRINT_ERR("DiskStructInit strncpy_s failed.\n");
        return;
    }
    disk->disk_name[nameLen] = '\0';
    LOS_ListInit(&disk->head);  //现在还没有磁盘分区
}

//对磁盘进行分区
static INT32 DiskDivideAndPartitionRegister(struct disk_divide_info *info, los_disk *disk)
{
    INT32 ret;

    if (info != NULL) {
		//根据已有的分区信息进行分区
        ret = DiskDivide(disk, info);
        if (ret != ENOERR) {
            PRINT_ERR("DiskDivide failed, ret = %d\n", ret);
            return ret;
        }
    } else {
    	//从磁盘中读取分区信息，然后再分区
        ret = DiskPartitionRegister(disk);
        if (ret != ENOERR) {
            PRINT_ERR("DiskPartitionRegister failed, ret = %d\n", ret);
            return ret;
        }
    }
    return ENOERR;
}


//删除磁盘
static INT32 DiskDeinit(los_disk *disk)
{
    los_part *part = NULL;
    char *diskName = NULL;
    CHAR devName[DEV_NAME_BUFF_SIZE];
    INT32 ret;

    if (LOS_ListEmpty(&disk->head) == FALSE) {
		//存在分区
        part = LOS_DL_LIST_ENTRY(disk->head.pstNext, los_part, list);
		//遍历分区
        while (&part->list != &disk->head) {
            diskName = (disk->disk_name == NULL) ? "null" : disk->disk_name;
            ret = snprintf_s(devName, sizeof(devName), sizeof(devName) - 1, "%s%c%d",
                             diskName, 'p', disk->part_count - 1);  //获得分区名称
            if (ret < 0) {
                return -ENAMETOOLONG;
            }
            DiskPartDelFromDisk(disk, part);  //从磁盘中移除分区
            (VOID)unregister_blockdriver(devName); //并注销分区的设备驱动
            DiskPartRelease(part); //删除分区

            part = LOS_DL_LIST_ENTRY(disk->head.pstNext, los_part, list); //下一个分区
        }
    }

    DISK_LOCK(&disk->disk_mutex);

#ifdef LOSCFG_FS_FAT_CACHE
    DiskCacheDeinit(disk);  //删除磁盘对应的块缓存子系统
#endif

    disk->dev = NULL;
    DISK_UNLOCK(&disk->disk_mutex);
    (VOID)unregister_blockdriver(disk->disk_name);
    if (disk->disk_name != NULL) {
        LOS_MemFree(m_aucSysMem0, disk->disk_name); //释放磁盘名称字符串
        disk->disk_name = NULL;
    }
    ret = pthread_mutex_destroy(&disk->disk_mutex); //删除磁盘互斥锁
    if (ret != 0) {
        PRINT_ERR("%s %d, mutex destroy failed, ret = %d\n", __FUNCTION__, __LINE__, ret);
        return -EFAULT;
    }

    disk->disk_status = STAT_UNUSED;  //标记磁盘描述符空闲

    return ENOERR;
}

//磁盘初始化的一部分逻辑
static VOID OsDiskInitSub(const CHAR *diskName, INT32 diskID, los_disk *disk,
                          struct geometry *diskInfo, struct inode *blkDriver)
{
    pthread_mutexattr_t attr;
#ifdef LOSCFG_FS_FAT_CACHE
	//磁盘缓存初始
    OsBcache *bc = DiskCacheInit((UINT32)diskID, diskInfo, blkDriver);
    disk->bcache = bc;
#endif

    (VOID)pthread_mutexattr_init(&attr);
    attr.type = PTHREAD_MUTEX_RECURSIVE; //可嵌套使用的互斥锁
    (VOID)pthread_mutex_init(&disk->disk_mutex, &attr);

	//磁盘描述符初始化
    DiskStructInit(diskName, diskID, diskInfo, blkDriver, disk);
}


//磁盘初始化
INT32 los_disk_init(const CHAR *diskName, const struct block_operations *bops,
                    VOID *priv, INT32 diskID, VOID *info)
{
    struct geometry diskInfo;
    struct inode *blkDriver = NULL;
    los_disk *disk = get_disk(diskID);  //获取磁盘描述符
    struct inode_search_s desc;
    INT32 ret;

    if ((diskName == NULL) || (disk == NULL) ||
        (disk->disk_status != STAT_UNREADY) || (strlen(diskName) > DISK_NAME)) {
        return VFS_ERROR;
    }

	//注册块设备驱动
    if (register_blockdriver(diskName, bops, RWE_RW_RW, priv) != 0) {
        PRINT_ERR("disk_init : register %s fail!\n", diskName);
        return VFS_ERROR;
    }

    SETUP_SEARCH(&desc, diskName, false); //查询设备文件
    ret = inode_find(&desc);
    if (ret < 0) {
        PRINT_ERR("disk_init : find %s fail!\n", diskName);
        ret = ENOENT;
        goto DISK_FIND_ERROR;
    }
    blkDriver = desc.node;  //设备文件存在

    if ((blkDriver->u.i_bops == NULL) || (blkDriver->u.i_bops->geometry == NULL) ||
        (blkDriver->u.i_bops->geometry(blkDriver, &diskInfo) != 0)) { //获取扇区大小和扇区数
        goto DISK_BLKDRIVER_ERROR;
    }
    if (diskInfo.geo_sectorsize < DISK_MAX_SECTOR_SIZE) {
        goto DISK_BLKDRIVER_ERROR;  //扇区太小
    }

    PRINTK("disk_init : register %s ok!\n", diskName);

	//磁盘描述符初始化
    OsDiskInitSub(diskName, diskID, disk, &diskInfo, blkDriver);
    inode_release(blkDriver);  //与inode_find成对使用

	//对磁盘进行分区
    if (DiskDivideAndPartitionRegister(info, disk) != ENOERR) {
        (VOID)DiskDeinit(disk);
        return VFS_ERROR;
    }

    disk->disk_status = STAT_INUSED;  //标记磁盘后续可用
    //记录磁盘类型
    if (info != NULL) {
        disk->type = EMMC;  
    } else {
        disk->type = OTHERS;
    }
    return ENOERR;

DISK_BLKDRIVER_ERROR:
    PRINT_ERR("disk_init : register %s ok but get disk info fail!\n", diskName);
    inode_release(blkDriver);
DISK_FIND_ERROR:
    (VOID)unregister_blockdriver(diskName);
    return VFS_ERROR;
}

//删除磁盘
INT32 los_disk_deinit(INT32 diskID)
{
    los_disk *disk = get_disk(diskID);
    if (disk == NULL) {
        return -EINVAL;
    }

    DISK_LOCK(&disk->disk_mutex);

    if (disk->disk_status != STAT_INUSED) {
        DISK_UNLOCK(&disk->disk_mutex);
        return -EINVAL;
    }

    disk->disk_status = STAT_UNREADY; //标记磁盘不可用
    DISK_UNLOCK(&disk->disk_mutex);

    return DiskDeinit(disk);  //删除磁盘
}

//同步磁盘数据
INT32 los_disk_sync(INT32 drvID)
{
    INT32 ret = ENOERR;
    los_disk *disk = get_disk(drvID);
    if (disk == NULL) {
        return EINVAL;
    }

    DISK_LOCK(&disk->disk_mutex);
    if (disk->disk_status != STAT_INUSED) {
        DISK_UNLOCK(&disk->disk_mutex);
        return EINVAL;
    }

#ifdef LOSCFG_FS_FAT_CACHE
        if (disk->bcache != NULL) {
			//同步块缓存数据到磁盘
            ret = BlockCacheSync(disk->bcache);
        }
#endif

    DISK_UNLOCK(&disk->disk_mutex);
    return ret;
}

//设置磁盘的块缓存
INT32 los_disk_set_bcache(INT32 drvID, UINT32 sectorPerBlock, UINT32 blockNum)
{
#ifdef LOSCFG_FS_FAT_CACHE

    INT32 ret;
    UINT32 intSave;
    OsBcache *bc = NULL;
    los_disk *disk = get_disk(drvID);
    if ((disk == NULL) || (sectorPerBlock == 0)) {
        return EINVAL;
    }

    /*
     * Because we use UINT32 flag[BCACHE_BLOCK_FLAGS] in bcache for sectors bitmap tag, so it must
     * be less than 32 * BCACHE_BLOCK_FLAGS.
     */
    if (((sectorPerBlock % UNSIGNED_INTEGER_BITS) != 0) ||
        ((sectorPerBlock >> UNINT_LOG2_SHIFT) > BCACHE_BLOCK_FLAGS)) {
        return EINVAL; //磁盘块内的扇区配置需要满足特定的要求
    }

    DISK_LOCK(&disk->disk_mutex);

    if (disk->disk_status != STAT_INUSED) {
        goto ERROR_HANDLE;
    }

    if (disk->bcache != NULL) {
		//如果磁盘原来有缓存，则先将缓存数据同步到磁盘
        ret = BlockCacheSync(disk->bcache);
        if (ret != ENOERR) {
            DISK_UNLOCK(&disk->disk_mutex);
            return ret;
        }
    }

    spin_lock_irqsave(&g_diskFatBlockSpinlock, intSave);
    DiskCacheDeinit(disk); //删除原有缓存

    g_uwFatBlockNums = blockNum;
    g_uwFatSectorsPerBlock = sectorPerBlock;

	//设置新缓存
    bc = BlockCacheInit(disk->dev, disk->sector_size, sectorPerBlock, blockNum, disk->sector_count / sectorPerBlock);
    if ((bc == NULL) && (blockNum != 0)) {
        spin_unlock_irqrestore(&g_diskFatBlockSpinlock, intSave);
        DISK_UNLOCK(&disk->disk_mutex);
        return ENOMEM;
    }

    if (bc != NULL) {
		//初始化块缓存对应的线程
        DiskCacheThreadInit((UINT32)drvID, bc);
    }

    disk->bcache = bc;  //记录磁盘块缓存
    spin_unlock_irqrestore(&g_diskFatBlockSpinlock, intSave);
    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return EINVAL;
#else
    return VFS_ERROR;
#endif
}

//在磁盘中查找分区
static los_part *OsPartFind(los_disk *disk, const struct inode *blkDriver)
{
    los_part *part = NULL;

    DISK_LOCK(&disk->disk_mutex);
    if ((disk->disk_status != STAT_INUSED) || (LOS_ListEmpty(&disk->head) == TRUE)) {
        goto EXIT; //磁盘还未分区好
    }
    part = LOS_DL_LIST_ENTRY(disk->head.pstNext, los_part, list);
    if (disk->dev == blkDriver) {
        goto EXIT; //整个磁盘就一个分区
    }

	//遍历分区
    while (&part->list != &disk->head) {
        if (part->dev == blkDriver) {
            goto EXIT;  //查找到匹配的分区
        }
        part = LOS_DL_LIST_ENTRY(part->list.pstNext, los_part, list); //下一个分区
    }
    part = NULL; //没有找到目标分区

EXIT:
    DISK_UNLOCK(&disk->disk_mutex);
    return part;
}


//在所有磁盘中查找分区
los_part *los_part_find(struct inode *blkDriver)
{
    INT32 i;
    los_disk *disk = NULL;
    los_part *part = NULL;

    if (blkDriver == NULL) {
        return NULL;
    }

	//遍历磁盘
    for (i = 0; i < SYS_MAX_DISK; i++) {
        disk = get_disk(i);
        if (disk == NULL) {
            continue;
        }
		//在磁盘中查找分区
        part = OsPartFind(disk, blkDriver);
        if (part != NULL) {
            return part;
        }
    }

    return NULL; //没有找到
}

//判断分区是否允许访问
INT32 los_part_access(const CHAR *dev, mode_t mode)
{
    los_part *part = NULL;
    struct inode *node = NULL;
    struct inode_search_s desc;
    (VOID)mode;

    SETUP_SEARCH(&desc, dev, false); //分区设备文件名
    if (inode_find(&desc) < 0) { //查找分区设备
        return VFS_ERROR;
    }
    node = desc.node; //查找成功

    part = los_part_find(node); //查找分区
    inode_release(node);
    if (part == NULL) {
        return VFS_ERROR; //查找失败，不能访问
    }

    return ENOERR; //查找成功，能访问
}


//设置磁盘分区名称
INT32 SetDiskPartName(los_part *part, const CHAR *src)
{
    size_t len;
    los_disk *disk = NULL;

    if ((part == NULL) || (src == NULL)) {
        return VFS_ERROR; //分区或名称参数不对
    }

    len = strlen(src);
    if ((len == 0) || (len >= DISK_NAME)) {
        return VFS_ERROR; //名称长度不合法
    }

    disk = get_disk((INT32)part->disk_id);
    if (disk == NULL) {
        return VFS_ERROR; //磁盘不存在
    }

    DISK_LOCK(&disk->disk_mutex);
    if (disk->disk_status != STAT_INUSED) {
        goto ERROR_HANDLE; //磁盘不可用
    }

	//申请分区名称字符串
    part->part_name = (CHAR *)zalloc(len + 1);
    if (part->part_name == NULL) {
        PRINT_ERR("%s[%d] zalloc failure\n", __FUNCTION__, __LINE__);
        goto ERROR_HANDLE;
    }

	//拷贝分区名称
    (VOID)strcpy_s(part->part_name, len + 1, src);

    DISK_UNLOCK(&disk->disk_mutex);
    return ENOERR;

ERROR_HANDLE:
    DISK_UNLOCK(&disk->disk_mutex);
    return VFS_ERROR;
}

//添加mmc分区
INT32 add_mmc_partition(struct disk_divide_info *info, size_t sectorStart, size_t sectorCount)
{
    UINT32 index, i;

    if (info == NULL) {
        return VFS_ERROR;
    }

    if ((info->part_count >= MAX_DIVIDE_PART_PER_DISK) || (sectorCount == 0)) {
        return VFS_ERROR; //分区过多，扇区数目不能为0
    }

    if ((sectorCount > info->sector_count) || ((info->sector_count - sectorCount) < sectorStart)) {
        return VFS_ERROR; //分区的扇区范围越界
    }

    index = info->part_count;
	//遍历已有分区
    for (i = 0; i < index; i++) {
		//新分区必须在已有分区之后
        if (sectorStart < (info->part[i].sector_start + info->part[i].sector_count)) {
            return VFS_ERROR;  
        }
    }

	//记录分区信息
    info->part[index].sector_start = sectorStart;
    info->part[index].sector_count = sectorCount;
    info->part[index].type = EMMC;
    info->part_count++; //当前分区数目增加

    return ENOERR;
}

//显示分区信息
VOID show_part(los_part *part)
{
    if ((part == NULL) || (part->dev == NULL)) {
        PRINT_ERR("part is NULL\n");
        return;
    }

    PRINTK("\npart info :\n");
    PRINTK("disk id          : %u\n", part->disk_id);
    PRINTK("part_id in system: %u\n", part->part_id);
    PRINTK("part no in disk  : %u\n", part->part_no_disk);
    PRINTK("part no in mbr   : %u\n", part->part_no_mbr);
    PRINTK("part filesystem  : %02X\n", part->filesystem_type);
    PRINTK("part dev name    : %s\n", part->dev->i_name);
    PRINTK("part sec start   : %llu\n", part->sector_start);
    PRINTK("part sec count   : %llu\n", part->sector_count);
}

//擦除磁盘某扇区范围
INT32 EraseDiskByID(UINT32 diskID, size_t startSector, UINT32 sectors)
{
    INT32 ret = VFS_ERROR;
#ifdef LOSCFG_DRIVERS_MMC
    los_disk *disk = get_disk((INT32)diskID);
    if (disk != NULL) {
		//数据擦除
        ret = do_mmc_erase(diskID, startSector, sectors);
    }
#endif

    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
