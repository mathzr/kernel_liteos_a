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

#include "stdio.h"
#include "string.h"
#include "time.h"
#include "sys/types.h"
#include "sys/shm.h"
#include "sys/stat.h"
#include "los_config.h"
#include "los_vm_map.h"
#include "los_vm_filemap.h"
#include "los_vm_phys.h"
#include "los_arch_mmu.h"
#include "los_vm_page.h"
#include "los_vm_lock.h"
#include "los_process.h"
#include "los_process_pri.h"
#include "user_copy.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

STATIC LosMux g_sysvShmMux;  //共享内存管理模块的互斥锁

/* private macro */
#define SYSV_SHM_LOCK()     (VOID)LOS_MuxLock(&g_sysvShmMux, LOS_WAIT_FOREVER) //获取锁
#define SYSV_SHM_UNLOCK()   (VOID)LOS_MuxUnlock(&g_sysvShmMux) //释放锁

#define SHM_MAX_PAGES 12800  //1个共享内存最多12800页
#define SHM_MAX (SHM_MAX_PAGES * PAGE_SIZE) //1个共享内存的最大尺寸
#define SHM_MIN 1 //共享内存的最小尺寸是1字节
#define SHM_MNI 192 //系统支持的最少的共享内存个数
#define SHM_SEG 128
#define SHM_ALL (SHM_MAX_PAGES)

//某共享内存的状态
#define SHM_SEG_FREE    0x2000 //空闲态
#define SHM_SEG_USED    0x4000 //已使用态
#define SHM_SEG_REMOVE  0x8000 //已删除态

#ifndef SHM_M
#define SHM_M   010000
#endif

#ifndef ACCESSPERMS
#define ACCESSPERMS (S_IRWXU | S_IRWXG | S_IRWXO)
#endif

#define SHM_GROUPE_TO_USER  3
#define SHM_OTHER_TO_USER   6

/* private structure */
struct shmSegMap {
    vaddr_t vaddr;  //虚拟内存首地址
    INT32 shmID; //共享内存ID
};

struct shmIDSource { //共享内存ID资源
    struct shmid_ds ds; //共享内存描述符
    UINT32 status; //状态
    LOS_DL_LIST node; //物理内存页链表头
};

/* private data */
STATIC struct shminfo g_shmInfo = {
    .shmmax = SHM_MAX,
    .shmmin = SHM_MIN,
    .shmmni = SHM_MNI,
    .shmseg = SHM_SEG,
    .shmall = SHM_ALL,
};

STATIC struct shmIDSource *g_shmSegs = NULL; //共享内存资源列表首地址

INT32 ShmInit(VOID)
{
    UINT32 ret;
    UINT32 i;

    ret = LOS_MuxInit(&g_sysvShmMux, NULL); //初始化互斥锁
    if (ret != LOS_OK) {
        return -1;
    }

	//申请共享内存资源列表
    g_shmSegs = LOS_MemAlloc((VOID *)OS_SYS_MEM_ADDR, sizeof(struct shmIDSource) * g_shmInfo.shmmni);
    if (g_shmSegs == NULL) {
        (VOID)LOS_MuxDestroy(&g_sysvShmMux);
        return -1;
    }
    (VOID)memset_s(g_shmSegs, (sizeof(struct shmIDSource) * g_shmInfo.shmmni),
                   0, (sizeof(struct shmIDSource) * g_shmInfo.shmmni));

    for (i = 0; i < g_shmInfo.shmmni; i++) {
        g_shmSegs[i].status = SHM_SEG_FREE; //每一个共享内存刚开始都是空闲状态
        g_shmSegs[i].ds.shm_perm.seq = i + 1; //初始序号为列表序号加1
        LOS_ListInit(&g_shmSegs[i].node); //刚开始不含任务物理内存
    }

    return 0;
}

//释放共享内存描述符
INT32 ShmDeinit(VOID)
{
    UINT32 ret;

	//释放共享内存描述符列表
    (VOID)LOS_MemFree((VOID *)OS_SYS_MEM_ADDR, g_shmSegs);
    g_shmSegs = NULL;

	//删除互斥锁
    ret = LOS_MuxDestroy(&g_sysvShmMux);
    if (ret != LOS_OK) {
        return -1;
    }

    return 0;
}

//标记内存页被共享
STATIC inline VOID ShmSetSharedFlag(struct shmIDSource *seg)
{
    LosVmPage *page = NULL;

	//遍历共享内存中的所有内存页
    LOS_DL_LIST_FOR_EACH_ENTRY(page, &seg->node, LosVmPage, node) {
        OsSetPageShared(page); //标记每页为共享页
    }
}

//清除内存页被共享标记
STATIC inline VOID ShmClearSharedFlag(struct shmIDSource *seg)
{
    LosVmPage *page = NULL;

	//遍历共享内存中的所有内存页
    LOS_DL_LIST_FOR_EACH_ENTRY(page, &seg->node, LosVmPage, node) {
        OsCleanPageShared(page); //标记每页不再共享
    }
}

//减少共享内存的页面引用计数
STATIC VOID ShmPagesRefDec(struct shmIDSource *seg)
{
    LosVmPage *page = NULL;

	//遍历共享内存中的所有内存页
    LOS_DL_LIST_FOR_EACH_ENTRY(page, &seg->node, LosVmPage, node) {
    	//减少每个页面引用计数
        LOS_AtomicDec(&page->refCounts);
    }
}


//申请共享内存资源，寻找空闲的共享内存描述符
STATIC INT32 ShmAllocSeg(key_t key, size_t size, int shmflg)
{
    INT32 i;
    INT32 segNum = -1;
    struct shmIDSource *seg = NULL;
    size_t count;

    if ((size == 0) || (size < g_shmInfo.shmmin) ||
        (size > g_shmInfo.shmmax)) { 
        return -EINVAL; //尺寸不满足要求
    }
    size = LOS_Align(size, PAGE_SIZE); //共享内存尺寸需要对齐到页的整数倍

	//遍历所有共享内存资源
    for (i = 0; i < g_shmInfo.shmmni; i++) {
        if (g_shmSegs[i].status & SHM_SEG_FREE) {
			//寻找到空闲资源
            g_shmSegs[i].status &= ~SHM_SEG_FREE; //取消空闲标记
            segNum = i;  //记录资源ID
            break;
        }
    }

    if (segNum < 0) {
        return -ENOSPC; //没有找到空闲资源，共享内存资源耗尽
    }

    seg = &g_shmSegs[segNum]; //获得共享内存资源描述符
    //申请所需的物理内存页，并记录在描述符中
    count = LOS_PhysPagesAlloc(size >> PAGE_SHIFT, &seg->node);
    if (count != (size >> PAGE_SHIFT)) {
		//没有申请足够的物理内存页，则释放之前的内存页，并返回失败
        (VOID)LOS_PhysPagesFree(&seg->node);
        seg->status = SHM_SEG_FREE;
        return -ENOMEM;
    }
    ShmSetSharedFlag(seg); //标记所有申请到的物理页为共享内存的物理页

    seg->status |= SHM_SEG_USED; //设置描述符为已使用状态
    seg->ds.shm_perm.mode = (unsigned int)shmflg & ACCESSPERMS; //记录访问权限
    seg->ds.shm_perm.key = key; //记录key
    seg->ds.shm_segsz = size; //记录size
    seg->ds.shm_perm.cuid = LOS_GetUserID(); //记录创建此共享内存的用户ID
    seg->ds.shm_perm.uid = LOS_GetUserID();  //记录创建此共享内存的用户ID
    seg->ds.shm_perm.cgid = LOS_GetGroupID();//记录创建此共享内存的用户组ID
    seg->ds.shm_perm.gid = LOS_GetGroupID(); //记录创建此共享内存的用户组ID
    seg->ds.shm_lpid = 0;
    seg->ds.shm_nattch = 0;                   //记录使用此共享内存的进程数目
    seg->ds.shm_cpid = LOS_GetCurrProcessID();//记录创建此共享内存的进程ID
    //操作相关的时间
    seg->ds.shm_atime = 0;
    seg->ds.shm_dtime = 0;
    seg->ds.shm_ctime = time(NULL); //创建时间

    return segNum;
}

//释放共享内存
STATIC INLINE VOID ShmFreeSeg(struct shmIDSource *seg)
{
    UINT32 count;

    ShmClearSharedFlag(seg); //先取消所有内存页的共享标记
    count = LOS_PhysPagesFree(&seg->node); //然后释放这些内存页
    if (count != (seg->ds.shm_segsz >> PAGE_SHIFT)) {
        VM_ERR("free physical pages failed, count = %d, size = %d", count, seg->ds.shm_segsz >> PAGE_SHIFT);
        return;
    }

    seg->status = SHM_SEG_FREE; //将描述符标记成空闲
    LOS_ListInit(&seg->node); //初始化物理内存页队列头
}

//根据key查找共享内存
STATIC INT32 ShmFindSegByKey(key_t key)
{
    INT32 i;
    struct shmIDSource *seg = NULL;

	//遍历共享内存描述符
    for (i = 0; i < g_shmInfo.shmmni; i++) {
        seg = &g_shmSegs[i];
        if ((seg->status & SHM_SEG_USED) &&
            (seg->ds.shm_perm.key == key)) {
            //描述符已使用且key值相同，则查找成功
            return i; //返回描述符编号
        }
    }

    return -1; //查找失败返回-1
}

//检查某共享内存是否满足要求
STATIC INT32 ShmSegValidCheck(INT32 segNum, size_t size, int shmFalg)
{
    struct shmIDSource *seg = &g_shmSegs[segNum];

    if (size > seg->ds.shm_segsz) {
        return -EINVAL; //超过了共享内存的大小
    }

    if ((shmFalg & (IPC_CREAT | IPC_EXCL)) ==
        (IPC_CREAT | IPC_EXCL)) {
        return -EEXIST; //重复创建
    }

    return segNum; //共享内存描述符编号
}

//根据ID获取共享内存描述符
STATIC struct shmIDSource *ShmFindSeg(int shmid)
{
    struct shmIDSource *seg = NULL;

    if ((shmid < 0) || (shmid >= g_shmInfo.shmmni)) {
        set_errno(EINVAL);
        return NULL; //ID越界
    }

    seg = &g_shmSegs[shmid]; //获取到描述符
    if ((seg->status & SHM_SEG_FREE) || (seg->status & SHM_SEG_REMOVE)) {
        set_errno(EIDRM);
        return NULL; //描述符空闲状态或者已删除状态，那么这个共享内存不能使用
    }

    return seg; //返回合法的共享内存描述符
}

//共享内存和虚拟地址的映射
STATIC VOID ShmVmmMapping(LosVmSpace *space, LOS_DL_LIST *pageList, VADDR_T vaddr, UINT32 regionFlags)
{
    LosVmPage *vmPage = NULL;
    VADDR_T va = vaddr;
    PADDR_T pa;
    STATUS_T ret;

	//遍历共享内存中的物理页链表
    LOS_DL_LIST_FOR_EACH_ENTRY(vmPage, pageList, LosVmPage, node) {
    	//取得物理页
        pa = VM_PAGE_TO_PHYS(vmPage);
        LOS_AtomicInc(&vmPage->refCounts); //增加物理页引用计数
        //将物理页和虚拟页映射起来
        ret = LOS_ArchMmuMap(&space->archMmu, va, pa, 1, regionFlags);
        if (ret != 1) {
            VM_ERR("LOS_ArchMmuMap failed, ret = %d", ret);
        }
        va += PAGE_SIZE; //下一页
    }
}


//与共享内存相关的内存区克隆
VOID OsShmFork(LosVmSpace *space, LosVmMapRegion *oldRegion, LosVmMapRegion *newRegion)
{
    struct shmIDSource *seg = NULL;

    SYSV_SHM_LOCK();
	//根据共享内存ID查询到描述符
    seg = ShmFindSeg(oldRegion->shmid);
    if (seg == NULL) {
        SYSV_SHM_UNLOCK();
        VM_ERR("shm fork failed!");
        return;
    }

	//新旧内存区共享相同的物理内存
    newRegion->shmid = oldRegion->shmid;
    newRegion->forkFlags = oldRegion->forkFlags; //共享相同的克隆标志
    //将共享内存对应的物理页映射到新内存区的虚拟地址空间
    ShmVmmMapping(space, &seg->node, newRegion->range.base, newRegion->regionFlags);
    seg->ds.shm_nattch++; //关联此共享内存的虚拟地址空间数目增加
    SYSV_SHM_UNLOCK();
}

//释放使用共享内存的某虚拟地址空间
VOID OsShmRegionFree(LosVmSpace *space, LosVmMapRegion *region)
{
    struct shmIDSource *seg = NULL;

    SYSV_SHM_LOCK();
    seg = ShmFindSeg(region->shmid); //查找共享内存描述符
    if (seg == NULL) {
        SYSV_SHM_UNLOCK();
        return;
    }

	//取消虚拟地址与共享内存的映射
    LOS_ArchMmuUnmap(&space->archMmu, region->range.base, region->range.size >> PAGE_SHIFT);
    ShmPagesRefDec(seg); //共享内存所有物理页的引用计数减少
    seg->ds.shm_nattch--; //使用此共享内存的虚拟地址空间个数减少
    if (seg->ds.shm_nattch <= 0 && (seg->status & SHM_SEG_REMOVE)) {
        ShmFreeSeg(seg); //没有人再使用此共享内存，那么删除此共享内存
    } else {
        seg->ds.shm_dtime = time(NULL); //最近一次与虚拟地址空间取消关联的时间
        //操作者进程ID
        seg->ds.shm_lpid = LOS_GetCurrProcessID();/* may not be the space's PID. */
    }
    SYSV_SHM_UNLOCK();
}

//当前内存区是共享内存吗
BOOL OsIsShmRegion(LosVmMapRegion *region)
{
    return (region->regionFlags & VM_MAP_REGION_FLAG_SHM) ? TRUE : FALSE;
}


//系统目前使用了多少共享内存(资源数)
STATIC INT32 ShmSegUsedCount(VOID)
{
    INT32 i;
    INT32 count = 0;
    struct shmIDSource *seg = NULL;

    for (i = 0; i < g_shmInfo.shmmni; i++) {
        seg = &g_shmSegs[i];
        if (seg->status & SHM_SEG_USED) {
            count++; //统计已使用的共享内存资源
        }
    }
    return count;
}


//检查共享内存权限
STATIC INT32 ShmPermCheck(struct shmIDSource *seg, mode_t mode)
{
    INT32 uid = LOS_GetUserID();
    UINT32 tmpMode = 0;
    mode_t privMode = seg->ds.shm_perm.mode;
    mode_t accMode;

    if ((uid == seg->ds.shm_perm.uid) || (uid == seg->ds.shm_perm.cuid)) {
        tmpMode |= SHM_M;
        accMode = mode & S_IRWXU;
    } else if (LOS_CheckInGroups(seg->ds.shm_perm.gid) ||
               LOS_CheckInGroups(seg->ds.shm_perm.cgid)) {
        privMode <<= SHM_GROUPE_TO_USER;
        accMode = (mode & S_IRWXG) << SHM_GROUPE_TO_USER;
    } else {
        privMode <<= SHM_OTHER_TO_USER;
        accMode = (mode & S_IRWXO) << SHM_OTHER_TO_USER;
    }

    if (privMode & SHM_R) {
        tmpMode |= SHM_R;
    }

    if (privMode & SHM_W) {
        tmpMode |= SHM_W;
    }

    if ((mode == SHM_M) && (tmpMode & SHM_M)) {
        return 0;
    }

    tmpMode &= ~SHM_M;
    if ((tmpMode & mode) == accMode) {
        return 0;
    } else {
        return EACCES;
    }
}

//创建共享内存
INT32 ShmGet(key_t key, size_t size, INT32 shmflg)
{
    INT32 ret;
    INT32 shmid;

    SYSV_SHM_LOCK();
    if ((((UINT32)shmflg & IPC_CREAT) == 0) &&
        (((UINT32)shmflg & IPC_EXCL) == 1)) {
        ret = -EINVAL; //shmflg参数不合理
        goto ERROR;
    }

    if (key == IPC_PRIVATE) {
		//由系统直接分配共享内存资源
        ret = ShmAllocSeg(key, size, shmflg);
        if (ret < 0) {
            goto ERROR;
        }
    } else {
    	//通过key查找系统中是否已有对应的共享内存资源
        ret = ShmFindSegByKey(key);
        if (ret < 0) {
			//共享内存不存在
            if (((unsigned int)shmflg & IPC_CREAT) == 0) {
                ret = -ENOENT; //用户不允许创建
                goto ERROR;
            } else {
				//创建共享内存
                ret = ShmAllocSeg(key, size, shmflg);
            }
        } else {
        	//共享内存存在
            shmid = ret;
			//检查权限是否满足需要
            ret = ShmPermCheck(ShmFindSeg(shmid), (unsigned int)shmflg & ACCESSPERMS);
            if (ret != 0) {
                ret = -ret;
                goto ERROR;
            }
			//然后检查其他标志是否满足需求
            ret = ShmSegValidCheck(shmid, size, shmflg);
        }
    }
    if (ret < 0) {
        goto ERROR;
    }

    SYSV_SHM_UNLOCK();

    return ret;
ERROR:
    set_errno(-ret);
    SYSV_SHM_UNLOCK();
    PRINT_DEBUG("%s %d, ret = %d\n", __FUNCTION__, __LINE__, ret);
    return -1;
}

INT32 ShmatParamCheck(const void *shmaddr, int shmflg)
{
    if ((shmflg & SHM_REMAP) && (shmaddr == NULL)) {
        return EINVAL; //如果是重映射，则原地址不能为空
    }

    if ((shmaddr != NULL) && !IS_PAGE_ALIGNED(shmaddr) &&
        ((shmflg & SHM_RND) == 0)) {
        return EINVAL; //指定的虚拟地址必须页对齐
    }

    return 0;
}

LosVmMapRegion *ShmatVmmAlloc(struct shmIDSource *seg, const VOID *shmaddr,
                              INT32 shmflg, UINT32 prot)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;
    LosVmMapRegion *region = NULL;
    VADDR_T vaddr;
    UINT32 regionFlags;
    INT32 ret;

    regionFlags = OsCvtProtFlagsToRegionFlags(prot, MAP_ANONYMOUS | MAP_SHARED);
    (VOID)LOS_MuxAcquire(&space->regionMux);
    if (shmaddr == NULL) {
        region = LOS_RegionAlloc(space, 0, seg->ds.shm_segsz, regionFlags, 0);
    } else {
        if (shmflg & SHM_RND) {
            vaddr = ROUNDDOWN((VADDR_T)(UINTPTR)shmaddr, SHMLBA);
        } else {
            vaddr = (VADDR_T)(UINTPTR)shmaddr;
        }
        if (!(shmflg & SHM_REMAP) && (LOS_RegionFind(space, vaddr) ||
                LOS_RegionFind(space, vaddr + seg->ds.shm_segsz - 1) ||
                LOS_RegionRangeFind(space, vaddr, seg->ds.shm_segsz - 1))) {
            ret = EINVAL;
            goto ERROR;
        }
        vaddr = (VADDR_T)LOS_MMap(vaddr, seg->ds.shm_segsz, prot,
                                      MAP_ANONYMOUS | MAP_SHARED, -1, 0);
        region = LOS_RegionFind(space, vaddr);
    }

    if (region == NULL) {
        ret = ENOMEM;
        goto ERROR;
    }
    ShmVmmMapping(space, &seg->node, region->range.base, regionFlags);
    (VOID)LOS_MuxRelease(&space->regionMux);
    return region;
ERROR:
    set_errno(ret);
    (VOID)LOS_MuxRelease(&space->regionMux);
    return NULL;
}


//映射一个已有的共享内存
//共享内存尺寸记录在共享内存描述符中
VOID *ShmAt(INT32 shmid, const VOID *shmaddr, INT32 shmflg)
{
    INT32 ret;
    UINT32 prot = PROT_READ;
    struct shmIDSource *seg = NULL;
    LosVmMapRegion *r = NULL;
    mode_t mode;

	//检查虚拟地址和映射参数
    ret = ShmatParamCheck(shmaddr, shmflg);
    if (ret != 0) {
        set_errno(ret);
        return (VOID *)-1;
    }

    if ((UINT32)shmflg & SHM_EXEC) {
        prot |= PROT_EXEC;  //共享内存用来加载代码，那么需要补充可执行权限
    } else if (((UINT32)shmflg & SHM_RDONLY) == 0) {
        prot |= PROT_WRITE; //共享内存用来写数据，那么需要补充写权限
    }

    SYSV_SHM_LOCK();
    seg = ShmFindSeg(shmid); //获取共享内存描述符
    if (seg == NULL) {
        SYSV_SHM_UNLOCK();
        return (VOID *)-1;
    }

	//设置读or读写权限
    mode = ((unsigned int)shmflg & SHM_RDONLY) ? SHM_R : (SHM_R | SHM_W);
    ret = ShmPermCheck(seg, mode); //检查共享内存的当前权限是否满足
    if (ret != 0) {
        goto ERROR;
    }

    seg->ds.shm_nattch++; //增加共享内存的引用计数
    r = ShmatVmmAlloc(seg, shmaddr, shmflg, prot); //将共享内存与虚拟地址进行映射
    if (r == NULL) {
		//映射失败
        seg->ds.shm_nattch--;
        SYSV_SHM_UNLOCK();
        return (VOID *)-1;
    }

	//映射成功，记录共享内存ID
    r->shmid = shmid;
    r->regionFlags |= VM_MAP_REGION_FLAG_SHM; //记录此内存区是共享内存
    seg->ds.shm_atime = time(NULL); //记录AT操作时间
    seg->ds.shm_lpid = LOS_GetCurrProcessID(); //记录AT操作进程
    SYSV_SHM_UNLOCK();

    return (VOID *)(UINTPTR)r->range.base; //返回映射后的虚拟地址
ERROR:
    set_errno(ret);
    SYSV_SHM_UNLOCK();
    PRINT_DEBUG("%s %d, ret = %d\n", __FUNCTION__, __LINE__, ret);
    return (VOID *)-1;
}


//对共享内存的操作
INT32 ShmCtl(INT32 shmid, INT32 cmd, struct shmid_ds *buf)
{
    struct shmIDSource *seg = NULL;
    INT32 ret = 0;
    struct shm_info shmInfo;
    struct ipc_perm shm_perm;

    cmd = ((UINT32)cmd & ~IPC_64);

    SYSV_SHM_LOCK();

    if ((cmd != IPC_INFO) && (cmd != SHM_INFO)) {
        seg = ShmFindSeg(shmid);
        if (seg == NULL) {
            SYSV_SHM_UNLOCK();
            return -1;
        }
    }

    if ((buf == NULL) && (cmd != IPC_RMID)) {
        ret = EINVAL;
        goto ERROR;
    }

    switch (cmd) {
        case IPC_STAT:
        case SHM_STAT:
            ret = ShmPermCheck(seg, SHM_R); //检查读权限
            if (ret != 0) {
                goto ERROR;
            }

			//将描述符相关信息拷贝到用户空间的buf中
            ret = LOS_ArchCopyToUser(buf, &seg->ds, sizeof(struct shmid_ds));
            if (ret != 0) {
                ret = EFAULT;
                goto ERROR;
            }
            if (cmd == SHM_STAT) {
				//将共享内存ID和perm序号组合成一个值返回
                ret = (unsigned int)((unsigned int)seg->ds.shm_perm.seq << 16) | (unsigned int)((unsigned int)shmid & 0xffff); /* 16: use the seq as the upper 16 bits */
            }
            break;
        case IPC_SET:
            ret = ShmPermCheck(seg, SHM_M);
            if (ret != 0) {
                ret = EPERM;
                goto ERROR;
            }

            ret = LOS_ArchCopyFromUser(&shm_perm, &buf->shm_perm, sizeof(struct ipc_perm));
            if (ret != 0) {
                ret = EFAULT;
                goto ERROR;
            }
            seg->ds.shm_perm.uid = shm_perm.uid;
            seg->ds.shm_perm.gid = shm_perm.gid;
            seg->ds.shm_perm.mode = (seg->ds.shm_perm.mode & ~ACCESSPERMS) |
                                    (shm_perm.mode & ACCESSPERMS);
            seg->ds.shm_ctime = time(NULL);
            break;
        case IPC_RMID:
            ret = ShmPermCheck(seg, SHM_M);
            if (ret != 0) {
                ret = EPERM;
                goto ERROR;
            }

            seg->status |= SHM_SEG_REMOVE;
            if (seg->ds.shm_nattch <= 0) {
                ShmFreeSeg(seg);
            }
            break;
        case IPC_INFO:
            ret = LOS_ArchCopyToUser(buf, &g_shmInfo, sizeof(struct shminfo));
            if (ret != 0) {
                ret = EFAULT;
                goto ERROR;
            }
            ret = g_shmInfo.shmmni;
            break;
        case SHM_INFO:
            shmInfo.shm_rss = 0;
            shmInfo.shm_swp = 0;
            shmInfo.shm_tot = 0;
            shmInfo.swap_attempts = 0;
            shmInfo.swap_successes = 0;
            shmInfo.used_ids = ShmSegUsedCount();
            ret = LOS_ArchCopyToUser(buf, &shmInfo, sizeof(struct shm_info));
            if (ret != 0) {
                ret = EFAULT;
                goto ERROR;
            }
            ret = g_shmInfo.shmmni;
            break;
        default:
            VM_ERR("the cmd(%d) is not supported!", cmd);
            ret = EINVAL;
            goto ERROR;
    }

    SYSV_SHM_UNLOCK();
    return ret;

ERROR:
    set_errno(ret);
    SYSV_SHM_UNLOCK();
    PRINT_DEBUG("%s %d, ret = %d\n", __FUNCTION__, __LINE__, ret);
    return -1;
}

INT32 ShmDt(const VOID *shmaddr)
{
    LosVmSpace *space = OsCurrProcessGet()->vmSpace;
    struct shmIDSource *seg = NULL;
    LosVmMapRegion *region = NULL;
    INT32 shmid;
    INT32 ret;

    if (IS_PAGE_ALIGNED(shmaddr) == 0) {
        ret = EINVAL;
        goto ERROR;
    }

    (VOID)LOS_MuxAcquire(&space->regionMux);
    region = LOS_RegionFind(space, (VADDR_T)(UINTPTR)shmaddr);
    if (region == NULL) {
        ret = EINVAL;
        goto ERROR_WITH_LOCK;
    }
    shmid = region->shmid;

    if (region->range.base != (VADDR_T)(UINTPTR)shmaddr) {
        ret = EINVAL;
        goto ERROR_WITH_LOCK;
    }

    /* remove it from aspace */
    LOS_RbDelNode(&space->regionRbTree, &region->rbNode);
    LOS_ArchMmuUnmap(&space->archMmu, region->range.base, region->range.size >> PAGE_SHIFT);
    /* free it */
    free(region);

    SYSV_SHM_LOCK();
    seg = ShmFindSeg(shmid);
    if (seg == NULL) {
        ret = EINVAL;
        SYSV_SHM_UNLOCK();
        goto ERROR_WITH_LOCK;
    }

    ShmPagesRefDec(seg);
    seg->ds.shm_nattch--;
    if ((seg->ds.shm_nattch <= 0) &&
        (seg->status & SHM_SEG_REMOVE)) {
        ShmFreeSeg(seg);
    } else {
        seg->ds.shm_dtime = time(NULL);
        seg->ds.shm_lpid = LOS_GetCurrProcessID();
    }
    SYSV_SHM_UNLOCK();
    (VOID)LOS_MuxRelease(&space->regionMux);
    return 0;

ERROR_WITH_LOCK:
    (VOID)LOS_MuxRelease(&space->regionMux);
ERROR:
    set_errno(ret);
    PRINT_DEBUG("%s %d, ret = %d\n", __FUNCTION__, __LINE__, ret);
    return -1;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif
