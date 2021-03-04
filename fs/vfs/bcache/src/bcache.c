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

#include "bcache.h"
#include "assert.h"
#include "stdlib.h"
#include "linux/delay.h"
#include "disk_pri.h"
#include "fs_other.h"
#include "user_copy.h"

#undef HALARC_ALIGNMENT
#define DMA_ALLGN          64
#define HALARC_ALIGNMENT   DMA_ALLGN
#define BCACHE_MAGIC_NUM   20132016
#define BCACHE_STATCK_SIZE 0x3000
#define ASYNC_EVENT_BIT    0x01

#ifdef DEBUG
#define D(args) printf(args)
#else
#define D(args)
#endif

#ifdef LOSCFG_FS_FAT_CACHE_SYNC_THREAD

UINT32 g_syncThreadPrio = CONFIG_FS_FAT_SYNC_THREAD_PRIO;
UINT32 g_dirtyRatio = CONFIG_FS_FAT_DIRTY_RATIO;
UINT32 g_syncInterval = CONFIG_FS_FAT_SYNC_INTERVAL;

VOID LOS_SetDirtyRatioThreshold(UINT32 dirtyRatio)
{
    if ((dirtyRatio != g_dirtyRatio) && (dirtyRatio <= 100)) { /* The ratio cannot exceed 100% */
        g_dirtyRatio = dirtyRatio;  //设置脏页上限比率，超过这个比率就可以触发脏页同步
    }
}

//设置数据同步时间间隔--周期同步
VOID LOS_SetSyncThreadInterval(UINT32 interval)
{
    g_syncInterval = interval;
}


//设置目标磁盘同步线程的优先级
INT32 LOS_SetSyncThreadPrio(UINT32 prio, const CHAR *name)
{
    INT32 ret = VFS_ERROR;
    INT32 diskID;
    los_disk *disk = NULL;
    if ((prio == 0) || (prio >= OS_TASK_PRIORITY_LOWEST)) { /* The priority can not be zero */
        return ret;  //优先级不合法
    }

    g_syncThreadPrio = prio;  //记录优先级

    /*
     * If the name is NULL, it only sets the value of a global variable,
     * and takes effect the next time the thread is created.
     */
    if (name == NULL) {
        return ENOERR;  //没有指定磁盘的话，下次指定磁盘时再生效
    }

    /* If the name is not NULL, it shall return an error if can't find the disk corresponding to name. */
    diskID = los_get_diskid_byname(name); //根据名字查询磁盘
    disk = get_disk(diskID); //获取磁盘控制块
    if (disk == NULL) {
        return ret;
    }

    if (pthread_mutex_lock(&disk->disk_mutex) != ENOERR) {
        PRINT_ERR("%s %d, mutex lock fail!\n", __FUNCTION__, __LINE__);
        return ret;
    }
    if ((disk->disk_status == STAT_INUSED) && (disk->bcache != NULL)) {
		//磁盘存在块缓存，设置对应数据同步任务的优先级
        ret = LOS_TaskPriSet(disk->bcache->syncTaskId, prio);
    }
    if (pthread_mutex_unlock(&disk->disk_mutex) != ENOERR) {
        PRINT_ERR("%s %d, mutex unlock fail!\n", __FUNCTION__, __LINE__);
        return VFS_ERROR;
    }
    return ret;
}
#endif

//根据编号查询块缓存(从红黑树根部开始查找)
static OsBcacheBlock *RbFindBlock(const OsBcache *bc, UINT64 num)
{
    OsBcacheBlock *block = NULL;
    struct rb_node *node = bc->rbRoot.rb_node;

	//以num排序的红黑树
    for (; node != NULL; node = (block->num < num) ? node->rb_right : node->rb_left) {
        block = rb_entry(node, OsBcacheBlock, rbNode);
        if (block->num == num) {
            return block; //找到块缓存
        }
    }
    return NULL;
}

//添加块缓存
static VOID RbAddBlock(OsBcache *bc, OsBcacheBlock *block)
{
    struct rb_node *node = bc->rbRoot.rb_node;
    struct rb_node **link = NULL;
    OsBcacheBlock *b = NULL;

    if (node == NULL) {
		//向空的红黑树添加块
        rb_link_node(&block->rbNode, NULL, &bc->rbRoot.rb_node);
    } else {
    	//从树根开始，寻找合适的插入位置
        for (; node != NULL; link = (b->num > block->num) ? &node->rb_left : &node->rb_right, node = *link) {
            b = rb_entry(node, OsBcacheBlock, rbNode);
            if (b->num == block->num) {
                PRINT_ERR("RbAddBlock fail, b->num = %llu, block->num = %llu\n", b->num, block->num);
                return;  //此编号的块缓存已存在，不允许重复添加
            }
        }
		//找到了插入位置，添加到红黑树中
        rb_link_node(&block->rbNode, &b->rbNode, link);
    }
	//根据红黑树算法，标记新插入节点的颜色,并做适当处理
    rb_insert_color(&block->rbNode, &bc->rbRoot);
}

//删除块缓存节点
static inline VOID RbDelBlock(OsBcache *bc, OsBcacheBlock *block)
{
    rb_erase(&block->rbNode, &bc->rbRoot);
}

//将块缓存移动到链表头部
static inline VOID ListMoveBlockToHead(OsBcache *bc, OsBcacheBlock *block)
{
    LOS_ListDelete(&block->listNode);
    LOS_ListAdd(&bc->listHead, &block->listNode);
}

//释放块缓存，放入空闲链
static inline VOID FreeBlock(OsBcache *bc, OsBcacheBlock *block)
{
    block->used = FALSE;
    LOS_ListAdd(&bc->freeListHead, &block->listNode);
}

//求对数
static UINT32 GetValLog2(UINT32 val)
{
    UINT32 i, log2;

    i = val;
    log2 = 0;
    while ((i & 1) == 0) { /* Check if the last bit is 1 */
        i >>= 1;
        log2++;
    }
    if (i != 1) { /* Not the power of 2 */
        return 0; //val不是2的幂指数
    }

    return log2;  //求出对数，即末尾连续的0bit的个数
}

//从位图数组中查找连续的扇区缓存(一个块中有多个扇区)，记录开始和结束位置
//正常情况下，有且仅有一块连续的置1bit
static INT32 FindFlagPos(const UINT32 *arr, UINT32 len, UINT32 *p1, UINT32 *p2)
{
    UINT32 *start = p1;
    UINT32 *end = p2;
    UINT32 i, j, tmp;
    UINT32 val = 1;

    *start = BCACHE_MAGIC_NUM;
    *end = 0;
    for (i = 0; i < len; i++) { //遍历位图数组
        for (j = 0; j < UNSIGNED_INTEGER_BITS; j++) { //遍历其中每一个位
            tmp = arr[i] << j;
            tmp = tmp >> UNINT_MAX_SHIFT_BITS;  //查看第j位(从高权重往低数)
            if (tmp != val) {
                continue;  //第一次查为1的位。第二次查为0的位，第三次又查为1的位
            }
            if (val && (*start == BCACHE_MAGIC_NUM)) {
                *start = (i << UNINT_LOG2_SHIFT) + j; //找到1开始位置，第i个元素中的第j位
                val = 1 - val; /* Control parity by 0 and 1 */ //第2次查之后的0位
            } else if (val && (*start != BCACHE_MAGIC_NUM)) {
                *start = 0;  //1和0都找到后，又找到了1， 这种情况是不合理的
                //即整个位图数组应该只出现若干连续的1位，且只出现1次。
                return VFS_ERROR;
            } else {
                *end = (i << UNINT_LOG2_SHIFT) + j;   //找到1之后0开始的位置，第i个元素中的第j位
                val = 1 - val; /* Control parity by 0 and 1 */ //第3次查之后的1位
            }
        }
    }
    if (*start == BCACHE_MAGIC_NUM) {
        *start = 0;
        return VFS_ERROR;  //不含bit 1  ， 位图中应该含有若干置1的bit才对
    }
    if (*end == 0) {
		//1之后不含bit 0，那么end就是整个位图数组的末尾
        *end = len << UNINT_LOG2_SHIFT;
    }

    return ENOERR;
}


//从块缓存读取数据,读入buf
static INT32 BlockRead(OsBcache *bc, OsBcacheBlock *block, UINT8 *buf)
{
	//以扇区为单位，将数据读入buf
    INT32 ret = bc->breadFun(bc->priv, buf, bc->sectorPerBlock,
                             (block->num) << GetValLog2(bc->sectorPerBlock));
    if (ret) {
        PRINT_ERR("BlockRead, brread_fn error, ret = %d\n", ret);
        if (block->modified == FALSE) {
			//如果读取失败，缓存中数据也没有修改，那么释放块缓存
            if (block->listNode.pstNext != NULL) {
				//从链表中移除
                LOS_ListDelete(&block->listNode); /* list del block */
                RbDelBlock(bc, block); //从红黑树移除
            }
            FreeBlock(bc, block); //加入空闲链
        }
        return ret;
    }

    block->readFlag = TRUE;  //读取成功
    return ENOERR;
}


//将磁盘数据读入缓存，只覆盖不是脏数据的部分(以扇区为单位)。
//因为脏数据还需要写回磁盘
static INT32 BcacheGetFlag(OsBcache *bc, OsBcacheBlock *block)
{
    UINT32 i, n, f, sectorPos, val, start, pos, currentSize;
	//这里求出block中位图的尺寸
    UINT32 flagUse = bc->sectorPerBlock >> UNINT_LOG2_SHIFT;  //每一个bit代表一个扇区
    UINT32 flag = UINT_MAX;
    INT32 ret, bits;

    if (block->readFlag == TRUE) {
        return ENOERR;  //已经读入了
    }

    for (i = 0; i < flagUse; i++) {
        flag &= block->flag[i];  //判断是否所有扇区都是脏数据
    }

    if (flag == UINT_MAX) {
        return ENOERR;  //所有位都为1，表明所有扇区都是脏数据
    }

	//还有扇区不是脏数据，那么从磁盘读入数据刷新这些扇区
	//将将磁盘按块整体读入到临时缓存
    ret = BlockRead(bc, block, bc->rwBuffer);
    if (ret != ENOERR) {
        return ret;
    }

	//然后刷新缓存块中非脏部分的数据
    for (i = 0, sectorPos = 0; i < flagUse; i++) {
		//以扇区为单位判断哪些数据需要拷贝
        val = block->flag[i];
        /* use unsigned integer for bit map */
		//bits = bits - (INT32)n 每轮处理n个扇区
		//val = ~(val << n), 交替进行拷贝和不拷贝处理，即连续的0bit做拷贝处理，连续的1bit做不拷贝处理
		//(f % EVEN_JUDGED) , 这里的奇偶判断就是为了实现上述的交替逻辑
        for (f = 0, bits = UNSIGNED_INTEGER_BITS; bits > 0; val = ~(val << n), f++, bits = bits - (INT32)n) {
			//计算位图中最左侧连续0bit的数目
            if (val == 0) {
                n = UNSIGNED_INTEGER_BITS;  //所有位为0
            } else {
                n = (UINT32)CLZ(val);   //最高的连续0位的数目
            }
            sectorPos += n; //连续0位之后的为1的bit的位置
            if (((f % EVEN_JUDGED) != 0) || (n == 0)) { /* Number of leading zeros of n is zero */
				//因为val在迭代过程中会移位并按位取反val = ~(val << n)，所以
				//连续的0和连续的1bit会交替得到判断
                goto LOOP;  //第奇数次是对连续1bit进行判断，或者第偶数次判断是无连续的0bit。都应该进入下一轮迭代
            }
			//第偶数次迭代，有连续的0bit，这些bit对应的扇区应该存入块缓存中
            if (sectorPos > ((i + 1) << UNINT_LOG2_SHIFT)) {				
                start = sectorPos - n;  //数据拷贝起始位置，扇区编号
                //由于左移运算的右侧0的填充作用，最后一个连续位段的bit数会比实际的多
                //需要使用实际数目来进行拷贝
                currentSize = (((i + 1) << UNINT_LOG2_SHIFT) - start) * bc->sectorSize;
            } else {
                start = sectorPos - n;  //数据拷贝起始位置，扇区编号
                currentSize = n * bc->sectorSize; //数据拷贝尺寸，连续n个扇区
            }
            pos = start * bc->sectorSize; //起始拷贝位置，字节编号
            //从临时缓存区拷贝进块缓冲区
            if (memcpy_s(block->data + pos, bc->blockSize - pos, bc->rwBuffer + pos, currentSize) != EOK) {
                return VFS_ERROR;
            }
LOOP:
			//由于左移运算的右侧0的填充作用，最后一个连续位段的bit数会比实际的多
            //需要调整成实际的边界
            if (sectorPos > ((i + 1) << UNINT_LOG2_SHIFT)) {
                sectorPos = (i + 1) << UNINT_LOG2_SHIFT;  
            }
        }
    }

    return ENOERR;
}


//将块缓存中指定范围的扇区标记成脏数据扇区
static VOID BcacheSetFlag(const OsBcache *bc, OsBcacheBlock *block, UINT32 pos, UINT32 size)
{
    UINT32 start, num, i, j, k;

    if (bc->sectorSize == 0) {
        PRINT_ERR("BcacheSetFlag sectorSize is equal to zero! \n");
        return;
    }

    start = pos / bc->sectorSize;  //起始扇区编号
    num = size / bc->sectorSize;   //需要标记的扇区数目

    i = start / UNSIGNED_INTEGER_BITS;  //起始位图数组下标
    j = start % UNSIGNED_INTEGER_BITS;  //位图数组元素内的起始位编号
    for (k = 0; k < num; k++) {  //共需要对num个扇区进行标记
    	//对第i个位图内第j位进行标记
        block->flag[i] |= 1u << (UNINT_MAX_SHIFT_BITS - j);
        j++; //位序号增加
        if (j == UNSIGNED_INTEGER_BITS) {
            j = 0; //位序号达到最大值，
            i++;   //则位图序号增加
        }
    }
}


//将缓存块中的脏数据同步到磁盘
static INT32 BcacheSyncBlock(OsBcache *bc, OsBcacheBlock *block)
{
    INT32 ret = ENOERR;
    UINT32 len, start, end;

    if (block->modified == TRUE) {  //缓存块最近修改过，才有必要同步
        D(("bcache writting block = %llu\n", block->num));

		//找出需要同步数据的起始和结束(不含)扇区
        ret = FindFlagPos(block->flag, bc->sectorPerBlock >> UNINT_LOG2_SHIFT, &start, &end);
        if (ret == ENOERR) {
			//只有一组连续的扇区数据块需要同步的情况下
            len = end - start;  //需要同步的扇区数
        } else {
            ret = BcacheGetFlag(bc, block);
            if (ret != ENOERR) {
                return ret;
            }
			//否则同步所有扇区，在同步所有扇区之前，先再次读入不含脏数据的扇区
            len = bc->sectorPerBlock;
        }

		//将块缓存中的数据写入磁盘，起始扇区号start, 扇区数len
        ret = bc->bwriteFun(bc->priv, (const UINT8 *)(block->data + (start * bc->sectorSize)),
                            len, (block->num * bc->sectorPerBlock) + start);
        if (ret == ENOERR) {
            block->modified = FALSE;  //此块缓存的脏数据已写入
            bc->modifiedBlock--;  //剩余需要写入脏数据的缓存块减少
        } else {
            PRINT_ERR("BcacheSyncBlock fail, ret = %d, len = %u, block->num = %llu, start = %u\n",
                      ret, len, block->num, start);
        }
    }
    return ret;
}

//块缓存按块序号升序方式加入链表
static void NumListAdd(OsBcache *bc, OsBcacheBlock *block)
{
    OsBcacheBlock *temp = NULL;

    LOS_DL_LIST_FOR_EACH_ENTRY(temp, &bc->numHead, OsBcacheBlock, numNode) {
        if (temp->num > block->num) {
            LOS_ListTailInsert(&temp->numNode, &block->numNode);
            return;
        }
    }

    LOS_ListTailInsert(&bc->numHead, &block->numNode);
}

//将块缓存加入磁盘缓存子系统
static void AddBlock(OsBcache *bc, OsBcacheBlock *block)
{
    RbAddBlock(bc, block);  //加入红黑树
    NumListAdd(bc, block);  //加入块序号链表
    bc->sumNum += block->num;  //已有块缓存序号求和
    bc->nBlock++;  //当前块缓存数
    LOS_ListAdd(&bc->listHead, &block->listNode);  //加入LRU链表
}

//删除块缓存
static void DelBlock(OsBcache *bc, OsBcacheBlock *block)
{
	//从LRU链表移除缓存块
    LOS_ListDelete(&block->listNode); /* lru list del */
	//从序号链表中移除缓存块
    LOS_ListDelete(&block->numNode);  /* num list del */
    bc->sumNum -= block->num;  //缓存块序号总数减少
    bc->nBlock--;  //缓存块数目减少
    //从红黑树中移除缓存块
    RbDelBlock(bc, block);            /* rb  tree del */
	//将缓存块放入空闲链
    FreeBlock(bc, block);             /* free list add */
}

//是否所有扇区都有脏数据
static BOOL BlockAllDirty(const OsBcache *bc, OsBcacheBlock *block)
{
    UINT32 start = 0;
    UINT32 end = 0;
    UINT32 len = bc->sectorPerBlock >> UNINT_LOG2_SHIFT;  //用来表示每个扇区状态的位图数组的长度

    if (block->modified == TRUE) {  //如果块中的数据已修改
        if (block->allDirty) {
            return TRUE; //所有扇区都含有脏数据，所有扇区都需要写磁盘
        }

		//在位图中查找连续1bit的开始和结束位置
        if (FindFlagPos(block->flag, len, &start, &end) == ENOERR) {
            if ((end - start) == bc->sectorPerBlock) {  //1bit的总数与扇区数相等
            	//意思就是所有扇区都对应1bit, 即所有扇区都含脏数据
                block->allDirty = TRUE;
                return TRUE;
            }
        }
    }

    return FALSE;  //存在0bit的情况，则不是全脏的块(含有不脏的扇区)
}

//写用户数据前，获得块缓存资源的方法之一
static OsBcacheBlock *GetBaseBlock(OsBcache *bc)
{
    OsBcacheBlock *base = bc->wStart;
    OsBcacheBlock *end = bc->wEnd;
    while (base < end) {  //遍历缓存块
        if (base->used == FALSE) {  //寻找空闲的缓存块
            base->used = TRUE;  //置成已使用
            LOS_ListDelete(&base->listNode);  //并从空闲链中移除
            return base;
        }
        base++;
    }

    return NULL; //没有找到缓存块
}

/* try get free block first, if failed free a useless block */
//获取缓存块用于读写数据
static OsBcacheBlock *GetSlowBlock(OsBcache *bc, BOOL read)
{
    LOS_DL_LIST *node = NULL;
    OsBcacheBlock *block = NULL;

	//先遍历空闲块链表
    LOS_DL_LIST_FOR_EACH_ENTRY(block, &bc->freeListHead, OsBcacheBlock, listNode) {
        if (block->readBuff == read) {
			//此空闲块之前也是用于读或者写，本次不变，拿来继续使用
            block->used = TRUE;
            LOS_ListDelete(&block->listNode);
            return block; /* get free one */
        }
    }

	//如果没有找到合适的空闲块，则从LRU链表中寻找合适的可以重用的缓存块
	//链表尾部是最久未使用的，因为每次使用时都是从头部插入
    node = bc->listHead.pstPrev;
    while (node != &bc->listHead) {
        block = LOS_DL_LIST_ENTRY(node, OsBcacheBlock, listNode);
        node = block->listNode.pstPrev;

        if (block->readBuff == read && read && !block->modified) {
			//本次缓存用来做读操作，且此缓存无脏数据
            DelBlock(bc, block);  //先释放此块
            block->used = TRUE;   //然后再次使用此块
            LOS_ListDelete(&block->listNode);
            return block; /* read only block */
        }
    }

	//还没有获取到缓存，再遍历LRU链表
    node = bc->listHead.pstPrev;
    while (node != &bc->listHead) {
        block = LOS_DL_LIST_ENTRY(node, OsBcacheBlock, listNode);
        node = block->listNode.pstPrev;

        if (block->readBuff == read) {
			//缓存读写属性一致
            if (block->modified == TRUE) {
				//如果存在脏块，则先将脏数据写入磁盘
                BcacheSyncBlock(bc, block);
            }

			//然后这个缓存就可以重用了
            DelBlock(bc, block);  
            block->used = TRUE;
            LOS_ListDelete(&block->listNode);
            return block; /* get used one */
        }
    }

    return NULL;
}

/* flush combined blocks */
//写入连续的若干块缓存的数据到磁盘
static VOID WriteMergedBlocks(OsBcache *bc, OsBcacheBlock *begin, int blocks)
{
    INT32 ret;
    OsBcacheBlock *cur = NULL;
    OsBcacheBlock *next = NULL;
    UINT32 len = blocks * bc->sectorPerBlock; //需要写入的扇区总数
    UINT64 pos = begin->num * bc->sectorPerBlock; //需要写入的起始扇区编号

    ret = bc->bwriteFun(bc->priv, (const UINT8 *)begin->data, len, pos);
    if (ret != ENOERR) {
        PRINT_ERR("WriteMergedBlocks bwriteFun failed ret %d\n", ret);
        return;
    }

    bc->modifiedBlock -= blocks;  //剩余需要写入的块减少
    cur = begin;
    while (blocks > 0) {
		//释放已经写入磁盘数据的缓存块
        next = LOS_DL_LIST_ENTRY(cur->numNode.pstNext, OsBcacheBlock, numNode);
        DelBlock(bc, cur);
        blocks--;
        cur = next;
    }
}

/* find continue blocks and flush them */
//找出若干连续的全脏缓存块，将其同步到磁盘
static VOID MergeSyncBlocks(OsBcache *bc, OsBcacheBlock *start)
{
    INT32 mergedBlock = 0;
    OsBcacheBlock *cur = start;  //起始块
    OsBcacheBlock *last = NULL;

    while (cur <= bc->wEnd) {
        if (!cur->used || !BlockAllDirty(bc, cur)) {
            break; //当前为空闲块，或者当前块不是全脏
        }

        if (last && (last->num + 1 != cur->num)) {
            break;  //上一缓存块和当前缓存块在磁盘上不连续
        }

        mergedBlock++; //可以合并写入的缓存块增加
        last = cur;
        cur++;  //继续考察下一个缓存块
    }

    if (mergedBlock > 0) {
		//存在可以合并写入的缓存块，则一次性写入这些缓存块的数据
        WriteMergedBlocks(bc, start, mergedBlock);
    }
}

/* get the min write block num of block cache buffer */
//获取需要做写操作的缓存块中，序号最小的那个
static inline UINT64 GetMinWriteNum(OsBcache *bc)
{
    UINT64 ret = 0;
    OsBcacheBlock *block = NULL;

	//按序号遍历缓存块链表
    LOS_DL_LIST_FOR_EACH_ENTRY(block, &bc->numHead, OsBcacheBlock, numNode) {
        if (!block->readBuff) { //查找写操作缓存块
            ret = block->num;  //返回对应的编号
            break;
        }
    }

    return ret;
}

//申请一个用于读/写操作的缓存块，并记录块编号
static OsBcacheBlock *AllocNewBlock(OsBcache *bc, BOOL read, UINT64 num)
{
    OsBcacheBlock *last = NULL;
    OsBcacheBlock *prefer = NULL;

    if (read) { /* read */
        return GetSlowBlock(bc, TRUE);  //读操作的缓存块只能通过此函数申请
    }

	//写操作的缓存块需要特殊考虑，尽可能将多个连续磁盘块的写入
	//与连续的缓存块关联起来，这样可以减少操作磁盘的次数
    /* fallback, this may happen when the block previously flushed, use read buffer */
    if (bc->nBlock && num < GetMinWriteNum(bc)) {
		//新写入的这个磁盘块没有在另一个写操作的磁盘块之后
		//所以没有机会合并成连续的磁盘块
		//采用传统方法申请缓存块
        return GetSlowBlock(bc, TRUE);
    }

	//在红黑树中查找上一个序号对应的缓存块
    last = RbFindBlock(bc, num - 1);  /* num=0 is ok */
    if (last == NULL || last->readBuff) {
		//上一个缓存块不存在，或者上一个缓存块是读操作缓存块
		//那么本缓存块从系统中获取某缓存块(不考虑实际位置)
        return GetBaseBlock(bc);      /* new block */
    }

	//否则有限考虑相邻的缓存块，即上一个缓存块和本缓存块相邻
	//这样，磁盘中相邻，内存中也相邻，可以减少读写磁盘次数
    prefer = last + 1;
    if (prefer > bc->wEnd) {
        prefer = bc->wStart;  //块编号回绕
    }

    /* this is a sync thread synced block! */
    if (prefer->used && !prefer->modified) {
		//这个缓存块虽然正在使用，但其与磁盘一致，无脏数据
		//先释放以后再重用
        prefer->used = FALSE;
        DelBlock(bc, prefer);
    }

    if (prefer->used) {
		//如果有脏数据，则先将所有扇区都脏的块同步到磁盘
        MergeSyncBlocks(bc, prefer);
    }

    if (prefer->used) {
		//还有脏数据，则把脏扇区同步到磁盘
        BcacheSyncBlock(bc, prefer);
        DelBlock(bc, prefer);  //回收此缓存块
    }

    prefer->used = TRUE;
    LOS_ListDelete(&prefer->listNode); /* del from free list */

    return prefer;  //使用回收的缓存块
}


//同步所有脏数据到磁盘
static INT32 BcacheSync(OsBcache *bc)
{
    LOS_DL_LIST *node = NULL;
    OsBcacheBlock *block = NULL;
    INT32 ret = ENOERR;

    D(("bcache cache sync\n"));

    (VOID)pthread_mutex_lock(&bc->bcacheMutex);
    node = bc->listHead.pstPrev;
    while (&bc->listHead != node) { //遍历所有磁盘块
        block = LOS_DL_LIST_ENTRY(node, OsBcacheBlock, listNode);
        ret = BcacheSyncBlock(bc, block); //同步磁盘块中的脏数据
        if (ret != ENOERR) {
            PRINT_ERR("BcacheSync error, ret = %d\n", ret);
            break;
        }
        node = node->pstPrev;
    }
    (VOID)pthread_mutex_unlock(&bc->bcacheMutex);

    return ret;
}

//初始化缓存块
static VOID BlockInit(OsBcache *bc, OsBcacheBlock *block, UINT64 num)
{
    (VOID)memset_s(block->flag, sizeof(block->flag), 0, sizeof(block->flag));
    block->num = num;  //缓存块对应的磁盘块序号
    block->readFlag = FALSE;  //还没有读入数据到缓存块
    if (block->modified == TRUE) {  //如果用户之前修改了这个缓存块数据
        block->modified = FALSE;  //那么放弃这些数据
        bc->modifiedBlock--;  //不再同步这个缓存块中的数据到磁盘
    }
    block->allDirty = FALSE; //目前没有脏数据
}

//获取用于某磁盘块读或写操作的缓存块
static INT32 BcacheGetBlock(OsBcache *bc, UINT64 num, BOOL readData, OsBcacheBlock **dblock)
{
    INT32 ret;
    OsBcacheBlock *block = NULL;
    OsBcacheBlock *first = NULL;

    /*
     * First check if the most recently used block is the requested block,
     * this can improve performance when using byte access functions.
     */
    if (LOS_ListEmpty(&bc->listHead) == FALSE) {
		//查看最近刚使用的缓存块，是不是我现在请求的磁盘块
        first = LOS_DL_LIST_ENTRY(bc->listHead.pstNext, OsBcacheBlock, listNode);
		//块号相同，则匹配，否则从红黑树去查找磁盘块
		//获取链表第一个元素比从红黑树查找效率高
        block = (first->num == num) ? first : RbFindBlock(bc, num);
    }

    if (block != NULL) {
		//磁盘块已存在
        D(("bcache block = %llu found in cache\n", num));

        if (first != block) {
			//移动到链表头部，表示最近刚使用
			//这样，最久未使用自然会遗留到链表尾部
            ListMoveBlockToHead(bc, block);
        }
        *dblock = block;  //记录查找到的缓存块

        if ((bc->prereadFun != NULL) && (readData == TRUE) && (block->pgHit == 1)) {
			//存在预读函数，本次缓存块是用来辅助读操作，本缓存块的前一轮
            block->pgHit = 0;  
            //已用完预读的缓存块，继续预读
            bc->prereadFun(bc, block);
        }

        return ENOERR;
    }

    D(("bcache block = %llu NOT found in cache\n", num));

	//申请新的缓存块
    block = AllocNewBlock(bc, readData, num);
    if (block == NULL) {
		//换一种算法继续申请
        block = GetSlowBlock(bc, readData);
    }

    BlockInit(bc, block, num); //初始化申请到的缓存块

    if (readData == TRUE) {
        D(("bcache reading block = %llu\n", block->num));

		//先将数据从磁盘读入缓存块
        ret = BlockRead(bc, block, block->data);
        if (ret != ENOERR) {
            return ret;
        }
		//然后再多读一点(预读)
        if (bc->prereadFun != NULL) {
            bc->prereadFun(bc, block);
        }
    }

	//将缓存块加入本缓存子系统
    AddBlock(bc, block);

    *dblock = block; //记录下缓存块
    return ENOERR;
}


//初始化块缓存子系统
static INT32 BcacheInitCache(OsBcache *bc,
                             UINT8 *memStart,
                             UINT32 memSize,
                             UINT32 blockSize)
{
    UINT8 *blockMem = NULL;
    UINT8 *dataMem = NULL;
    OsBcacheBlock *block = NULL;
    UINT32 blockNum, i;

    LOS_ListInit(&bc->listHead);  //初始化LRU链表
    LOS_ListInit(&bc->numHead);  //初始化序号排列的缓存表
    bc->sumNum = 0;  //正在使用的缓存块序号之和
    bc->nBlock = 0;  //正在使用的缓存块数目

    if (!GetValLog2(blockSize)) {
        PRINT_ERR("GetValLog2(%u) return 0.\n", blockSize);
        return -EINVAL; //缓存块尺寸不合理
    }

    bc->rbRoot.rb_node = NULL;  //红黑树开始时为空
    bc->memStart = memStart;    //块缓存子系统存储区首地址
    bc->blockSize = blockSize;  //每个块缓存的尺寸
    bc->blockSizeLog2 = GetValLog2(blockSize); //块缓存尺寸的对数(2为底)
    bc->modifiedBlock = 0;      //数据发生了变化，还没有写磁盘的缓存块数目

    /* init block memory pool */
    LOS_ListInit(&bc->freeListHead); //空闲缓存块链表

	//缓存块容量为缓存区总容量除以每个缓存块占用量(头部+数据部分)
	//缓存块数据区首地址需要按DMA对齐要求，所有也要考虑
    blockNum = (memSize - DMA_ALLGN) / (sizeof(OsBcacheBlock) + bc->blockSize);
    blockMem = bc->memStart;  //缓存区起始地址
    dataMem = blockMem + (sizeof(OsBcacheBlock) * blockNum); //先存储所有的缓存块头
    dataMem += ALIGN_DISP((UINTPTR)dataMem); //然后存储所有的缓存块数据， 按DMA要求对齐

    for (i = 0; i < blockNum; i++) {
		//初始化每个缓存块头部
        block = (OsBcacheBlock *)(VOID *)blockMem;
        block->data = dataMem;  //绑定相应的数据区
        //fatfs最开始几个数据块只用来做读操作
        block->readBuff = (i < CONFIG_FS_FAT_READ_NUMS) ? TRUE : FALSE;

        if (i == CONFIG_FS_FAT_READ_NUMS) {
            bc->wStart = block;  //写操作的起始块
        }

        LOS_ListAdd(&bc->freeListHead, &block->listNode);  //每个块缓存初始时都是空闲的

        blockMem += sizeof(OsBcacheBlock);  //下一个缓存块头部
        dataMem += bc->blockSize; //下一个缓存块数据区
    }

    bc->wEnd = block;   //最后一个缓存块之后

    return ENOERR;
}

//从磁盘读出数据
static INT32 DrvBread(struct inode *priv, UINT8 *buf, UINT32 len, UINT64 pos)
{
    INT32 ret = priv->u.i_bops->read(priv, buf, pos, len);
    if (ret != (INT32)len) {
        PRINT_ERR("%s failure\n", __FUNCTION__);
        return ret;
    }
    return ENOERR;
}

//将数据写入磁盘
static INT32 DrvBwrite(struct inode *priv, const UINT8 *buf, UINT32 len, UINT64 pos)
{
    INT32 ret = priv->u.i_bops->write(priv, buf, pos, len);
    if (ret != (INT32)len) {
        PRINT_ERR("%s failure\n", __FUNCTION__);
        return ret;
    }
    return ENOERR;
}

//创建块缓存驱动
INT32 BlockCacheDrvCreate(VOID *handle,
                          UINT8 *memStart,
                          UINT32 memSize,
                          UINT32 blockSize,
                          OsBcache *bc)
{
    INT32 ret;
    bc->priv = handle;  //此缓存系统对应的设备文件
    bc->breadFun = DrvBread; //磁盘读取函数
    bc->bwriteFun = DrvBwrite; //磁盘写入函数

	//初始化块缓存内存池
    ret = BcacheInitCache(bc, memStart, memSize, blockSize);
    if (ret != ENOERR) {
        return ret;
    }

	//初始化互斥锁
    if (pthread_mutex_init(&bc->bcacheMutex, NULL) != ENOERR) {
        return VFS_ERROR;
    }
    bc->bcacheMutex.attr.type = PTHREAD_MUTEX_RECURSIVE; //可嵌套使用互斥锁

    return ENOERR;
}


//从块缓存读取数据到用户空间
INT32 BlockCacheRead(OsBcache *bc, UINT8 *buf, UINT32 *len, UINT64 sector)
{
    OsBcacheBlock *block = NULL;
    UINT8 *tempBuf = buf;
    UINT32 size;
    UINT32 currentSize;
    INT32 ret = ENOERR;
    UINT64 pos;
    UINT64 num;

    if (bc == NULL || buf == NULL || len == NULL) {
        return -EPERM;
    }

    size = *len;  //需要读入的字节数
    pos = sector * bc->sectorSize; //磁盘中读数据起始位置
    num = pos >> bc->blockSizeLog2; //磁盘块编号
    pos = pos & (bc->blockSize - 1); //磁盘块编号

    while (size > 0) { //还有数据需要读取
    	//只能以块为单位读取数据，不能跨块读取
        if ((size + pos) > bc->blockSize) {
			//本块最多还能读取的数据(不是最后一块)
            currentSize = bc->blockSize - (UINT32)pos;
        } else {
        	//最后需要读入的数据(最后一块)
            currentSize = size;
        }

        (VOID)pthread_mutex_lock(&bc->bcacheMutex);

		//获取块缓存用于数据读取
        ret = BcacheGetBlock(bc, num, TRUE, &block); 
        if (ret != ENOERR) {
            (VOID)pthread_mutex_unlock(&bc->bcacheMutex);
            break;
        }

        if ((block->readFlag == FALSE) && (block->modified == TRUE)) {
			//如果本块还没有读入数据，且本块原来有脏数据
			//那么只能将数据读入不是脏数据的扇区
            ret = BcacheGetFlag(bc, block);
            if (ret != ENOERR) {
                (VOID)pthread_mutex_unlock(&bc->bcacheMutex);
                return ret;
            }
        } else if ((block->readFlag == FALSE) && (block->modified == FALSE)) {
        	//还没有从磁盘读入块缓存，没有脏数据需要写磁盘
        	//那么先从磁盘读入数据到块缓存中
            ret = BlockRead(bc, block, block->data);
            if (ret != ENOERR) {
                (VOID)pthread_mutex_unlock(&bc->bcacheMutex);
                return ret;
            }
        }

		// else  如果块缓存中已经有和磁盘一致的数据，那么直接从块缓存获取数据，提升效率

		//将块缓存的数据拷贝到用户空间
        if (LOS_CopyFromKernel((VOID *)tempBuf, size, (VOID *)(block->data + pos), currentSize) != EOK) {
            (VOID)pthread_mutex_unlock(&bc->bcacheMutex);
            return VFS_ERROR;
        }

        (VOID)pthread_mutex_unlock(&bc->bcacheMutex);

        tempBuf += currentSize;  //用户空间剩余存放数据位置
        size -= currentSize;     //剩余需要读入的尺寸
        pos = 0;                 //从第2个块缓存开始，都从块缓存开始位置读
        num++;                   //读下一个缓存块
    }
    *len -= size;  //记录已经读入的字节数
    return ret;
}


//向磁盘写入数据，先向块缓存写入数据，并标记块缓存为脏
INT32 BlockCacheWrite(OsBcache *bc, const UINT8 *buf, UINT32 *len, UINT64 sector)
{
    OsBcacheBlock *block = NULL;
    const UINT8 *tempBuf = buf;
    UINT32 size = *len;
    INT32 ret = ENOERR;
    UINT32 currentSize;
    UINT64 pos;
    UINT64 num;

    pos = sector * bc->sectorSize;  //磁盘中写入数据的起始位置
    num = pos >> bc->blockSizeLog2; //起始磁盘块号
    pos = pos & (bc->blockSize - 1); //起始磁盘块内的起始偏移位置

    D(("bcache write len = %u pos = %llu bnum = %llu\n", *len, pos, num));

    while (size > 0) {
        if ((size + pos) > bc->blockSize) {
			//每次最多向一个缓存块写入数据
            currentSize = bc->blockSize - (UINT32)pos;
        } else {
        	//需要写入的最后一个缓存块
            currentSize = size;
        }

        (VOID)pthread_mutex_lock(&bc->bcacheMutex);
		//获取用于写操作的缓存块
        ret = BcacheGetBlock(bc, num, FALSE, &block);
        if (ret != ENOERR) {
            (VOID)pthread_mutex_unlock(&bc->bcacheMutex);
            break;
        }

		//将数据从用户空间拷贝到缓存块中
        if (LOS_CopyToKernel((VOID *)(block->data + pos), bc->blockSize - (UINT32)pos,
            (VOID *)tempBuf, currentSize) != EOK) {
            (VOID)pthread_mutex_unlock(&bc->bcacheMutex);
            return VFS_ERROR;
        }
        if (block->modified == FALSE) {
            block->modified = TRUE;  //标记缓存块已写入用户数据
            bc->modifiedBlock++;     //带写入磁盘的缓存块数目增加
        }
        if ((pos == 0) && (currentSize == bc->blockSize)) {
			//整个缓存块都写入了数据，那么所有扇区都标记成脏
            memset_s(block->flag, sizeof(block->flag), 0xFF, sizeof(block->flag));
            block->allDirty = TRUE;  //所有扇区脏的另一种表示
        } else {
			//标记部分扇区为脏的扇区(实际写入数据的扇区)
            BcacheSetFlag(bc, block, (UINT32)pos, currentSize);
        }
        (VOID)pthread_mutex_unlock(&bc->bcacheMutex);

        tempBuf += currentSize;  //下一个缓存块数据来源
        size -= currentSize;     //剩余需要写入的数据尺寸
        pos = 0;                 //第2个缓存块开始，从缓存块开始位置写
        num++;                   //下一个磁盘块
    }
    *len -= size;   //记录成功写入的字节数
    return ret;
}

//块缓存数据同步到磁盘
INT32 BlockCacheSync(OsBcache *bc)
{
    return BcacheSync(bc);
}

//向目标磁盘同步数据
INT32 OsSdSync(INT32 id)
{
#ifdef LOSCFG_FS_FAT_CACHE
    INT32 ret;
    los_disk *disk = get_disk(id);  //根据ID获得磁盘描述符
    if (disk == NULL) {
        return VFS_ERROR;
    }

    if (pthread_mutex_lock(&disk->disk_mutex) != ENOERR) {
        PRINT_ERR("%s %d, mutex lock fail!\n", __FUNCTION__, __LINE__);
        return VFS_ERROR;
    }
    if ((disk->disk_status == STAT_INUSED) && (disk->bcache != NULL)) {
		//本磁盘正在使用，且具有缓存机制，那么将缓存数据同步到磁盘
        ret = BcacheSync(disk->bcache);
    } else {
        ret = VFS_ERROR;
    }
    if (pthread_mutex_unlock(&disk->disk_mutex) != ENOERR) {
        PRINT_ERR("%s %d, mutex unlock fail!\n", __FUNCTION__, __LINE__);
        return VFS_ERROR;
    }
    return ret;
#else
    return VFS_ERROR;
#endif
}

//同步数据到磁盘
INT32 LOS_BcacheSyncByName(const CHAR *name)
{
    INT32 diskID = los_get_diskid_byname(name); //根据磁盘名获得磁盘ID
    return OsSdSync(diskID); //同步数据到磁盘
}

//获取磁盘块缓存系统中脏数据比率
INT32 BcacheGetDirtyRatio(INT32 id)
{
#ifdef LOSCFG_FS_FAT_CACHE
    INT32 ret;
    los_disk *disk = get_disk(id); //获取磁盘描述符
    if (disk == NULL) {
        return VFS_ERROR;
    }

    if (pthread_mutex_lock(&disk->disk_mutex) != ENOERR) {
        PRINT_ERR("%s %d, mutex lock fail!\n", __FUNCTION__, __LINE__);
        return VFS_ERROR;
    }
    if ((disk->disk_status == STAT_INUSED) && (disk->bcache != NULL)) {
		//计算已修订磁盘块占总磁盘块的百分比
        ret = (INT32)((disk->bcache->modifiedBlock * PERCENTAGE) / GetFatBlockNums());
    } else {
        ret = VFS_ERROR;
    }
    if (pthread_mutex_unlock(&disk->disk_mutex) != ENOERR) {
        PRINT_ERR("%s %d, mutex unlock fail!\n", __FUNCTION__, __LINE__);
        return VFS_ERROR;
    }
    return ret;
#else
    return VFS_ERROR;
#endif
}

//根据磁盘名称获取脏数据块比率
INT32 LOS_GetDirtyRatioByName(const CHAR *name)
{
    INT32 diskID = los_get_diskid_byname(name);
    return BcacheGetDirtyRatio(diskID);
}

#ifdef LOSCFG_FS_FAT_CACHE_SYNC_THREAD
//磁盘块脏数据同步线程
static VOID BcacheSyncThread(UINT32 id)
{
    INT32 diskID = (INT32)id;
    INT32 dirtyRatio;
    while (1) {
        dirtyRatio = BcacheGetDirtyRatio(diskID);
        if (dirtyRatio > (INT32)g_dirtyRatio) {
			//当脏数据比率超过阈值时，触发同步
            (VOID)OsSdSync(diskID);
        }
        msleep(g_syncInterval);  //周期检查脏数据比率
    }
}

//脏数据同步线程创建
VOID BcacheSyncThreadInit(OsBcache *bc, INT32 id)
{
    UINT32 ret;
    TSK_INIT_PARAM_S appTask;

    (VOID)memset_s(&appTask, sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    appTask.pfnTaskEntry = (TSK_ENTRY_FUNC)BcacheSyncThread;
    appTask.uwStackSize = BCACHE_STATCK_SIZE;
    appTask.pcName = "bcache_sync_task";
    appTask.usTaskPrio = g_syncThreadPrio;
    appTask.auwArgs[0] = (UINTPTR)id;  //磁盘块ID
    appTask.uwResved = LOS_TASK_STATUS_DETACHED;
    ret = LOS_TaskCreate(&bc->syncTaskId, &appTask);
    if (ret != ENOERR) {
        PRINT_ERR("Bcache sync task create failed in %s, %d\n", __FUNCTION__, __LINE__);
    }
}

//删除脏数据同步线程
VOID BcacheSyncThreadDeinit(const OsBcache *bc)
{
    if (bc != NULL) {
        if (LOS_TaskDelete(bc->syncTaskId) != ENOERR) {
            PRINT_ERR("Bcache sync task delete failed in %s, %d\n", __FUNCTION__, __LINE__);
        }
    }
}
#endif

//块设备缓存子系统初始化
OsBcache *BlockCacheInit(struct inode *devNode, UINT32 sectorSize, UINT32 sectorPerBlock,
                         UINT32 blockNum, UINT64 blockCount)
{
    OsBcache *bcache = NULL;
    struct inode *blkDriver = devNode;
    UINT8 *bcacheMem = NULL;
    UINT8 *rwBuffer = NULL;
    UINT32 blockSize, memSize;

    if ((blkDriver == NULL) || (sectorSize * sectorPerBlock * blockNum == 0) || (blockCount == 0)) {
        return NULL;
    }

    blockSize = sectorSize * sectorPerBlock;  //磁盘块和缓存块尺寸 = 扇区尺寸 * 每磁盘扇区数
    if ((((UINT64)(sizeof(OsBcacheBlock) + blockSize) * blockNum) + DMA_ALLGN) > UINT_MAX) {
		//磁盘太大，系统不支持
        return NULL;
    }
	//所有缓存块需要占用的空间(含头部和数据)
    memSize = ((sizeof(OsBcacheBlock) + blockSize) * blockNum) + DMA_ALLGN;

	//缓存管理控制头
    bcache = (OsBcache *)zalloc(sizeof(OsBcache));
    if (bcache == NULL) {
        PRINT_ERR("bcache_init : malloc %u Bytes failed!\n", sizeof(OsBcache));
        return NULL;
    }

	//设置磁盘同步处理函数
    set_sd_sync_fn(OsSdSync);

	//申请所有缓存块占用的内存
    bcacheMem = (UINT8 *)zalloc(memSize);
    if (bcacheMem == NULL) {
        PRINT_ERR("bcache_init : malloc %u Bytes failed!\n", memSize);
        goto ERROR_OUT_WITH_BCACHE;
    }

	//额外申请一个块缓存内存，用于向脏缓存块读入数据前临时存放数据
    rwBuffer = (UINT8 *)memalign(DMA_ALLGN, blockSize);
    if (rwBuffer == NULL) {
        PRINT_ERR("bcache_init : malloc %u Bytes failed!\n", blockSize);
        goto ERROR_OUT_WITH_MEM;
    }

	//记录上述字段
    bcache->rwBuffer = rwBuffer;
    bcache->sectorSize = sectorSize;
    bcache->sectorPerBlock = sectorPerBlock;
    bcache->blockCount = blockCount;  //磁盘块数目

	//并初始化各缓存块
    if (BlockCacheDrvCreate(blkDriver, bcacheMem, memSize, blockSize, bcache) != ENOERR) {
        goto ERROR_OUT_WITH_BUFFER;
    }

    return bcache;

ERROR_OUT_WITH_BUFFER:
    free(rwBuffer);
ERROR_OUT_WITH_MEM:
    free(bcacheMem);
ERROR_OUT_WITH_BCACHE:
    free(bcache);
    return NULL;
}

//释放块缓存资源
VOID BlockCacheDeinit(OsBcache *bcache)
{
    if (bcache != NULL) {
        (VOID)pthread_mutex_destroy(&bcache->bcacheMutex);
        free(bcache->memStart);  //所有缓存块内存
        bcache->memStart = NULL;
        free(bcache->rwBuffer);  //辅助缓存块内存
        bcache->rwBuffer = NULL;
        free(bcache);   //缓存管理头部
    }
}


//异步预读磁盘到缓存块线程
static VOID BcacheAsyncPrereadThread(VOID *arg)
{
    OsBcache *bc = (OsBcache *)arg;
    OsBcacheBlock *block = NULL;
    INT32 ret;
    UINT32 i;

    for (;;) {
		//接收需要异步读的事件
        ret = (INT32)LOS_EventRead(&bc->bcacheEvent, PREREAD_EVENT_MASK,
                                   LOS_WAITMODE_OR | LOS_WAITMODE_CLR, LOS_WAIT_FOREVER);
        if (ret != ASYNC_EVENT_BIT) {
            PRINT_ERR("The event read in %s, %d is error!!!\n", __FUNCTION__, __LINE__);
            continue;
        }

		//只处理ASYNC_EVENT_BIT事件，每次预读2个缓存块
        for (i = 1; i <= PREREAD_BLOCK_NUM; i++) {
            if ((bc->curBlockNum + i) >= bc->blockCount) {
                break;  //没有磁盘块可读了
            }

            (VOID)pthread_mutex_lock(&bc->bcacheMutex);
			//获取缓存块，并将指定磁盘块数据读入缓存块
            ret = BcacheGetBlock(bc, bc->curBlockNum + i, TRUE, &block);
            if (ret != ENOERR) {
                PRINT_ERR("read block %llu error : %d!\n", bc->curBlockNum, ret);
            }

            (VOID)pthread_mutex_unlock(&bc->bcacheMutex);
        }

        if (block != NULL) {
			//本缓存块是最后一个预读的磁盘块数据
            block->pgHit = 1; /* preread complete */
        }
    }
}

//唤醒预读线程
VOID ResumeAsyncPreread(OsBcache *arg1, const OsBcacheBlock *arg2)
{
    UINT32 ret;
    OsBcache *bc = arg1;
    const OsBcacheBlock *block = arg2;

    if (OsCurrTaskGet()->taskID != bc->prereadTaskId) {
		//当前线程不是预读线程
        bc->curBlockNum = block->num;  //记录当前正在读的磁盘块号
        ret = LOS_EventWrite(&bc->bcacheEvent, ASYNC_EVENT_BIT); //然后唤醒预读线程
        if (ret != ENOERR) {
            PRINT_ERR("Write event failed in %s, %d\n", __FUNCTION__, __LINE__);
        }
    }
}

//预读模块初始化
UINT32 BcacheAsyncPrereadInit(OsBcache *bc)
{
    UINT32 ret;
    TSK_INIT_PARAM_S appTask;

    ret = LOS_EventInit(&bc->bcacheEvent);  //创建预读事件
    if (ret != ENOERR) {
        PRINT_ERR("Async event init failed in %s, %d\n", __FUNCTION__, __LINE__);
        return ret;
    }

	//创建预读线程
    (VOID)memset_s(&appTask, sizeof(TSK_INIT_PARAM_S), 0, sizeof(TSK_INIT_PARAM_S));
    appTask.pfnTaskEntry = (TSK_ENTRY_FUNC)BcacheAsyncPrereadThread;
    appTask.uwStackSize = BCACHE_STATCK_SIZE;
    appTask.pcName = "bcache_async_task";
    appTask.usTaskPrio = BCACHE_PREREAD_PRIO;
    appTask.auwArgs[0] = (UINTPTR)bc;
    appTask.uwResved = LOS_TASK_STATUS_DETACHED;
    ret = LOS_TaskCreate(&bc->prereadTaskId, &appTask);
    if (ret != ENOERR) {
        PRINT_ERR("Bcache async task create failed in %s, %d\n", __FUNCTION__, __LINE__);
    }

    return ret;
}

//预读模块删除
UINT32 BcacheAsyncPrereadDeinit(OsBcache *bc)
{
    UINT32 ret = LOS_NOK;

    if (bc != NULL) {
		//删除预读线程
        ret = LOS_TaskDelete(bc->prereadTaskId);
        if (ret != ENOERR) {
            PRINT_ERR("Bcache async task delete failed in %s, %d\n", __FUNCTION__, __LINE__);
        }

		//删除预读事件
        ret = LOS_EventDestroy(&bc->bcacheEvent);
        if (ret != ENOERR) {
            PRINT_ERR("Async event destroy failed in %s, %d\n", __FUNCTION__, __LINE__);
            return ret;
        }
    }

    return ret;
}
