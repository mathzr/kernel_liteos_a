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

/**
 * @defgroup los_vm_map vm mapping management
 * @ingroup kernel
 */

#ifndef __LOS_VM_MAP_H__
#define __LOS_VM_MAP_H__

#include "los_typedef.h"
#include "los_arch_mmu.h"
#include "los_rbtree.h"
#include "los_vm_syscall.h"
#include "los_vm_zone.h"
#include "los_vm_common.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

typedef struct VmMapRange {
	//虚拟内存区首地址
    VADDR_T             base;           /**< vm region base addr */
	//虚拟内存区尺寸
    UINT32              size;           /**< vm region size */
} LosVmMapRange;

struct VmMapRegion;
typedef struct VmMapRegion LosVmMapRegion;
struct VmFileOps;
typedef struct VmFileOps LosVmFileOps;
struct VmSpace;
typedef struct VmSpace LosVmSpace;


//页异常信息
typedef struct VmFault {
	//页异常标志位
    UINT32          flags;              /* FAULT_FLAG_xxx flags */
	//产生异常的逻辑页编号
    unsigned long   pgoff;              /* Logical page offset based on region */
	//产生异常的虚拟地址，逻辑页首地址
    VADDR_T         vaddr;              /* Faulting virtual address */
	//产生异常的内存页对应的内核虚拟地址
    VADDR_T         *pageKVaddr;        /* KVaddr of pagefault's vm page's paddr */
} LosVmPgFault;

//内存区域映射成文件时，支持的操作
struct VmFileOps {
    void (*open)(struct VmMapRegion *region);
    void (*close)(struct VmMapRegion *region);
    int  (*fault)(struct VmMapRegion *region, LosVmPgFault *pageFault);
    void (*remove)(struct VmMapRegion *region, LosArchMmu *archMmu, VM_OFFSET_T offset);
};

struct VmMapRegion {
	//内存区域对应的红黑树节点
    LosRbNode           rbNode;         /**< region red-black tree node */
	//此内存区域所属的内存空间
    LosVmSpace          *space;
	//
    LOS_DL_LIST         node;           /**< region dl list */
	//此内存区域覆盖的虚拟地址范围
    LosVmMapRange       range;          /**< region address range */
	//此内存区域对应的页偏移，特别是映射到文件的时候
    VM_OFFSET_T         pgOff;          /**< region page offset to file */
	//此内存区域的某些属性
    UINT32              regionFlags;   /**< region flags: cow, user_wired */
	//做为共享内存时，共享内存的ID
    UINT32              shmid;          /**< shmid about shared region */
	//此内存区域的读写权限属性
    UINT8               protectFlags;   /**< vm region protect flags: PROT_READ, PROT_WRITE, */
	//此内存区域的克隆属性
    UINT8               forkFlags;      /**< vm space fork flags: COPY, ZERO, */
	//此内存区域的类型，匿名映射，文件映射，设备内存
    UINT8               regionType;     /**< vm region type: ANON, FILE, DEV */
    union {
        struct VmRegionFile {
            unsigned int fileMagic;  //文件魔数
            struct file *file; //映射的文件
            const LosVmFileOps *vmFOps;  //支持的文件操作
        } rf;  //文件映射
        struct VmRegionAnon {
            LOS_DL_LIST  node;          /**< region LosVmPage list */
        } ra;  //匿名映射
        struct VmRegionDev {
            LOS_DL_LIST  node;          /**< region LosVmPage list */
            const LosVmFileOps *vmFOps; //支持的操作
        } rd;  //设备映射
    } unTypeData;
};


//定义一个虚拟地址空间
typedef struct VmSpace {
	//将若干个虚拟地址空间链接起来
    LOS_DL_LIST         node;           /**< vm space dl list */
	//每个地址空间有若干个内存区
    LOS_DL_LIST         regions;        /**< region dl list */
	//将这些内存区也组织在红黑树中
    LosRbTree           regionRbTree;   /**< region red-black tree root */
	//红黑树保护锁
    LosMux              regionMux;      /**< region list mutex lock */
	//地址空间起始地址
    VADDR_T             base;           /**< vm space base addr */
	//地址空间尺寸
    UINT32              size;           /**< vm space size */
	//地址空间堆起始地址
    VADDR_T             heapBase;       /**< vm space heap base address */
	//地址空间堆尺寸
    VADDR_T             heapNow;        /**< vm space heap base now */
	//地址空间堆区
    LosVmMapRegion      *heap;          /**< heap region */
	//地址空间映射区首地址
    VADDR_T             mapBase;        /**< vm space mapping area base */
	//地址空间映射区尺寸
    UINT32              mapSize;        /**< vm space mapping area size */
	//与本地址空间页表相关的数据
    LosArchMmu          archMmu;        /**< vm mapping physical memory */
#ifdef LOSCFG_DRIVERS_TZDRIVER
	//代码起始地址
    VADDR_T             codeStart;      /**< user process code area start */
	//代码结束地址
    VADDR_T             codeEnd;        /**< user process code area end */
#endif
} LosVmSpace;

//内存区的类型：匿名，文件，设备
#define     VM_MAP_REGION_TYPE_NONE                 (0x0)
#define     VM_MAP_REGION_TYPE_ANON                 (0x1)
#define     VM_MAP_REGION_TYPE_FILE                 (0x2)
#define     VM_MAP_REGION_TYPE_DEV                  (0x4)
#define     VM_MAP_REGION_TYPE_MASK                 (0x7)

/* the high 8 bits(24~31) should reserved, shm will use it */
//高8位保留给共享内存模块使用
#define     VM_MAP_REGION_FLAG_CACHED               (0<<0) //缓存
#define     VM_MAP_REGION_FLAG_UNCACHED             (1<<0) //不缓存
#define     VM_MAP_REGION_FLAG_UNCACHED_DEVICE      (2<<0) /* only exists on some arches, otherwise UNCACHED */
#define     VM_MAP_REGION_FLAG_WRITE_COMBINING      (3<<0) /* only exists on some arches, otherwise UNCACHED */
#define     VM_MAP_REGION_FLAG_CACHE_MASK           (3<<0)
#define     VM_MAP_REGION_FLAG_PERM_USER            (1<<2) // ?
#define     VM_MAP_REGION_FLAG_PERM_READ            (1<<3) //读
#define     VM_MAP_REGION_FLAG_PERM_WRITE           (1<<4) //写
#define     VM_MAP_REGION_FLAG_PERM_EXECUTE         (1<<5) //执行
#define     VM_MAP_REGION_FLAG_PROT_MASK            (0xF<<2)
#define     VM_MAP_REGION_FLAG_NS                   (1<<6) /* NON-SECURE */ //非安全区
#define     VM_MAP_REGION_FLAG_SHARED               (1<<7) //共享区
#define     VM_MAP_REGION_FLAG_PRIVATE              (1<<8) //私有区
#define     VM_MAP_REGION_FLAG_FLAG_MASK            (3<<7)
#define     VM_MAP_REGION_FLAG_STACK                (1<<9) //栈
#define     VM_MAP_REGION_FLAG_HEAP                 (1<<10) //堆
#define     VM_MAP_REGION_FLAG_DATA                 (1<<11) //数据
#define     VM_MAP_REGION_FLAG_TEXT                 (1<<12) //代码
#define     VM_MAP_REGION_FLAG_BSS                  (1<<13) //未初始化数据
#define     VM_MAP_REGION_FLAG_VDSO                 (1<<14) //VDSO区
#define     VM_MAP_REGION_FLAG_MMAP                 (1<<15) //映射内存区
#define     VM_MAP_REGION_FLAG_SHM                  (1<<16) //共享内存区
#define     VM_MAP_REGION_FLAG_INVALID              (1<<17) /* indicates that flags are not specified */

//将用户传入的标志位转换成内存区可以识别的标志位
STATIC INLINE UINT32 OsCvtProtFlagsToRegionFlags(unsigned long prot, unsigned long flags)
{
    UINT32 regionFlags = 0;

    regionFlags |= VM_MAP_REGION_FLAG_PERM_USER; //由用户触发的调用？
    regionFlags |= (prot & PROT_READ) ? VM_MAP_REGION_FLAG_PERM_READ : 0; //读
    regionFlags |= (prot & PROT_WRITE) ? VM_MAP_REGION_FLAG_PERM_WRITE : 0; //写
    regionFlags |= (prot & PROT_EXEC) ? VM_MAP_REGION_FLAG_PERM_EXECUTE : 0; //执行
    regionFlags |= (flags & MAP_SHARED) ? VM_MAP_REGION_FLAG_SHARED : 0; //共享
    regionFlags |= (flags & MAP_PRIVATE) ? VM_MAP_REGION_FLAG_PRIVATE : 0; //私有

    return regionFlags;
}

//是否内核地址
STATIC INLINE BOOL LOS_IsKernelAddress(VADDR_T vaddr)
{
	//在内核空间起始地址和结束地址之间的地址都是内核地址
    return ((vaddr >= (VADDR_T)KERNEL_ASPACE_BASE) &&
            (vaddr <= ((VADDR_T)KERNEL_ASPACE_BASE + ((VADDR_T)KERNEL_ASPACE_SIZE - 1))));
}


//[vaddr, len]这个区间是否都在内核地址空间
STATIC INLINE BOOL LOS_IsKernelAddressRange(VADDR_T vaddr, size_t len)
{
    return (vaddr + len > vaddr) && LOS_IsKernelAddress(vaddr) && (LOS_IsKernelAddress(vaddr + len - 1));
}


//获取指定内存区最后一个字节的位置
STATIC INLINE VADDR_T LOS_RegionEndAddr(LosVmMapRegion *region)
{
    return (region->range.base + region->range.size - 1);
}

//计算一个区间的尺寸[start, end]
STATIC INLINE size_t LOS_RegionSize(VADDR_T start, VADDR_T end)
{
    return (end - start + 1);
}

//此内存区映射成了一个文件？
STATIC INLINE BOOL LOS_IsRegionTypeFile(LosVmMapRegion* region)
{
    return region->regionType == VM_MAP_REGION_TYPE_FILE;
}

//此内存区只允许用户读?
STATIC INLINE BOOL LOS_IsRegionPermUserReadOnly(LosVmMapRegion* region)
{
    return ((region->regionFlags & VM_MAP_REGION_FLAG_PROT_MASK) ==
            (VM_MAP_REGION_FLAG_PERM_USER | VM_MAP_REGION_FLAG_PERM_READ));
}

//此内存区是私有的
STATIC INLINE BOOL LOS_IsRegionFlagPrivateOnly(LosVmMapRegion* region)
{
    return ((region->regionFlags & VM_MAP_REGION_FLAG_FLAG_MASK) == VM_MAP_REGION_FLAG_PRIVATE);
}

//设置此内存区为映射成文件的
STATIC INLINE VOID LOS_SetRegionTypeFile(LosVmMapRegion* region)
{
    region->regionType = VM_MAP_REGION_TYPE_FILE;
}

//此内存区映射为设备所占用
STATIC INLINE BOOL LOS_IsRegionTypeDev(LosVmMapRegion* region)
{
    return region->regionType == VM_MAP_REGION_TYPE_DEV;
}

//设置此内存区为设备占用
STATIC INLINE VOID LOS_SetRegionTypeDev(LosVmMapRegion* region)
{
    region->regionType = VM_MAP_REGION_TYPE_DEV;
}

//此内存区为匿名映射内存？
STATIC INLINE BOOL LOS_IsRegionTypeAnon(LosVmMapRegion* region)
{
    return region->regionType == VM_MAP_REGION_TYPE_ANON;
}

//设置此内存区为匿名映射内存
STATIC INLINE VOID LOS_SetRegionTypeAnon(LosVmMapRegion* region)
{
    region->regionType = VM_MAP_REGION_TYPE_ANON;
}

//此地址为用户空间地址？
STATIC INLINE BOOL LOS_IsUserAddress(VADDR_T vaddr)
{
    return ((vaddr >= USER_ASPACE_BASE) &&
            (vaddr <= (USER_ASPACE_BASE + (USER_ASPACE_SIZE - 1))));
}

//[vaddr, len]为用户空间地址段？
STATIC INLINE BOOL LOS_IsUserAddressRange(VADDR_T vaddr, size_t len)
{
    return (vaddr + len > vaddr) && LOS_IsUserAddress(vaddr) && (LOS_IsUserAddress(vaddr + len - 1));
}

// vaddr是vmalloc空间地址？
STATIC INLINE BOOL LOS_IsVmallocAddress(VADDR_T vaddr)
{
    return ((vaddr >= VMALLOC_START) &&
            (vaddr <= (VMALLOC_START + (VMALLOC_SIZE - 1))));
}

//此地址空间还没有内存区？
STATIC INLINE BOOL OsIsVmRegionEmpty(LosVmSpace *vmSpace)
{
    if (vmSpace->regionRbTree.ulNodes == 0) {
        return TRUE;
    }
    return FALSE;
}

LosVmSpace *LOS_GetKVmSpace(VOID);
LOS_DL_LIST *LOS_GetVmSpaceList(VOID);
LosVmSpace *LOS_GetVmallocSpace(VOID);
VOID OsInitMappingStartUp(VOID);
VOID OsKSpaceInit(VOID);
BOOL LOS_IsRangeInSpace(const LosVmSpace *space, VADDR_T vaddr, size_t size);
STATUS_T LOS_VmSpaceReserve(LosVmSpace *space, size_t size, VADDR_T vaddr);
LosVmSpace *LOS_GetKVmSpace(VOID);
INT32 OsUserHeapFree(LosVmSpace *vmSpace, VADDR_T addr, size_t len);
VADDR_T OsAllocRange(LosVmSpace *vmSpace, size_t len);
VADDR_T OsAllocSpecificRange(LosVmSpace *vmSpace, VADDR_T vaddr, size_t len);
LosVmMapRegion *OsCreateRegion(VADDR_T vaddr, size_t len, UINT32 regionFlags, unsigned long offset);
BOOL OsInsertRegion(LosRbTree *regionRbTree, LosVmMapRegion *region);
LosVmSpace *LOS_SpaceGet(VADDR_T vaddr);
BOOL LOS_IsRegionFileValid(LosVmMapRegion *region);
LosVmMapRegion *LOS_RegionRangeFind(LosVmSpace *vmSpace, VADDR_T addr, size_t len);
LosVmMapRegion *LOS_RegionFind(LosVmSpace *vmSpace, VADDR_T addr);
PADDR_T LOS_PaddrQuery(VOID *vaddr);
LosVmMapRegion *LOS_RegionAlloc(LosVmSpace *vmSpace, VADDR_T vaddr, size_t len, UINT32 regionFlags, VM_OFFSET_T pgoff);
STATUS_T OsRegionsRemove(LosVmSpace *space, VADDR_T vaddr, size_t size);
STATUS_T OsVmRegionAdjust(LosVmSpace *space, VADDR_T vaddr, size_t size);
LosVmMapRegion *OsVmRegionDup(LosVmSpace *space, LosVmMapRegion *oldRegion, VADDR_T vaddr, size_t size);
STATUS_T OsIsRegionCanExpand(LosVmSpace *space, LosVmMapRegion *region, size_t size);
STATUS_T LOS_RegionFree(LosVmSpace *space, LosVmMapRegion *region);
STATUS_T LOS_VmSpaceFree(LosVmSpace *space);
STATUS_T LOS_VaddrToPaddrMmap(LosVmSpace *space, VADDR_T vaddr, PADDR_T paddr, size_t len, UINT32 flags);
BOOL OsUserVmSpaceInit(LosVmSpace *vmSpace, VADDR_T *virtTtb);
STATUS_T LOS_VmSpaceClone(LosVmSpace *oldVmSpace, LosVmSpace *newVmSpace);
STATUS_T LOS_UserSpaceVmAlloc(LosVmSpace *space, size_t size, VOID **ptr, UINT8 align_log2, UINT32 regionFlags);
LosMux *OsGVmSpaceMuxGet(VOID);
STATUS_T OsUnMMap(LosVmSpace *space, VADDR_T addr, size_t size);
/**
 * thread safety
 * it is used to malloc continuous virtual memory, no sure for continuous physical memory.
 */
VOID *LOS_VMalloc(size_t size);
VOID LOS_VFree(const VOID *addr);

/**
 * thread safety
 * these is used to malloc or free kernel memory.
 * when the size is large and close to multiples of pages,
 * will alloc pmm pages, otherwise alloc bestfit memory.
 */
VOID *LOS_KernelMalloc(UINT32 size);
VOID *LOS_KernelMallocAlign(UINT32 size, UINT32 boundary);
VOID *LOS_KernelRealloc(VOID *ptr, UINT32 size);
VOID LOS_KernelFree(VOID *ptr);

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

#endif /* __LOS_VM_MAP_H__ */

