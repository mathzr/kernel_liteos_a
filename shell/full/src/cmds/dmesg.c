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

/*
   +-------------------------------------------------------+
   | Info |          log_space                             |
   +-------------------------------------------------------+
   |
   |__buffer_space

Case A:
   +-------------------------------------------------------+
   |           |#############################|             |
   +-------------------------------------------------------+
               |                             |
              Head                           Tail
Case B:
   +-------------------------------------------------------+
   |##########|                                    |#######|
   +-------------------------------------------------------+
              |                                    |
              Tail                                 Head
*/

#include "sys_config.h"
#ifdef LOSCFG_SHELL_DMESG
#include "dmesg_pri.h"
#include "show.h"
#include "shcmd.h"
#include "securec.h"
#include "unistd.h"
#include "stdlib.h"
#include "los_task.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define BUF_MAX_INDEX (g_logBufSize - 1)

//保护内核启动过程使用的日志缓冲区对应的自旋锁
LITE_OS_SEC_BSS STATIC SPIN_LOCK_INIT(g_dmesgSpin);

//日志缓冲区描述符
STATIC DmesgInfo *g_dmesgInfo = NULL;
//缓冲区尺寸
STATIC UINT32 g_logBufSize = 0;
//缓冲区内存地址(含描述符)
STATIC VOID *g_mallocAddr = NULL;
//日志等级
STATIC UINT32 g_dmesgLogLevel = 3;
//控制台锁
STATIC UINT32 g_consoleLock = 0;
//串口锁
STATIC UINT32 g_uartLock = 0;
//日志等级字符串
STATIC const CHAR *g_levelString[] = {
    "EMG",
    "COMMON",
    "ERR",
    "WARN",
    "INFO",
    "DEBUG"
};

//锁住控制台
STATIC VOID OsLockConsole(VOID)
{
    g_consoleLock = 1;
}

//解锁控制台
STATIC VOID OsUnlockConsole(VOID)
{
    g_consoleLock = 0;
}

//锁住串口
STATIC VOID OsLockUart(VOID)
{
    g_uartLock = 1;
}

//解锁串口
STATIC VOID OsUnlockUart(VOID)
{
    g_uartLock = 0;
}

//检查错误
STATIC UINT32 OsCheckError(VOID)
{
    if (g_dmesgInfo == NULL) {
        return LOS_NOK; //无内核启动日志描述符
    }

    if (g_dmesgInfo->logSize > g_logBufSize) {
        return LOS_NOK; //日志尺寸过大
    }

    if (((g_dmesgInfo->logSize == g_logBufSize) || (g_dmesgInfo->logSize == 0)) &&
        (g_dmesgInfo->logTail != g_dmesgInfo->logHead)) {
        return LOS_NOK; //尺寸满或空，但日志头尾不相等
    }

    return LOS_OK;
}

//从日志缓冲区中读出日志
STATIC INT32 OsDmesgRead(CHAR *buf, UINT32 len)
{
    UINT32 readLen;
    UINT32 logSize = g_dmesgInfo->logSize;
    UINT32 head = g_dmesgInfo->logHead;
    UINT32 tail = g_dmesgInfo->logTail;
    CHAR *logBuf = g_dmesgInfo->logBuf;
    errno_t ret;

    if (OsCheckError()) { //检查缓冲区完整性
        return -1;
    }
    if (logSize == 0) {
        return 0; //没有数据可读
    }

	//尽可能多的从缓冲区中读出日志
    readLen = len < logSize ? len : logSize;

    if (head < tail) { /* Case A */ //有效数据在缓冲区中部
        ret = memcpy_s(buf, len, logBuf + head, readLen); //只需要拷贝一次数据
        if (ret != EOK) {
            return -1;
        }
        g_dmesgInfo->logHead += readLen;  //移动下一次读取日志的位置
        g_dmesgInfo->logSize -= readLen;  //调整缓冲区中剩余日志的尺寸
    } else { /* Case B */ //有效数据在缓冲区的两端
        if (readLen <= (g_logBufSize - head)) {
			//需要读取的尺寸不超过尾部数据区
            ret = memcpy_s(buf, len, logBuf + head, readLen); //只读一次就可以了
            if (ret != EOK) {
                return -1;
            }
            g_dmesgInfo->logHead += readLen; //下一次读取的位置
            g_dmesgInfo->logSize -= readLen; //剩余日志尺寸
        } else {
			//需要读取2次，先把尾部读完
            ret = memcpy_s(buf, len, logBuf + head, g_logBufSize - head);
            if (ret != EOK) {
                return -1;
            }

			//再读取首部的部分数据
            ret = memcpy_s(buf + g_logBufSize - head, len - (g_logBufSize - head),
                           logBuf, readLen - (g_logBufSize - head));
            if (ret != EOK) {
                return -1;
            }
			//调整下一次读取数据的起始位置
            g_dmesgInfo->logHead = readLen - (g_logBufSize - head);
            g_dmesgInfo->logSize -= readLen; //剩余的日志尺寸
        }
    }
    return (INT32)readLen;
}

//将日志缓冲区中的数据拷贝到一个新的缓冲区
STATIC INT32 OsCopyToNew(const VOID *addr, UINT32 size) 
{
    UINT32 copyStart = 0;
    UINT32 copyLen;
    CHAR *temp = NULL;
    CHAR *newBuf = (CHAR *)addr + sizeof(DmesgInfo); //新缓冲区包含控制头
    UINT32 bufSize = size - sizeof(DmesgInfo); //新缓冲区数据区尺寸
    INT32 ret;

    if (g_dmesgInfo->logSize == 0) {
        return 0; //没有日志数据
    }

    temp = (CHAR *)malloc(g_dmesgInfo->logSize);  //申请临时缓存用于读取志数据
    if (temp == NULL) {
        return -1;
    }

    (VOID)memset_s(temp, g_dmesgInfo->logSize, 0, g_dmesgInfo->logSize);
	//计算需要拷贝的数据长度，不能超过新缓冲区数据区长度
    copyLen = ((bufSize < g_dmesgInfo->logSize) ? bufSize : g_dmesgInfo->logSize);
    if (bufSize < g_dmesgInfo->logSize) {
		//如果新缓冲区数据区长度较小，则拷贝最新的日志，最老的日志要舍弃
        copyStart = g_dmesgInfo->logSize - bufSize;
    }

	//完整读取日志数据
    ret = OsDmesgRead(temp, g_dmesgInfo->logSize);
    if (ret <= 0) {
        PRINT_ERR("%s,%d failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
        free(temp);
        return -1;
    }

    /* if new buf size smaller than logSize */
	//拷贝完整或部分日志数据到新缓冲区
    ret = memcpy_s(newBuf, bufSize, temp + copyStart, copyLen);
    if (ret != EOK) {
        PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
        free(temp);
        return -1;
    }
    free(temp);  //释放辅助日志读取的临时内存

    return (INT32)copyLen;
}

//重新设置日志缓冲区，并将旧缓冲区中的日志拷贝到新缓冲区中
STATIC UINT32 OsDmesgResetMem(const VOID *addr, UINT32 size)
{
    VOID *temp = NULL;
    INT32 copyLen;
    UINT32 intSave;

    if (size <= sizeof(DmesgInfo)) {
        return LOS_NOK; //新缓冲区没有办法存储日志数据
    }

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    temp = g_dmesgInfo;
	//将日志数据拷贝到新缓冲区
    copyLen = OsCopyToNew(addr, size);
    if (copyLen < 0) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        return LOS_NOK;
    }

	//根据新缓冲区重新构造控制头
    g_logBufSize = size - sizeof(DmesgInfo);
    g_dmesgInfo = (DmesgInfo *)addr;
    g_dmesgInfo->logBuf = (CHAR *)addr + sizeof(DmesgInfo);
    g_dmesgInfo->logSize = copyLen;
    g_dmesgInfo->logTail = ((copyLen == g_logBufSize) ? 0 : copyLen);
    g_dmesgInfo->logHead = 0;

    /* if old mem came from malloc */
    if (temp == g_mallocAddr) {
        g_mallocAddr = NULL;
        free(temp);  //释放旧日志区
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);

    return LOS_OK;
}

//调整日志缓冲区尺寸，内部逻辑与上一个函数类似
//不过缓冲区由malloc申请
STATIC UINT32 OsDmesgChangeSize(UINT32 size)
{
    VOID *temp = NULL;
    INT32 copyLen;
    CHAR *newString = NULL;
    UINT32 intSave;

    if (size == 0) {
        return LOS_NOK;
    }

	//新日志缓冲区(含控制头
    newString = (CHAR *)malloc(size + sizeof(DmesgInfo));
    if (newString == NULL) {
        return LOS_NOK;
    }

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    temp = g_dmesgInfo;

    copyLen = OsCopyToNew(newString, size + sizeof(DmesgInfo));
    if (copyLen < 0) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        free(newString);
        return LOS_NOK;
    }

    g_logBufSize = size;
    g_dmesgInfo = (DmesgInfo *)newString;
    g_dmesgInfo->logBuf = (CHAR *)newString + sizeof(DmesgInfo);
    g_dmesgInfo->logSize = copyLen;
    g_dmesgInfo->logTail = ((copyLen == g_logBufSize) ? 0 : copyLen);
    g_dmesgInfo->logHead = 0;

    if (temp == g_mallocAddr) {
        g_mallocAddr = NULL;
        free(temp);
    }
    g_mallocAddr = newString;
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);

    return LOS_OK;
}

//控制台是否锁住
UINT32 OsCheckConsoleLock(VOID)
{
    return g_consoleLock;
}

//串口是否锁住
UINT32 OsCheckUartLock(VOID)
{
    return g_uartLock;
}

//初始化dmesg缓冲区，本缓冲区主要用于记录启机阶段的调试信息
UINT32 OsDmesgInit(VOID)
{
    CHAR* buffer = NULL;

    buffer = (CHAR *)malloc(KERNEL_LOG_BUF_SIZE + sizeof(DmesgInfo));
    if (buffer == NULL) {
        return LOS_NOK;
    }
    g_mallocAddr = buffer;
    g_dmesgInfo = (DmesgInfo *)buffer;
    g_dmesgInfo->logHead = 0; //日志数据的起始位置
    g_dmesgInfo->logTail = 0; //下一条日志的写入位置
    g_dmesgInfo->logSize = 0; //剩余还未读取的日志存储空间大小
    g_dmesgInfo->logBuf = buffer + sizeof(DmesgInfo); //日志缓冲区首地址
    g_logBufSize = KERNEL_LOG_BUF_SIZE; //日志缓冲区长度

    return LOS_OK;
}

//向日志缓冲区写入一个字符
STATIC CHAR OsLogRecordChar(CHAR c)
{
	//写入字符，并增加下一次需要写入的偏移值
    *(g_dmesgInfo->logBuf + g_dmesgInfo->logTail++) = c;

    if (g_dmesgInfo->logTail > BUF_MAX_INDEX) {
        g_dmesgInfo->logTail = 0; //到达缓冲区末尾，下次从首部开始写入
    }

    if (g_dmesgInfo->logSize < g_logBufSize) {
        (g_dmesgInfo->logSize)++; //日志尺寸增加
    } else {
    	//日志满，下次写入位置和读取位置一致
        g_dmesgInfo->logHead = g_dmesgInfo->logTail;
    }
    return c; //返回刚写入的字符
}

//向日志缓冲区写入字符串
UINT32 OsLogRecordStr(const CHAR *str, UINT32 len)
{
    UINT32 i = 0;
    UINTPTR intSave;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    while (len--) {  //遍历字符串
    	//依次写入每一个字符
        (VOID)OsLogRecordChar(str[i]);
        i++;  //不担心字符串过长，因为满了以后会循环覆盖
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
    return i; //返回写入的字符串长度，即len
}

//当前缓冲区已满，继续写入日志的函数
STATIC VOID OsBufFullWrite(const CHAR *dst, UINT32 logLen)
{
    UINT32 bufSize = g_logBufSize;
    UINT32 tail = g_dmesgInfo->logTail;
    CHAR *buf = g_dmesgInfo->logBuf;
    errno_t ret;

    if (!logLen || (dst == NULL)) {
        return;
    }
    if (logLen > bufSize) { /* full re-write */
		//需要写入的尺寸超过了缓冲区总长度，那么至少要分2次写入
		//第一次写满缓冲区尾部
        ret = memcpy_s(buf + tail, bufSize - tail, dst, bufSize - tail);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }
		//第二次从头开始写入，并填满缓冲区
        ret = memcpy_s(buf, bufSize, dst + bufSize - tail, tail);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//还有数据需要写入，递归调用
        OsBufFullWrite(dst + bufSize, logLen - bufSize);
    } else {
		//剩余数据不会或刚好充满缓冲区的情况
        if (logLen > (bufSize - tail)) { /* need cycle back to start */
			//但是尾部空闲部分不够写入，还是要分2次写入
			//先写满尾部
            ret = memcpy_s(buf + tail, bufSize - tail, dst, bufSize - tail);
            if (ret != EOK) {
                PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
                return;
            }
			//然后剩余数据写入头部
            ret = memcpy_s(buf, bufSize, dst + bufSize - tail, logLen - (bufSize - tail));
            if (ret != EOK) {
                PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
                return;
            }

			//记录下有效日志数据区的位置
            g_dmesgInfo->logTail = logLen - (bufSize - tail);
			//缓冲区满状态，所以head和tail保持一致
            g_dmesgInfo->logHead = g_dmesgInfo->logTail;
        } else { /* no need cycle back to start */
            ret = memcpy_s(buf + tail, bufSize - tail, dst, logLen);
            if (ret != EOK) {
                PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
                return;
            }
            g_dmesgInfo->logTail += logLen;
            if (g_dmesgInfo->logTail > BUF_MAX_INDEX) {
                g_dmesgInfo->logTail = 0;
            }
			//调用者会保证把缓冲区写满
            g_dmesgInfo->logHead = g_dmesgInfo->logTail;
        }
    }
}


//tail在前，head在后的情况下，写入日志数据
STATIC VOID OsWriteTailToHead(const CHAR *dst, UINT32 logLen)
{
    UINT32 writeLen = 0;
    UINT32 bufSize = g_logBufSize;
    UINT32 logSize = g_dmesgInfo->logSize;
    UINT32 tail = g_dmesgInfo->logTail;
    CHAR *buf = g_dmesgInfo->logBuf;
    errno_t ret;

    if ((!logLen) || (dst == NULL)) {
        return;
    }
    if (logLen > (bufSize - logSize)) { /* space-need > space-remain */
		//需要写入的数据超过了剩余的缓冲区
        writeLen = bufSize - logSize; //先将缓冲区填满
        ret = memcpy_s(buf + tail, bufSize - tail, dst, writeLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//此时，tail就追上head了
        g_dmesgInfo->logTail = g_dmesgInfo->logHead;
        g_dmesgInfo->logSize = g_logBufSize; //缓冲区填满了
        OsBufFullWrite(dst + writeLen, logLen - writeLen); //缓冲区已满情况下，继续写入剩余的日志
    } else {
		//需要写入的数据不会超过剩余缓冲区
        ret = memcpy_s(buf + tail, bufSize - tail, dst, logLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//正常调整tail和size即可
        g_dmesgInfo->logTail += logLen;
        g_dmesgInfo->logSize += logLen;
    }
}

//tail在head之后的情况，先从tail往后写数据，可能还会继续在缓冲区头部写数据
STATIC VOID OsWriteTailToEnd(const CHAR *dst, UINT32 logLen)
{
    UINT32 writeLen;
    UINT32 bufSize = g_logBufSize;
    UINT32 tail = g_dmesgInfo->logTail;
    CHAR *buf = g_dmesgInfo->logBuf;
    errno_t ret;

    if ((!logLen) || (dst == NULL)) {
        return;
    }
    if (logLen >= (bufSize - tail)) { /* need cycle to start ,then became B */
		//数据能填满缓冲区尾部的情况
        writeLen = bufSize - tail;
		//先填满缓冲区尾部
        ret = memcpy_s(buf + tail, writeLen, dst, writeLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

        g_dmesgInfo->logSize += writeLen; //日志尺寸正常更新
        g_dmesgInfo->logTail = 0;  //从头部开始继续写日志数据
        if (g_dmesgInfo->logSize == g_logBufSize) { /* Tail = Head is 0 */
			//缓冲区已满，继续写入剩余的日志数据
            OsBufFullWrite(dst + writeLen, logLen - writeLen);
        } else {
        	//此时tail一定小于head，日志写入方向是tailtoend, 继续写剩余日志
            OsWriteTailToHead(dst + writeLen, logLen - writeLen);
        }
    } else { /* just do serial copy */
    	//剩余日志不会填满缓冲区尾部
        ret = memcpy_s(buf + tail, bufSize - tail, dst, logLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//正常更新缓冲区
        g_dmesgInfo->logTail += logLen;
        g_dmesgInfo->logSize += logLen;
    }
}

//记录日志信息
INT32 OsLogMemcpyRecord(const CHAR *buf, UINT32 logLen)
{
    UINT32 intSave;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    if (OsCheckError()) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        return -1;
    }
    if (g_dmesgInfo->logSize < g_logBufSize) {
		//日志缓冲区未满
        if (g_dmesgInfo->logHead <= g_dmesgInfo->logTail) {
			//head在tail之前
            OsWriteTailToEnd(buf, logLen);
        } else {
        	//tail在head之前
            OsWriteTailToHead(buf, logLen);
        }
    } else {
    	//缓冲区已满，覆盖写入
        OsBufFullWrite(buf, logLen);
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);

    return LOS_OK;
}

//显示当前日志缓冲区中的内容，注意内部并没有修订缓冲区控制块的内容
VOID OsLogShow(VOID)
{
    UINT32 intSave;
    UINT32 index;
    UINT32 i = 0;
    CHAR *p = NULL;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
	//从head位置开始读取
    index = g_dmesgInfo->logHead;

	//日志数据读取到临时内存中
    p = (CHAR *)malloc(g_dmesgInfo->logSize + 1);
    if (p == NULL) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        return;
    }
    (VOID)memset_s(p, g_dmesgInfo->logSize + 1, 0, g_dmesgInfo->logSize + 1);

	//读取缓存区中的日志
    while (i < g_dmesgInfo->logSize) {
		//按字节读取
        *(p + i) = *(g_dmesgInfo->logBuf + index++);
        if (index > BUF_MAX_INDEX) {
            index = 0; //尾部读完了，再读头部
        }
        i++; //已读取的字节数目
        if (index == g_dmesgInfo->logTail) {
            break; //读完日志
        }
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
    UartPuts(p, i, UART_WITH_LOCK); //读取完的日志信息输出到串口
    free(p); //释放临时内存块
}

//设置dmesg日志等级
STATIC INT32 OsDmesgLvSet(const CHAR *level)
{
    UINT32 levelNum, ret;
    CHAR *p = NULL;

	//字符串转整数
    levelNum = strtoul(level, &p, 0);
    if (*p != 0) {
        PRINTK("dmesg: invalid option or parameter.\n");
        return -1;
    }

	//设置日志等级
    ret = LOS_DmesgLvSet(levelNum);
    if (ret == LOS_OK) {
        PRINTK("Set current dmesg log level %s\n", g_levelString[g_dmesgLogLevel]);
        return LOS_OK;
    } else {
        PRINTK("current dmesg log level %s\n", g_levelString[g_dmesgLogLevel]);
        PRINTK("dmesg -l [num] can access as 0:EMG 1:COMMON 2:ERROR 3:WARN 4:INFO 5:DEBUG\n");
        return -1;
    }
}

//设置dmesg日志缓冲区尺寸
STATIC INT32 OsDmesgMemSizeSet(const CHAR *size)
{
    UINT32 sizeVal;
    CHAR *p = NULL;

	//字符串转整数
    sizeVal = strtoul(size, &p, 0);
    if (sizeVal > MAX_KERNEL_LOG_BUF_SIZE) {
        goto ERR_OUT;
    }

	//设置缓冲区尺寸
    if (!(LOS_DmesgMemSet(NULL, sizeVal))) {
        PRINTK("Set dmesg buf size %u success\n", sizeVal);
        return LOS_OK;
    } else {
        goto ERR_OUT;
    }

ERR_OUT:
    PRINTK("Set dmesg buf size %u fail\n", sizeVal);
    return LOS_NOK;
}

//获取日志等级
UINT32 OsDmesgLvGet(VOID)
{
    return g_dmesgLogLevel;
}

//设置日志等级
UINT32 LOS_DmesgLvSet(UINT32 level)
{
    if (level > 5) { /* 5: count of level */
        return LOS_NOK;
    }

    g_dmesgLogLevel = level;
    return LOS_OK;
}

//清空日志缓冲区
VOID LOS_DmesgClear(VOID)
{
    UINT32 intSave;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    (VOID)memset_s(g_dmesgInfo->logBuf, g_logBufSize, 0, g_logBufSize);
    g_dmesgInfo->logHead = 0;
    g_dmesgInfo->logTail = 0;
    g_dmesgInfo->logSize = 0;
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
}

UINT32 LOS_DmesgMemSet(const VOID *addr, UINT32 size)
{
    UINT32 ret = 0;

    if (addr == NULL) {
        ret = OsDmesgChangeSize(size);
    } else {
        ret = OsDmesgResetMem(addr, size);
    }
    return ret;
}

//从日志缓冲区中读取日志
INT32 LOS_DmesgRead(CHAR *buf, UINT32 len)
{
    INT32 ret;
    UINT32 intSave;

    if (buf == NULL) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    ret = OsDmesgRead(buf, len);
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
    return ret;
}

//写日志文件
INT32 OsDmesgWrite2File(const CHAR *fullpath, const CHAR *buf, UINT32 logSize)
{
    INT32 ret;

	//追加方式写日志到文件，文件不存在，则新建文件
    INT32 fd = open(fullpath, O_CREAT | O_RDWR | O_APPEND, 0644); /* 0644:file right */
    if (fd < 0) {
        return -1;
    }
    ret = write(fd, buf, logSize);
    (VOID)close(fd);
    return ret;
}

#ifdef LOSCFG_FS_VFS
//从日志缓冲区中取出日志并写入日志文件
INT32 LOS_DmesgToFile(const CHAR *filename)
{
    CHAR *fullpath = NULL;
    CHAR *buf = NULL;
    INT32 ret;
    CHAR *shellWorkingDirectory = OsShellGetWorkingDirtectory();
    UINT32 logSize, bufSize, head, tail, intSave;
    CHAR *logBuf = NULL;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    if (OsCheckError()) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        return -1;
    }
    logSize = g_dmesgInfo->logSize;
    bufSize = g_logBufSize;
    head = g_dmesgInfo->logHead;
    tail = g_dmesgInfo->logTail;
    logBuf = g_dmesgInfo->logBuf;
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);

	//获取日志文件的全路径
    ret = vfs_normalize_path(shellWorkingDirectory, filename, &fullpath);
    if (ret != 0) {
        return -1;
    }

	//获取临时内存，用于读取日志信息，这部分为了性能和安全，没有使用有锁操作
    buf = (CHAR *)malloc(logSize);
    if (buf == NULL) {
        goto ERR_OUT2;
    }

    if (head < tail) {
		//日志信息在中部
        ret = memcpy_s(buf, logSize, logBuf + head, logSize);
        if (ret != EOK) {
            goto ERR_OUT3;
        }
    } else {
    	//日志信息在2头，先拷贝尾部，再拷贝头部
        ret = memcpy_s(buf, logSize, logBuf + head, bufSize - head);
        if (ret != EOK) {
            goto ERR_OUT3;
        }
        ret = memcpy_s(buf + bufSize - head, logSize - (bufSize - head), logBuf, tail);
        if (ret != EOK) {
            goto ERR_OUT3;
        }
    }

	//将日志信息写入文件
    ret = OsDmesgWrite2File(fullpath, buf, logSize);
ERR_OUT3:
    free(buf);
ERR_OUT2:
    free(fullpath);
    return ret;
}
#else
INT32 LOS_DmesgToFile(CHAR *filename)
{
    (VOID)filename;
    PRINTK("File operation need VFS\n");
    return -1;
}
#endif

//dmesg相关的命令
INT32 OsShellCmdDmesg(INT32 argc, const CHAR **argv)
{
    if (argc == 1) {
        PRINTK("\n");
        OsLogShow();  //dmesg
        return LOS_OK;
    } else if (argc == 2) { /* 2: count of parameters */
        if (argv == NULL) {
            goto ERR_OUT;
        }

        if (!strcmp(argv[1], "-c")) {
            PRINTK("\n");  // dmesg -c
            OsLogShow();
            LOS_DmesgClear(); 
            return LOS_OK;
        } else if (!strcmp(argv[1], "-C")) {
            LOS_DmesgClear(); //dmesg -C
            return LOS_OK;
        } else if (!strcmp(argv[1], "-D")) {
            OsLockConsole(); //dmesg -D
            return LOS_OK;
        } else if (!strcmp(argv[1], "-E")) {
            OsUnlockConsole(); //dmesg -E
            return LOS_OK;
        } else if (!strcmp(argv[1], "-L")) {
            OsLockUart();  //dmesg -L
            return LOS_OK;
        } else if (!strcmp(argv[1], "-U")) {
            OsUnlockUart(); //dmesg -U
            return LOS_OK;
        }
    } else if (argc == 3) { /* 3: count of parameters */
        if (argv == NULL) {
            goto ERR_OUT;
        }

        if (!strcmp(argv[1], ">")) {
			//输出重定向到指定的文件
            if (LOS_DmesgToFile((CHAR *)argv[2]) < 0) { /* 2:index of parameters */
                PRINTK("Dmesg write log to %s fail \n", argv[2]); /* 2:index of parameters */
                return -1;
            } else {
                PRINTK("Dmesg write log to %s success \n", argv[2]); /* 2:index of parameters */
                return LOS_OK;
            }
        } else if (!strcmp(argv[1], "-l")) {
        	//设置日志等级
            return OsDmesgLvSet(argv[2]); /* 2:index of parameters */
        } else if (!strcmp(argv[1], "-s")) {
			//设置日志缓冲区尺寸
            return OsDmesgMemSizeSet(argv[2]); /* 2:index of parameters */
        }
    }

ERR_OUT:
    PRINTK("dmesg: invalid option or parameter.\n");
    return -1;
}

//dmesg命令入口注册
SHELLCMD_ENTRY(dmesg_shellcmd, CMD_TYPE_STD, "dmesg", XARGS, (CmdCallBackFunc)OsShellCmdDmesg);

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
#endif
