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
 * @defgroup los_vm_filemap vm filemap definition
 * @ingroup kernel
 */

//虚拟内存文件映射相关的处理逻辑

#include "los_vm_filemap.h"
#include "los_vm_page.h"
#include "los_vm_phys.h"
#include "los_vm_common.h"
#include "los_vm_fault.h"
#include "los_process_pri.h"
#include "inode/inode.h"
#include "los_vm_lock.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//在页缓存链表中插入页缓存，按页偏移pgoff升序排列
STATIC VOID OsPageCacheAdd(LosFilePage *page, struct page_mapping *mapping, VM_OFFSET_T pgoff)
{
    LosFilePage *fpage = NULL;

    LOS_DL_LIST_FOR_EACH_ENTRY(fpage, &mapping->page_list, LosFilePage, node) {
        if (fpage->pgoff > pgoff) {
			//有序插入
            LOS_ListTailInsert(&fpage->node, &page->node);
            goto done_add;
        }
    }

    LOS_ListTailInsert(&mapping->page_list, &page->node); //或者插入尾部

    OsSetPageLRU(page->vmPage);  

done_add:
    mapping->nrpages++;  //页缓存计数增加
}


//页缓存既加入文件的页缓存队列，也加入活动的LRU队列
VOID OsAddToPageacheLru(LosFilePage *page, struct page_mapping *mapping, VM_OFFSET_T pgoff)
{
    OsPageCacheAdd(page, mapping, pgoff);
    OsLruCacheAdd(page, VM_LRU_ACTIVE_FILE);
}


//删除页缓存
VOID OsPageCacheDel(LosFilePage *fpage)
{
    /* delete from file cache list */
	//先从文件的缓存队列中移除
    LOS_ListDelete(&fpage->node);
    fpage->mapping->nrpages--; //队列中元素减少

    /* unmap and remove map info */
    if (OsIsPageMapped(fpage)) {
		//然后再取消本页的所有虚拟地址映射
        OsUnmapAllLocked(fpage);
    }

	//释放物理内存页
    LOS_PhysPageFree(fpage->vmPage);

	//释放页缓存描述符
    LOS_MemFree(m_aucSysMem0, fpage);
}


//添加本页缓存的虚拟地址映射信息
VOID OsAddMapInfo(LosFilePage *page, LosArchMmu *archMmu, VADDR_T vaddr)
{
    LosMapInfo *info = NULL;

	//创建映射信息
    info = (LosMapInfo *)LOS_MemAlloc(m_aucSysMem0, sizeof(LosMapInfo));
    if (info == NULL) {
        VM_ERR("OsAddMapInfo alloc memory failed!");
        return;
    }
	//记录页缓存和虚拟地址的映射关系
    info->page = page;
    info->archMmu = archMmu;
    info->vaddr = vaddr;

    LOS_ListAdd(&page->i_mmap, &info->node); //加入映射表
    page->n_maps++; //映射信息数目增加
}


//获取页缓存的某虚拟地址映射信息结构
LosMapInfo *OsGetMapInfo(LosFilePage *page, LosArchMmu *archMmu, VADDR_T vaddr)
{
    LosMapInfo *info = NULL;
    LOS_DL_LIST *immap = &page->i_mmap;

	//遍历页缓存映射信息列表，根据虚拟地址查询映射信息
    LOS_DL_LIST_FOR_EACH_ENTRY(info, immap, LosMapInfo, node) {
        if ((info->archMmu == archMmu) && (info->vaddr == vaddr) && (info->page == page)) {
            return info;
        }
    }

    return NULL;
}


//从LRU列表中移除页缓存，并删除页缓存
VOID OsDeletePageCacheLru(LosFilePage *page)
{
    /* delete form lru list */
    OsLruCacheDel(page);
    /* delete from cache lits and free pmm if need */
    OsPageCacheDel(page);
}


//获取文件内存页缓存，并填入文件的内容
STATIC LosFilePage *OsPagecacheGetPageAndFill(struct file *filp, VM_OFFSET_T pgOff, size_t *readSize, VADDR_T *kvaddr)
{
    LosFilePage *page = NULL;
    struct page_mapping *mapping = filp->f_mapping;

    page = OsFindGetEntry(mapping, pgOff); //查询页缓存是否已存在
    if (page != NULL) {
		//页缓存已存在
        OsSetPageLocked(page->vmPage); //先锁住物理页
        OsPageRefIncLocked(page); //增加页缓存的引用计数
        *kvaddr = (VADDR_T)(UINTPTR)OsVmPageToVaddr(page->vmPage);  //获取物理页的内核虚拟地址
        *readSize = PAGE_SIZE;  //读取的字节数为一页
    } else {
		//页缓存不存在，申请新的页缓存
        page = OsPageCacheAlloc(mapping, pgOff);
        if (page == NULL) {
            VM_ERR("Failed to alloc a page frame");
            return page;
        }
        OsSetPageLocked(page->vmPage); //先锁住物理页
        *kvaddr = (VADDR_T)(UINTPTR)OsVmPageToVaddr(page->vmPage); //获取物理页的内核虚拟地址

        file_seek(filp, pgOff << PAGE_SHIFT, SEEK_SET); //调整文件指针到合理的位置
        /* "ReadPage" func exists definitely in this procedure */
		//将文件中的数据读入此页缓存
        *readSize = filp->f_inode->u.i_mops->readpage(filp, (char *)(UINTPTR)*kvaddr, PAGE_SIZE);
        if (*readSize == 0) {
            VM_ERR("read 0 bytes");
            OsCleanPageLocked(page->vmPage);
        }
		//将页缓存放入文件的页缓存链表
        OsAddToPageacheLru(page, mapping, pgOff);
    }

    return page;  //返回页缓存描述符
}


//从文件中读入数据
ssize_t OsMappingRead(struct file *filp, char *buf, size_t size)
{
    INT32 ret;
    vaddr_t kvaddr = 0;
    UINT32 intSave;
    struct stat bufStat;
    size_t readSize = 0;
    size_t readTotal = 0;
    size_t readLeft = size;
    LosFilePage *page = NULL;
    VM_OFFSET_T pos = file_seek(filp, 0, SEEK_CUR);  //获取本次读取的开始位置
    VM_OFFSET_T pgOff = pos >> PAGE_SHIFT; //判断其所在的文件内部内存页偏移
    INT32 offInPage = pos % PAGE_SIZE; //页内字节偏移
    struct page_mapping *mapping = filp->f_mapping;  //此文件的内存页列表
    //需要读取的内存页数目
    INT32 nPages = (ROUNDUP(pos + size, PAGE_SIZE) - ROUNDDOWN(pos, PAGE_SIZE)) >> PAGE_SHIFT;

    ret = stat(filp->f_path, &bufStat); //获取文件的属性
    if (ret != OK) {
        VM_ERR("Get file size failed. (filepath=%s)", filp->f_path);
        return 0;
    }

    if (pos >= bufStat.st_size) { //当前读取位置超越了文件大小，读不到数据
        PRINT_INFO("%s filp->f_pos >= bufStat.st_size (pos=%ld, fileSize=%ld)\n", filp->f_path, pos, bufStat.st_size);
        return 0;
    }

    LOS_SpinLockSave(&mapping->list_lock, &intSave);

	//逐页读取，直到读完所有页，或者缓存满
    for (INT32 i = 0; (i < nPages) && readLeft; i++, pgOff++) {
		//获取一页内存，并将数据读入这页内存
        page = OsPagecacheGetPageAndFill(filp, pgOff, &readSize, &kvaddr);
        if ((page == NULL) || (readSize == 0)) {
            break; //读失败，则返回
        }
        if (readSize < PAGE_SIZE) {
            readLeft = readSize;  //最后一次可能无法读完一页
        }

		//最开始读到的数据不需要从页起始位置拷贝，而应该从页内offInPage位置拷贝
        readSize = MIN2((PAGE_SIZE - offInPage), readLeft);

		//拷贝读到的数据到输出缓冲区
        (VOID)memcpy_s((VOID *)buf, readLeft, (char *)kvaddr + offInPage, readSize);
        buf += readSize; //下一次拷贝的目标位置
        readLeft -= readSize; //输出缓冲区的剩余尺寸
        readTotal += readSize; //当前输出到缓冲区的字节总数目

        offInPage = 0; //从第2页开始，都是从每页的最开始读取

        OsCleanPageLocked(page->vmPage); //OsPagecacheGetPageAndFill中锁住了物理页，这里解锁
    }

    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);
    file_seek(filp, pos + readTotal, SEEK_SET);  //修正文件指针的位置，下一次才能正确读取

    return readTotal; //返回成功读入的字节数目
}


//向文件中写入数据
ssize_t OsMappingWrite(struct file *filp, const char *buf, size_t size)
{
    VADDR_T kvaddr;
    UINT32 intSave;
    INT32 writeSize = 0;
    size_t writeLeft = size;
    VM_OFFSET_T pos = file_seek(filp, 0, SEEK_CUR); //获取当前文件指针位置
    VM_OFFSET_T pgOff = pos >> PAGE_SHIFT; //当前文件缓存页编号
    INT32 offInPage = pos % PAGE_SIZE; //在缓存页中文件指针对应的位置
    LosFilePage *page = NULL;
    struct page_mapping *mapping = filp->f_mapping;
	//需要写入的缓存页数目
    INT32 nPages = (ROUNDUP(pos + size, PAGE_SIZE) - ROUNDDOWN(pos, PAGE_SIZE)) >> PAGE_SHIFT;

    LOS_SpinLockSave(&mapping->list_lock, &intSave);

	//向文件写入数据，即先写入到缓存页中，逐页写入
    for (INT32 i = 0; i < nPages; i++, pgOff++) {
        page = OsFindGetEntry(mapping, pgOff); //寻找缓存页
        if (page) {
			//缓存页存在，获取缓存页的内核虚拟地址
            kvaddr = (VADDR_T)(UINTPTR)OsVmPageToVaddr(page->vmPage);
            OsSetPageLocked(page->vmPage); //锁住缓存页，我们要操作了
            OsPageRefIncLocked(page); //增加缓存页引用计数
        } else {
			//缓存页不存在，则创建缓存页
            page = OsPageCacheAlloc(mapping, pgOff);
            if (page == NULL) {
                VM_ERR("Failed to alloc a page frame");
                break;
            }
			//获取缓存页内核虚拟地址
            kvaddr = (VADDR_T)(UINTPTR)OsVmPageToVaddr(page->vmPage);
			//将缓存页加入页缓存队列和LRU队列
            OsAddToPageacheLru(page, mapping, pgOff);
            OsSetPageLocked(page->vmPage); //锁住缓存页
        }

		//计算本页需要写入的字节数目
		//第1页，从offInPage开始写，最多写到页边界，或者最多写到文件尾部
		//第2页开始，从0位置开始写
        writeSize = MIN2((PAGE_SIZE - offInPage), writeLeft);

		//将数据写入缓存页
        (VOID)memcpy_s((char *)(UINTPTR)kvaddr + offInPage, writeLeft, buf, writeSize);
        buf += writeSize;  //下次需要写入的数据
        writeLeft -= writeSize; //剩余需要写入的数据

        OsMarkPageDirty(page, NULL, offInPage, writeSize); //标记当前缓存页为脏页，后续好同步到磁盘

        offInPage = 0; //从第2页开始，从每页开始位置写入

        OsCleanPageLocked(page->vmPage);  //本页操作结束，解锁
    }

    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);

    file_seek(filp, pos + size - writeLeft, SEEK_SET); //调整文件指针，方便下一次写入操作
    return (size - writeLeft); //返回本次成功写入的字节数目
}


//取消文件页缓存的虚拟地址映射
STATIC VOID OsPageCacheUnmap(LosFilePage *fpage, LosArchMmu *archMmu, VADDR_T vaddr)
{
    UINT32 intSave;
    LosMapInfo *info = NULL;

    LOS_SpinLockSave(&fpage->physSeg->lruLock, &intSave);
    info = OsGetMapInfo(fpage, archMmu, vaddr); //查找虚拟地址的映射信息是否存在
    if (info == NULL) {
        VM_ERR("OsPageCacheUnmap get map info fail!");
    } else {
    	//存在则取消虚拟地址映射
        OsUnmapPageLocked(fpage, info);
    }
    if (!(OsIsPageMapped(fpage) && ((fpage->flags & VM_MAP_REGION_FLAG_PERM_EXECUTE) ||
        OsIsPageDirty(fpage->vmPage)))) {
        OsPageRefDecNoLock(fpage);  //除了脏页或者代码页，应该想办法允许这个内存页回收
    }

    LOS_SpinUnlockRestore(&fpage->physSeg->lruLock, intSave);
}


//删除文件中的某页
VOID OsVmmFileRemove(LosVmMapRegion *region, LosArchMmu *archMmu, VM_OFFSET_T pgoff)
{
    UINT32 intSave;
    vaddr_t vaddr;
    paddr_t paddr = 0;
    struct file *file = NULL;
    struct page_mapping *mapping = NULL;
    LosFilePage *fpage = NULL;
    LosFilePage *tmpPage = NULL;
    LosVmPage *mapPage = NULL;

    if (!LOS_IsRegionFileValid(region) || (region->unTypeData.rf.file->f_mapping == NULL)) {
        return;
    }
    file = region->unTypeData.rf.file;  //获得内存区关联的文件
    mapping = file->f_mapping;
	//根据页偏移获取到虚拟地址
    vaddr = region->range.base + ((UINT32)(pgoff - region->pgOff) << PAGE_SHIFT);

	//查询对应的物理地址
    status_t status = LOS_ArchMmuQuery(archMmu, vaddr, &paddr, NULL);
    if (status != LOS_OK) {
        return;
    }

    mapPage = LOS_VmPageGet(paddr);  //获取物理页描述符

    /* is page is in cache list */
    LOS_SpinLockSave(&mapping->list_lock, &intSave);
    fpage = OsFindGetEntry(mapping, pgoff);  //查找对应的页缓存
    /* no cache or have cache but not map(cow), free it direct */
    if ((fpage == NULL) || (fpage->vmPage != mapPage)) {
		//没有页缓存，直接释放物理页，并取消映射
        LOS_PhysPageFree(mapPage);
        LOS_ArchMmuUnmap(archMmu, vaddr, 1);
    /* this is a page cache map! */
    } else {
    	//存在页缓存，则取消页缓存映射
        OsPageCacheUnmap(fpage, archMmu, vaddr);
        if (OsIsPageDirty(fpage->vmPage)) {
			//如果当前物理页是脏页，则还要复制一下脏页，只是复制页缓存描述符，保证物理页不被释放
            tmpPage = OsDumpDirtyPage(fpage);
        }
    }
    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);

    if (tmpPage) {
        OsDoFlushDirtyPage(tmpPage); //将脏页数据写入磁盘
    }
    return;
}


//标记某页缓存为脏页
VOID OsMarkPageDirty(LosFilePage *fpage, LosVmMapRegion *region, INT32 off, INT32 len)
{
    if (region != NULL) {
		//标记物理页现在含有脏数据
        OsSetPageDirty(fpage->vmPage);
        fpage->dirtyOff = off; //记录脏数据的起始位置
        fpage->dirtyEnd = len; //记录脏数据的结束位置
    } else {
        OsSetPageDirty(fpage->vmPage); //标记物理页现在含有脏数据
        if ((off + len) > fpage->dirtyEnd) {
            fpage->dirtyEnd = off + len; //刷新脏数据的结束位置
        }

        if (off < fpage->dirtyOff) {
            fpage->dirtyOff = off; //刷新脏数据的起始位置
        }
    }
}


//获取脏页中脏数据的尺寸
STATIC UINT32 GetDirtySize(LosFilePage *fpage, struct file *file)
{
    UINT32 fileSize;
    UINT32 dirtyBegin;
    UINT32 dirtyEnd;
    struct stat buf_stat;

	//读取文件属性
    if (stat(file->f_path, &buf_stat) != OK) {
        VM_ERR("FlushDirtyPage get file size failed. (filepath=%s)", file->f_path);
        return 0;
    }

	//获得文件尺寸
    fileSize = buf_stat.st_size;
	//脏页起始位置
    dirtyBegin = ((UINT32)fpage->pgoff << PAGE_SHIFT);
	//脏页结束位置
    dirtyEnd = dirtyBegin + PAGE_SIZE;

    if (dirtyBegin >= fileSize) {
        return 0; //脏页起始位置比文件尺寸还大，说明脏数据不是本文件的
    }

    if (dirtyEnd >= fileSize) {
		//脏页结束位置比当前文件尺寸还大，说明本页前面一部分才是文件数据
        return fileSize - dirtyBegin; //返回本页内实际数据的尺寸
    }

    return PAGE_SIZE; //其它情况，整页都是脏数据，需要回写磁盘
}


//将页缓存中的脏数据写入磁盘
STATIC INT32 OsFlushDirtyPage(LosFilePage *fpage)
{
    UINT32 ret;
    size_t len;
    char *buff = NULL;
    VM_OFFSET_T oldPos;
    struct file *file = fpage->mapping->host; //需要写入的文件
    if ((file == NULL) || (file->f_inode == NULL)) {
        VM_ERR("page cache file error");
        return LOS_NOK; //文件不存在
    }

    oldPos = file_seek(file, 0, SEEK_CUR); //备份原文件指针位置
    buff = (char *)OsVmPageToVaddr(fpage->vmPage); //此脏页起始地址
    //将文件指针调整到脏数据起始位置
    file_seek(file, (((UINT32)fpage->pgoff << PAGE_SHIFT) + fpage->dirtyOff), SEEK_SET);
    len = fpage->dirtyEnd - fpage->dirtyOff; //计算脏数据尺寸
    len = (len == 0) ? GetDirtySize(fpage, file) : len; //整页都是脏数据或者部分是脏数据
    if (len == 0) {
		//无脏数据可写
        OsCleanPageDirty(fpage->vmPage); //清除脏页标志
        (VOID)file_seek(file, oldPos, SEEK_SET); //恢复文件指针
        return LOS_OK;
    }

	//将脏页数据写入磁盘
    if (file->f_inode && file->f_inode->u.i_mops->writepage) {		
        ret = file->f_inode->u.i_mops->writepage(file, (buff + fpage->dirtyOff), len);
    } else {
        ret = file_write(file, (VOID *)buff, len);
    }
    if (ret <= 0) {
        VM_ERR("WritePage error ret %d", ret);
    }
    ret = (ret <= 0) ? LOS_NOK : LOS_OK;
    OsCleanPageDirty(fpage->vmPage); //清除脏页标记
    (VOID)file_seek(file, oldPos, SEEK_SET); //恢复原文件指针位置

    return ret;
}


//复制脏页描述符
LosFilePage *OsDumpDirtyPage(LosFilePage *oldFPage)
{
    LosFilePage *newFPage = NULL;

	//申请新页缓存描述符
    newFPage = (LosFilePage *)LOS_MemAlloc(m_aucSysMem0, sizeof(LosFilePage));
    if (newFPage == NULL) {
        VM_ERR("Failed to allocate for temp page!");
        return NULL;
    }

    OsCleanPageDirty(oldFPage->vmPage); //清除原脏页标记
    LOS_AtomicInc(&oldFPage->vmPage->refCounts); //增加原脏页引用计数
    /* no map page cache */
    if (LOS_AtomicRead(&oldFPage->vmPage->refCounts) == 1) {
        LOS_AtomicInc(&oldFPage->vmPage->refCounts); //TBD
    }
	//拷贝脏页描述符
    (VOID)memcpy_s(newFPage, sizeof(LosFilePage), oldFPage, sizeof(LosFilePage));

    return newFPage;
}

//将脏页数据写入磁盘，并释放脏页
VOID OsDoFlushDirtyPage(LosFilePage *fpage)
{
    if (fpage == NULL) {
        return;
    }
    (VOID)OsFlushDirtyPage(fpage);
    LOS_PhysPageFree(fpage->vmPage);
    LOS_MemFree(m_aucSysMem0, fpage);
}


//释放页缓存
STATIC VOID OsReleaseFpage(struct page_mapping *mapping, LosFilePage *fpage)
{
    UINT32 intSave;
    UINT32 lruSave;
    SPIN_LOCK_S *lruLock = &fpage->physSeg->lruLock;
    LOS_SpinLockSave(&mapping->list_lock, &intSave);
    LOS_SpinLockSave(lruLock, &lruSave);
    OsCleanPageLocked(fpage->vmPage); //页缓存解锁
    OsDeletePageCacheLru(fpage); //从LRU队列中移除页缓存，并释放页缓存
    LOS_SpinUnlockRestore(lruLock, lruSave);
    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);
}

//删除映射信息
VOID OsDelMapInfo(LosVmMapRegion *region, LosVmPgFault *vmf, BOOL cleanDirty)
{
    UINT32 intSave;
    LosMapInfo *info = NULL;
    LosFilePage *fpage = NULL;

    if (!LOS_IsRegionFileValid(region) || (region->unTypeData.rf.file->f_mapping == NULL) || (vmf == NULL)) {
        return;
    }

    LOS_SpinLockSave(&region->unTypeData.rf.file->f_mapping->list_lock, &intSave);
	//查找页缓存
    fpage = OsFindGetEntry(region->unTypeData.rf.file->f_mapping, vmf->pgoff);
    if (fpage == NULL) {
		//页缓存不存在
        LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);
        return;
    }

    if (cleanDirty) {
		//需要清除脏页标记的情况
        OsCleanPageDirty(fpage->vmPage);
    }
	//查询本页的虚拟地址映射
    info = OsGetMapInfo(fpage, &region->space->archMmu, (vaddr_t)vmf->vaddr);
    if (info != NULL) {
		//删除映射
        fpage->n_maps--;
        LOS_ListDelete(&info->node);
        LOS_AtomicDec(&fpage->vmPage->refCounts);
        LOS_MemFree(m_aucSysMem0, info);
    }
    LOS_SpinUnlockRestore(&region->unTypeData.rf.file->f_mapping->list_lock, intSave);
}


//文件页异常处理
INT32 OsVmmFileFault(LosVmMapRegion *region, LosVmPgFault *vmf)
{
    INT32 ret;
    VM_OFFSET_T oldPos;
    VOID *kvaddr = NULL;

    UINT32 intSave;
    bool newCache = false;
    struct file *file = NULL;
    struct page_mapping *mapping = NULL;
    LosFilePage *fpage = NULL;

    if (!LOS_IsRegionFileValid(region) || (region->unTypeData.rf.file->f_mapping == NULL) || (vmf == NULL)) {
        VM_ERR("Input param is NULL");
        return LOS_NOK;
    }
    file = region->unTypeData.rf.file; //本内存区域映射的文件
    mapping = file->f_mapping; //文件对应的页映射列表

    /* get or create a new cache node */
    LOS_SpinLockSave(&mapping->list_lock, &intSave);
	//查找页缓存
    fpage = OsFindGetEntry(mapping, vmf->pgoff);
    if (fpage != NULL) {
        OsPageRefIncLocked(fpage); //页缓存存在，增加引用计数
    } else {
    	//新建新的页缓存
        fpage = OsPageCacheAlloc(mapping, vmf->pgoff);
        if (fpage == NULL) {
            VM_ERR("Failed to alloc a page frame");
            LOS_SpinUnlockRestore(&mapping->list_lock, intSave);
            return LOS_NOK;
        }
        newCache = true;
    }
    OsSetPageLocked(fpage->vmPage); //锁住需要操作的物理页
    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);
    kvaddr = OsVmPageToVaddr(fpage->vmPage); //获取内存页首地址

    /* read file to new page cache */
    if (newCache) {
		//新的页缓存，则需要存入数据
        oldPos = file_seek(file, 0, SEEK_CUR); //备份旧文件指针
        file_seek(file, fpage->pgoff << PAGE_SHIFT, SEEK_SET); //调整文件指针便于从文件中读入数据
        //读入数据到页缓存中
        if (file->f_inode && file->f_inode->u.i_mops->readpage) {
            ret = file->f_inode->u.i_mops->readpage(file, (char *)kvaddr, PAGE_SIZE);
        } else {
            ret = file_read(file, kvaddr, PAGE_SIZE);
        }
        file_seek(file, oldPos, SEEK_SET); //恢复文件指针
        if (ret == 0) {
            VM_ERR("Failed to read from file!");
            OsReleaseFpage(mapping, fpage);
            return LOS_NOK;
        }
        LOS_SpinLockSave(&mapping->list_lock, &intSave);
        OsAddToPageacheLru(fpage, mapping, vmf->pgoff); //将页缓存放入LRU队列和文件缓存队列
        LOS_SpinUnlockRestore(&mapping->list_lock, intSave);
    }

    LOS_SpinLockSave(&mapping->list_lock, &intSave);
    /* cow fault case no need to save mapinfo */
    if (!((vmf->flags & VM_MAP_PF_FLAG_WRITE) && !(region->regionFlags & VM_MAP_REGION_FLAG_SHARED))) {
		//读操作或者共享内存的写操作，需要记录下操作的虚拟地址
        OsAddMapInfo(fpage, &region->space->archMmu, (vaddr_t)vmf->vaddr);
        fpage->flags = region->regionFlags;
    }

    /* share page fault, mark the page dirty */
    if ((vmf->flags & VM_MAP_PF_FLAG_WRITE) && (region->regionFlags & VM_MAP_REGION_FLAG_SHARED)) {
		//上一次向共享内存写数据失败，这次标记为脏页以后，后台会重试
        OsMarkPageDirty(fpage, region, 0, 0);
    }

    vmf->pageKVaddr = kvaddr; //记录下引起异常的虚拟地址
    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);
    return LOS_OK;
}


//将文件缓存中的数据写入磁盘
VOID OsFileCacheFlush(struct page_mapping *mapping)
{
    UINT32 intSave;
    UINT32 lruLock;
    LOS_DL_LIST_HEAD(dirtyList);
    LosFilePage *ftemp = NULL;
    LosFilePage *fpage = NULL;

    if (mapping == NULL) {
        return;
    }
    LOS_SpinLockSave(&mapping->list_lock, &intSave);
	//遍历文件所有缓存页
    LOS_DL_LIST_FOR_EACH_ENTRY(fpage, &mapping->page_list, LosFilePage, node) {
        LOS_SpinLockSave(&fpage->physSeg->lruLock, &lruLock);
        if (OsIsPageDirty(fpage->vmPage)) {
			//先将脏页描述符拷贝一份放入临时链表
            ftemp = OsDumpDirtyPage(fpage);
            if (ftemp != NULL) {
                LOS_ListTailInsert(&dirtyList, &ftemp->node);
            }
        }
        LOS_SpinUnlockRestore(&fpage->physSeg->lruLock, lruLock);
    }
    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);

	//遍历脏页链表
    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(fpage, ftemp, &dirtyList, LosFilePage, node) {
    	//脏页数据写入磁盘
        OsDoFlushDirtyPage(fpage);
    }
}


//清除文件页缓存
VOID OsFileCacheRemove(struct page_mapping *mapping)
{
    UINT32 intSave;
    UINT32 lruSave;
    SPIN_LOCK_S *lruLock = NULL;
    LOS_DL_LIST_HEAD(dirtyList);
    LosFilePage *ftemp = NULL;
    LosFilePage *fpage = NULL;
    LosFilePage *fnext = NULL;

    LOS_SpinLockSave(&mapping->list_lock, &intSave);
	//遍历文件页缓存
    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(fpage, fnext, &mapping->page_list, LosFilePage, node) {
        lruLock = &fpage->physSeg->lruLock;
        LOS_SpinLockSave(lruLock, &lruSave);
        if (OsIsPageDirty(fpage->vmPage)) {
			//拷贝脏页描述符，放入临时链表
            ftemp = OsDumpDirtyPage(fpage);
            if (ftemp != NULL) {
                LOS_ListTailInsert(&dirtyList, &ftemp->node);
            }
        }

        OsDeletePageCacheLru(fpage); //清除原页缓存
        LOS_SpinUnlockRestore(lruLock, lruSave);
    }
    LOS_SpinUnlockRestore(&mapping->list_lock, intSave);

	//遍历脏页
    LOS_DL_LIST_FOR_EACH_ENTRY_SAFE(fpage, fnext, &dirtyList, LosFilePage, node) {
    	//脏页数据写入磁盘
        OsDoFlushDirtyPage(fpage);
    }
}

//虚拟内存文件操作
LosVmFileOps g_commVmOps = {
    .open = NULL,
    .close = NULL,
    .fault = OsVmmFileFault, //文件页异常操作
    .remove = OsVmmFileRemove, //文件页缓存移除操作
};

//将文件和内存区关联
INT32 OsVfsFileMmap(struct file *filep, LosVmMapRegion *region)
{
    region->unTypeData.rf.vmFOps = &g_commVmOps;
    region->unTypeData.rf.file = filep;
    region->unTypeData.rf.fileMagic = filep->f_magicnum;
    return ENOERR;
}

//文件内存映射
STATUS_T OsNamedMMap(struct file *filep, LosVmMapRegion *region)
{
    struct inode *inodePtr = NULL;
    if (filep == NULL) {
        return LOS_ERRNO_VM_MAP_FAILED;
    }
    inodePtr = filep->f_inode; //文件索引节点
    if (inodePtr == NULL) {
        return LOS_ERRNO_VM_MAP_FAILED;
    }
    if (INODE_IS_MOUNTPT(inodePtr)) { //普通文件
        if (inodePtr->u.i_mops->mmap) {
            LOS_SetRegionTypeFile(region);
            return inodePtr->u.i_mops->mmap(filep, region); //执行具体映射过程
        } else {
            VM_ERR("file mmap not support");
            return LOS_ERRNO_VM_MAP_FAILED;
        }
    } else if (INODE_IS_DRIVER(inodePtr)) { //设备文件
        if (inodePtr->u.i_ops && inodePtr->u.i_ops->mmap) {
            LOS_SetRegionTypeDev(region);
            return inodePtr->u.i_ops->mmap(filep, region); //执行具体映射过程
        } else {
            VM_ERR("dev mmap not support");
            return LOS_ERRNO_VM_MAP_FAILED;
        }
    } else {
        VM_ERR("mmap file type unknown");
        return LOS_ERRNO_VM_MAP_FAILED;
    }
}

//查询页缓存
LosFilePage *OsFindGetEntry(struct page_mapping *mapping, VM_OFFSET_T pgoff)
{
    LosFilePage *fpage = NULL;

	//遍历页缓存
    LOS_DL_LIST_FOR_EACH_ENTRY(fpage, &mapping->page_list, LosFilePage, node) {
        if (fpage->pgoff == pgoff) {
            return fpage; //根据页编号，找到页缓存
        }

        if (fpage->pgoff > pgoff) {
            break; //由于页缓存是升序排列，所以后面不可能再找到了
        }
    }

    return NULL;
}

/* need mutex & change memory to dma zone. */
//申请页缓存
LosFilePage *OsPageCacheAlloc(struct page_mapping *mapping, VM_OFFSET_T pgoff)
{
    VOID *kvaddr = NULL;
    LosVmPhysSeg *physSeg = NULL;
    LosVmPage *vmPage = NULL;
    LosFilePage *fpage = NULL;

    vmPage = LOS_PhysPageAlloc(); //申请物理页
    if (vmPage == NULL) {
        VM_ERR("alloc vm page failed");
        return NULL;
    }
    physSeg = OsVmPhysSegGet(vmPage); //物理页所在的物理内存段
    kvaddr = OsVmPageToVaddr(vmPage); //物理页对应的内核虚拟地址
    if ((physSeg == NULL) || (kvaddr == NULL)) {
        LOS_PhysPageFree(vmPage);
        VM_ERR("alloc vm page failed!");
        return NULL;
    }

	//申请页缓存描述符
    fpage = (LosFilePage *)LOS_MemAlloc(m_aucSysMem0, sizeof(LosFilePage));
    if (fpage == NULL) {
        LOS_PhysPageFree(vmPage);
        VM_ERR("Failed to allocate for page!");
        return NULL;
    }

	//初始化页缓存描述符
    (VOID)memset_s((VOID *)fpage, sizeof(LosFilePage), 0, sizeof(LosFilePage));

    LOS_ListInit(&fpage->i_mmap);
    LOS_ListInit(&fpage->node);
    LOS_ListInit(&fpage->lru);
    fpage->n_maps = 0;
    fpage->dirtyOff = PAGE_SIZE; //没有脏数据
    fpage->dirtyEnd = 0;
    fpage->physSeg = physSeg; //页缓存所在的物理内存段
    fpage->vmPage = vmPage;   //页缓存所使用的物理页
    fpage->mapping = mapping; //页缓存所在的文件映射
    fpage->pgoff = pgoff;     //页缓存对应的页编号
    //物理页内容清0
    (VOID)memset_s(kvaddr, PAGE_SIZE, 0, PAGE_SIZE);

    return fpage;
}

#ifdef LOSCFG_FS_VFS
//释放指定进程的某映射文件
VOID OsVmmFileRegionFree(struct file *filep, LosProcessCB *processCB)
{
    int ret;
    LosVmSpace *space = NULL;
    LosVmMapRegion *region = NULL;
    LosRbNode *pstRbNode = NULL;
    LosRbNode *pstRbNodeTmp = NULL;

    if (processCB == NULL) {
        processCB = OsCurrProcessGet(); //如果没有指定进程，则使用当前进程
    }

    space = processCB->vmSpace;  //进程的地址空间
    if (space != NULL) {
        (VOID)LOS_MuxAcquire(&space->regionMux);
        /* free the regions associated with filep */
        RB_SCAN_SAFE(&space->regionRbTree, pstRbNode, pstRbNodeTmp)
            region = (LosVmMapRegion *)pstRbNode;
            if (LOS_IsRegionFileValid(region)) {
                if (region->unTypeData.rf.file != filep) {
                    continue;
                }
                ret = LOS_RegionFree(space, region); //删除与文件映射的内存区
                if (ret != LOS_OK) {
                    VM_ERR("free region error, space %p, region %p", space, region);
                }
            }
        RB_SCAN_SAFE_END(&space->regionRbTree, pstRbNode, pstRbNodeTmp)
        (VOID)LOS_MuxRelease(&space->regionMux);
    }
}
#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
