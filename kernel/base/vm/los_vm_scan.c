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

#include "menuconfig.h"
#ifdef LOSCFG_FS_VFS

#include "fs/file.h"
#include "los_vm_filemap.h"

/* unmap a lru page by map record info caller need lru lock */
//取消某内存页的某个文件映射
VOID OsUnmapPageLocked(LosFilePage *page, LosMapInfo *info)
{
    if (page == NULL || info == NULL) {
        VM_ERR("UnmapPage error input null!");
        return;
    }
    page->n_maps--;  //本内存页的映射数目减少
    LOS_ListDelete(&info->node); //从内存页的映射列表中移除
    LOS_AtomicDec(&page->vmPage->refCounts); //本内存页的使用者减少
    LOS_ArchMmuUnmap(info->archMmu, info->vaddr, 1); //取消本内存页info相关的虚拟地址映射
    LOS_MemFree(m_aucSysMem0, info); //删除映射信息结构
}

//取消某内存页的所有文件映射
VOID OsUnmapAllLocked(LosFilePage *page)
{
    LosMapInfo *info = NULL;
    LosMapInfo *next = NULL;
    LOS_DL_LIST *immap = &page->i_mmap;

	//遍历某内存页的地址映射
    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(info, next, immap, LosMapInfo, node) {
        OsUnmapPageLocked(page, info); //依次取消映射
    }
}

/* add a new lru node to lru list, lruType can be file or anon */
//将文件或匿名页添加到内存段的LRU缓存中
VOID OsLruCacheAdd(LosFilePage *fpage, enum OsLruList lruType)
{
    UINT32 intSave;
    LosVmPhysSeg *physSeg = fpage->physSeg; //页所在的物理内存段
    LosVmPage *page = fpage->vmPage; //文件页所使用的实际物理页

    LOS_SpinLockSave(&physSeg->lruLock, &intSave);
    OsSetPageActive(page);  //将页设置成活跃的
    OsCleanPageReferenced(page); //取消页的引用标记
    physSeg->lruSize[lruType]++; //增加内存段中lru链表的长度计数
    LOS_ListTailInsert(&physSeg->lruList[lruType], &fpage->lru); //将文件页添加到物理内存段的lru链表中

    LOS_SpinUnlockRestore(&physSeg->lruLock, intSave);
}

/* dellete a lru node, caller need hold lru_lock */
//将文件或匿名页从内存段的lru链表中移除
VOID OsLruCacheDel(LosFilePage *fpage)
{
    LosVmPhysSeg *physSeg = fpage->physSeg; //内存段
    int type = OsIsPageActive(fpage->vmPage) ? VM_LRU_ACTIVE_FILE : VM_LRU_INACTIVE_FILE;

    physSeg->lruSize[type]--;  //链表长度减少
    LOS_ListDelete(&fpage->lru); //移除链表
}

//inactive链表是否更短
BOOL OsInactiveListIsLow(LosVmPhysSeg *physSeg)
{
    return (physSeg->lruSize[VM_LRU_ACTIVE_FILE] >
            physSeg->lruSize[VM_LRU_INACTIVE_FILE]) ? TRUE : FALSE;
}

/* move a page from inactive list to active list  head */
//从inactive移动一页到active
STATIC INLINE VOID OsMoveToActiveList(LosFilePage *fpage)
{
    LosVmPhysSeg *physSeg = fpage->physSeg;

    physSeg->lruSize[VM_LRU_ACTIVE_FILE]++;
    physSeg->lruSize[VM_LRU_INACTIVE_FILE]--;
    LOS_ListDelete(&fpage->lru);
    LOS_ListTailInsert(&physSeg->lruList[VM_LRU_ACTIVE_FILE], &fpage->lru);
}

/* move a page from active list to inactive list  head */
//从active移动一页到inactive
STATIC INLINE VOID OsMoveToInactiveList(LosFilePage *fpage)
{
    LosVmPhysSeg *physSeg = fpage->physSeg;

    physSeg->lruSize[VM_LRU_ACTIVE_FILE]--;
    physSeg->lruSize[VM_LRU_INACTIVE_FILE]++;
    LOS_ListDelete(&fpage->lru);
    LOS_ListTailInsert(&physSeg->lruList[VM_LRU_INACTIVE_FILE], &fpage->lru);
}

/* move a page to the most active pos in lru list(active head) */
//从active链表中其它位置移动到头部
STATIC INLINE VOID OsMoveToActiveHead(LosFilePage *fpage)
{
    LosVmPhysSeg *physSeg = fpage->physSeg;
    LOS_ListDelete(&fpage->lru);
    LOS_ListTailInsert(&physSeg->lruList[VM_LRU_ACTIVE_FILE], &fpage->lru);
}

/* move a page to the most active pos in lru list(inactive head) */
//从inactive链表中其它位置移动到头部
STATIC INLINE VOID OsMoveToInactiveHead(LosFilePage *fpage)
{
    LosVmPhysSeg *physSeg = fpage->physSeg;
    LOS_ListDelete(&fpage->lru);
    LOS_ListTailInsert(&physSeg->lruList[VM_LRU_INACTIVE_FILE], &fpage->lru);
}

/* page referced add: (call by page cache get)
----------inactive----------|----------active------------
[ref:0,act:0], [ref:1,act:0]|[ref:0,act:1], [ref:1,act:1]
ref:0, act:0 --> ref:1, act:0
ref:1, act:0 --> ref:0, act:1
ref:0, act:1 --> ref:1, act:1
*/
//增加页面引用计数，active状态按上述要求做适当的变更
VOID OsPageRefIncLocked(LosFilePage *fpage)
{
    BOOL isOrgActive;
    UINT32 intSave;
    LosVmPage *page = NULL;

    if (fpage == NULL) {
        return;
    }

    LOS_SpinLockSave(&fpage->physSeg->lruLock, &intSave);

    page = fpage->vmPage; //内存页
    isOrgActive = OsIsPageActive(page); //是否活跃的

	//按本函数头部注释中的状态转换表来处理
    if (OsIsPageReferenced(page) && !OsIsPageActive(page)) {
        OsCleanPageReferenced(page);
        OsSetPageActive(page);
    } else if (!OsIsPageReferenced(page)) {
        OsSetPageReferenced(page);
    }

    if (!isOrgActive && OsIsPageActive(page)) {
        /* move inactive to active */
		//内存页状态从不活跃切换成活跃，需要切换链表
        OsMoveToActiveList(fpage);
    /* no change, move head */
    } else {
    	//根据状态转换表，不存在从活跃切换回不活跃的过程
    	//所以剩下的就是状态不变的情况，此时需要将节点移动到自身链表头部
        if (OsIsPageActive(page)) {
            OsMoveToActiveHead(fpage);
        } else {
            OsMoveToInactiveHead(fpage);
        }
    }

    LOS_SpinUnlockRestore(&fpage->physSeg->lruLock, intSave);
}

/* page referced dec: (call by thrinker)
----------inactive----------|----------active------------
[ref:0,act:0], [ref:1,act:0]|[ref:0,act:1], [ref:1,act:1]
ref:1, act:1 --> ref:0, act:1
ref:0, act:1 --> ref:1, act:0
ref:1, act:0 --> ref:0, act:0
*/
//减少页面引用计数，active状态做适当调整
VOID OsPageRefDecNoLock(LosFilePage *fpage)
{
    BOOL isOrgActive;
    LosVmPage *page = NULL;

    if (fpage == NULL) {
        return;
    }

    page = fpage->vmPage;
    isOrgActive = OsIsPageActive(page);

    if (!OsIsPageReferenced(page) && OsIsPageActive(page)) {
        OsCleanPageActive(page);
        OsSetPageReferenced(page);
    } else if (OsIsPageReferenced(page)) {
        OsCleanPageReferenced(page);
    }

    if (isOrgActive && !OsIsPageActive(page)) {
        OsMoveToInactiveList(fpage);
    }
}

VOID OsShrinkActiveList(LosVmPhysSeg *physSeg, int nScan)
{
    LosFilePage *fpage = NULL;
    LosFilePage *fnext = NULL;
    LOS_DL_LIST *activeFile = &physSeg->lruList[VM_LRU_ACTIVE_FILE];

    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(fpage, fnext, activeFile, LosFilePage, lru) {
        if (LOS_SpinTrylock(&fpage->mapping->list_lock) != LOS_OK) {
            continue;
        }

        /* happend when caller hold cache lock and try reclaim this page */
        if (OsIsPageLocked(fpage->vmPage)) {
			//被锁住的内存页不能释放
            LOS_SpinUnlock(&fpage->mapping->list_lock);
            continue;
        }

        if (OsIsPageMapped(fpage) && (fpage->flags & VM_MAP_REGION_FLAG_PERM_EXECUTE)) {
			//已经映射且可执行态的内存页不能释放
            LOS_SpinUnlock(&fpage->mapping->list_lock);
            continue;
        }

        OsPageRefDecNoLock(fpage); //其他内存页可以做释放动作

        LOS_SpinUnlock(&fpage->mapping->list_lock);

        if (--nScan <= 0) {  //最多释放nScan内存页
            break;
        }
    }
}


//清理和回收不活动的内存页，最多回收nScan个内存页
//同时把脏的内存页放入待回写队列上
int OsShrinkInactiveList(LosVmPhysSeg *physSeg, int nScan, LOS_DL_LIST *list)
{
    UINT32 nrReclaimed = 0;
    LosVmPage *page = NULL;
    SPIN_LOCK_S *flock = NULL;
    LosFilePage *fpage = NULL;
    LosFilePage *fnext = NULL;
    LosFilePage *ftemp = NULL;
    LOS_DL_LIST *inactive_file = &physSeg->lruList[VM_LRU_INACTIVE_FILE];

	//遍历不活动内存页列表
    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(fpage, fnext, inactive_file, LosFilePage, lru) {
        flock = &fpage->mapping->list_lock;

        if (LOS_SpinTrylock(flock) != LOS_OK) {
            continue;
        }

        page = fpage->vmPage; //内存页
        if (OsIsPageLocked(page)) {
            LOS_SpinUnlock(flock); //被锁住的内存页无法操作，直接返回
            continue;
        }

		//文件映射的脏页，或者文件映射的可执行页，不参与回收
        if (OsIsPageMapped(fpage) && (OsIsPageDirty(page) || (fpage->flags & VM_MAP_REGION_FLAG_PERM_EXECUTE))) {
            LOS_SpinUnlock(flock);
            continue;
        }

        if (OsIsPageDirty(page)) { //其它脏页
            ftemp = OsDumpDirtyPage(fpage); //回收filepage, 但不能回收vmpage
            if (ftemp != NULL) {
                LOS_ListTailInsert(list, &ftemp->node);  //脏页面的数据还是要待回写，所以还是暂存到队列中
            }
        }

        OsDeletePageCacheLru(fpage); //删除内存页
        LOS_SpinUnlock(flock);
        nrReclaimed++;  //增加回收计数

        if (--nScan <= 0) { //最多回收nScan个内存页
            break;
        }
    }

    return nrReclaimed;
}


//inactive状态内存页更少
bool InactiveListIsLow(LosVmPhysSeg *physSeg)
{
    return (physSeg->lruSize[VM_LRU_ACTIVE_FILE] > physSeg->lruSize[VM_LRU_INACTIVE_FILE]) ? TRUE : FALSE;
}


//尝试释放一些内存页
#ifdef LOSCFG_FS_VFS
int OsTryShrinkMemory(size_t nPage)
{
    UINT32 intSave;
    size_t totalPages;
    size_t nReclaimed = 0;
    LosVmPhysSeg *physSeg = NULL;
    UINT32 index;
    LOS_DL_LIST_HEAD(dirtyList);  //脏页链表
    LosFilePage *fpage = NULL;
    LosFilePage *fnext = NULL;

    if (nPage <= 0) {
        nPage = VM_FILEMAP_MIN_SCAN;  //最少释放的内存页数目
    }

    if (nPage > VM_FILEMAP_MAX_SCAN) {
        nPage = VM_FILEMAP_MAX_SCAN;  //最多释放的内存页数目
    }

	//遍历每一个物理内存段
    for (index = 0; index < g_vmPhysSegNum; index++) {
        physSeg = &g_vmPhysSeg[index]; //物理内存段
        LOS_SpinLockSave(&physSeg->lruLock, &intSave);
		//活动或者不活动的内存页总数
        totalPages = physSeg->lruSize[VM_LRU_ACTIVE_FILE] + physSeg->lruSize[VM_LRU_INACTIVE_FILE];
        if (totalPages < VM_FILEMAP_MIN_SCAN) {
            LOS_SpinUnlockRestore(&physSeg->lruLock, intSave);
            continue; //总页数还达不到一次释放要求
        }

        if (InactiveListIsLow(physSeg)) {
			//不活动页偏少，先尝试切换部分活动页到不活动页
            OsShrinkActiveList(physSeg, (nPage < VM_FILEMAP_MIN_SCAN) ? VM_FILEMAP_MIN_SCAN : nPage);
        }

		//回收不活动页
        nReclaimed += OsShrinkInactiveList(physSeg, nPage, &dirtyList);
        LOS_SpinUnlockRestore(&physSeg->lruLock, intSave);

        if (nReclaimed >= nPage) {
            break; //回收的内存页数达到要求
        }
    }

    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(fpage, fnext, &dirtyList, LosFilePage, node) {
        OsDoFlushDirtyPage(fpage);  //将脏页写回永久存储
    }

    return nReclaimed;
}
#else
int OsTryShrinkMemory(size_t nPage)
{
    return 0;
}
#endif

#endif
