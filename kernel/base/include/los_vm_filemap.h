/*
 * Copyright (c) 2013-2019 Huawei Technologies Co., Ltd. All rights reserved.
 * Copyright (c) 2020-2021 Huawei Device Co., Ltd. All rights reserved.
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
 * @defgroup los_vm_filemap vm filemap definition
 * @ingroup kernel
 */

#ifndef __LOS_VM_FILEMAP_H__
#define __LOS_VM_FILEMAP_H__

#include "fs/file.h"
#include "los_vm_map.h"
#include "los_vm_page.h"
#include "los_vm_common.h"
#include "los_vm_phys.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//文件页缓存描述结构
typedef struct FilePage {
    LOS_DL_LIST             node;  //将同一个文件的缓存页连接起来
    LOS_DL_LIST             lru;   //存入active或者inactive的LRU链表，用于指导回收
    //此文件页缓存关联的mmap列表
    LOS_DL_LIST             i_mmap;       /* list of mappings */
	//i_mmap链表的长度
    UINT32                  n_maps;       /* num of mapping */
	//此文件页缓存所属的内存段
    struct VmPhysSeg        *physSeg;      /* physical memory that file page belongs to */
	//此文件页缓存实际使用的物理内存页
    struct VmPage           *vmPage;
	//此文件页缓存属于哪个文件映射
    struct page_mapping     *mapping;
	//属于文件内的第几页
    VM_OFFSET_T             pgoff;
	
    UINT32                  flags;

	//脏数据页内起始位置
    UINT16                  dirtyOff;

	//脏数据页内结束位置(不含)
    UINT16                  dirtyEnd;
} LosFilePage;

typedef struct MapInfo {
    LOS_DL_LIST             node;
    VADDR_T                 vaddr;
    LosFilePage             *page;
    LosArchMmu              *archMmu;
} LosMapInfo;

enum OsPageFlags {
    FILE_PAGE_FREE, //此内存页现在空闲
    FILE_PAGE_LOCKED, //将此内存页锁在内存中，不允许回收
    FILE_PAGE_REFERENCED, //当做为文件页缓存时，此标志主要影响其在active和inactive链表的切换，进一步影响页回收
    FILE_PAGE_DIRTY, //当内存页做为文件页缓存时，它数据是否需要再次写入文件
    FILE_PAGE_LRU, //无作用
    FILE_PAGE_ACTIVE,  //当内存页做为文件页缓存时，它是否在活动链表中
    FILE_PAGE_SHARED,  //此内存页是否共享内存中的一页
};

#define PGOFF_MAX                       2000
#define MAX_SHRINK_PAGECACHE_TRY        2
#define VM_FILEMAP_MAX_SCAN             (SYS_MEM_SIZE_DEFAULT >> PAGE_SHIFT)
#define VM_FILEMAP_MIN_SCAN             32

STATIC INLINE VOID OsSetPageLocked(LosVmPage *page)
{
    LOS_BitmapSet(&page->flags, FILE_PAGE_LOCKED);
}

STATIC INLINE VOID OsCleanPageLocked(LosVmPage *page)
{
    LOS_BitmapClr(&page->flags, FILE_PAGE_LOCKED);
}

STATIC INLINE VOID OsSetPageDirty(LosVmPage *page)
{
    LOS_BitmapSet(&page->flags, FILE_PAGE_DIRTY);
}

STATIC INLINE VOID OsCleanPageDirty(LosVmPage *page)
{
    LOS_BitmapClr(&page->flags, FILE_PAGE_DIRTY);
}

STATIC INLINE VOID OsSetPageActive(LosVmPage *page)
{
    LOS_BitmapSet(&page->flags, FILE_PAGE_ACTIVE);
}

STATIC INLINE VOID OsCleanPageActive(LosVmPage *page)
{
    LOS_BitmapClr(&page->flags, FILE_PAGE_ACTIVE);
}

STATIC INLINE VOID OsSetPageLRU(LosVmPage *page)
{
    LOS_BitmapSet(&page->flags, FILE_PAGE_LRU);
}

STATIC INLINE VOID OsSetPageFree(LosVmPage *page)
{
    LOS_BitmapSet(&page->flags, FILE_PAGE_FREE);
}

STATIC INLINE VOID OsCleanPageFree(LosVmPage *page)
{
    LOS_BitmapClr(&page->flags, FILE_PAGE_FREE);
}

STATIC INLINE VOID OsSetPageReferenced(LosVmPage *page)
{
    LOS_BitmapSet(&page->flags, FILE_PAGE_REFERENCED);
}

STATIC INLINE VOID OsCleanPageReferenced(LosVmPage *page)
{
    LOS_BitmapClr(&page->flags, FILE_PAGE_REFERENCED);
}

STATIC INLINE BOOL OsIsPageActive(LosVmPage *page)
{
    return BIT_GET(page->flags, FILE_PAGE_ACTIVE);
}

STATIC INLINE BOOL OsIsPageLocked(LosVmPage *page)
{
    return BIT_GET(page->flags, FILE_PAGE_LOCKED);
}

STATIC INLINE BOOL OsIsPageReferenced(LosVmPage *page)
{
    return BIT_GET(page->flags, FILE_PAGE_REFERENCED);
}

STATIC INLINE BOOL OsIsPageDirty(LosVmPage *page)
{
    return BIT_GET(page->flags, FILE_PAGE_DIRTY);
}

STATIC INLINE BOOL OsIsPageMapped(LosFilePage *page)
{
    return (page->n_maps != 0);
}

/* The follow three functions is used to SHM module */
STATIC INLINE VOID OsSetPageShared(LosVmPage *page)
{
    LOS_BitmapSet(&page->flags, FILE_PAGE_SHARED);
}

STATIC INLINE VOID OsCleanPageShared(LosVmPage *page)
{
    LOS_BitmapClr(&page->flags, FILE_PAGE_SHARED);
}

STATIC INLINE BOOL OsIsPageShared(LosVmPage *page)
{
    return BIT_GET(page->flags, FILE_PAGE_SHARED);
}

INT32 OsVfsFileMmap(struct file *filep, LosVmMapRegion *region);
LosFilePage *OsPageCacheAlloc(struct page_mapping *mapping, VM_OFFSET_T pgoff);
LosFilePage *OsFindGetEntry(struct page_mapping *mapping, VM_OFFSET_T pgoff);
LosMapInfo *OsGetMapInfo(LosFilePage *page, LosArchMmu *archMmu, VADDR_T vaddr);
VOID OsAddMapInfo(LosFilePage *page, LosArchMmu *archMmu, VADDR_T vaddr);
VOID OsDelMapInfo(LosVmMapRegion *region, LosVmPgFault *pgFault, BOOL cleanDirty);
VOID OsFileCacheFlush(struct page_mapping *mapping);
VOID OsFileCacheRemove(struct page_mapping *mapping);
VOID OsUnmapPageLocked(LosFilePage *page, LosMapInfo *info);
VOID OsUnmapAllLocked(LosFilePage *page);
VOID OsLruCacheAdd(LosFilePage *fpage, enum OsLruList lruType);
VOID OsLruCacheDel(LosFilePage *fpage);
LosFilePage *OsDumpDirtyPage(LosFilePage *oldPage);
VOID OsDoFlushDirtyPage(LosFilePage *fpage);
VOID OsDeletePageCacheLru(LosFilePage *page);
STATUS_T OsNamedMMap(struct file *filep, LosVmMapRegion *region);
VOID OsPageRefDecNoLock(LosFilePage *page);
VOID OsPageRefIncLocked(LosFilePage *page);
int OsTryShrinkMemory(size_t nPage);
VOID OsMarkPageDirty(LosFilePage *fpage, LosVmMapRegion *region, int off, int len);

typedef struct ProcessCB LosProcessCB;
VOID OsVmmFileRegionFree(struct file *filep, LosProcessCB *processCB);
#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

#endif /* __LOS_VM_FILEMAP_H__ */

