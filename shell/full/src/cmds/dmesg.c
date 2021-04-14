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

//�����ں���������ʹ�õ���־��������Ӧ��������
LITE_OS_SEC_BSS STATIC SPIN_LOCK_INIT(g_dmesgSpin);

//��־������������
STATIC DmesgInfo *g_dmesgInfo = NULL;
//�������ߴ�
STATIC UINT32 g_logBufSize = 0;
//�������ڴ��ַ(��������)
STATIC VOID *g_mallocAddr = NULL;
//��־�ȼ�
STATIC UINT32 g_dmesgLogLevel = 3;
//����̨��
STATIC UINT32 g_consoleLock = 0;
//������
STATIC UINT32 g_uartLock = 0;
//��־�ȼ��ַ���
STATIC const CHAR *g_levelString[] = {
    "EMG",
    "COMMON",
    "ERR",
    "WARN",
    "INFO",
    "DEBUG"
};

//��ס����̨
STATIC VOID OsLockConsole(VOID)
{
    g_consoleLock = 1;
}

//��������̨
STATIC VOID OsUnlockConsole(VOID)
{
    g_consoleLock = 0;
}

//��ס����
STATIC VOID OsLockUart(VOID)
{
    g_uartLock = 1;
}

//��������
STATIC VOID OsUnlockUart(VOID)
{
    g_uartLock = 0;
}

//������
STATIC UINT32 OsCheckError(VOID)
{
    if (g_dmesgInfo == NULL) {
        return LOS_NOK; //���ں�������־������
    }

    if (g_dmesgInfo->logSize > g_logBufSize) {
        return LOS_NOK; //��־�ߴ����
    }

    if (((g_dmesgInfo->logSize == g_logBufSize) || (g_dmesgInfo->logSize == 0)) &&
        (g_dmesgInfo->logTail != g_dmesgInfo->logHead)) {
        return LOS_NOK; //�ߴ�����գ�����־ͷβ�����
    }

    return LOS_OK;
}

//����־�������ж�����־
STATIC INT32 OsDmesgRead(CHAR *buf, UINT32 len)
{
    UINT32 readLen;
    UINT32 logSize = g_dmesgInfo->logSize;
    UINT32 head = g_dmesgInfo->logHead;
    UINT32 tail = g_dmesgInfo->logTail;
    CHAR *logBuf = g_dmesgInfo->logBuf;
    errno_t ret;

    if (OsCheckError()) { //��黺����������
        return -1;
    }
    if (logSize == 0) {
        return 0; //û�����ݿɶ�
    }

	//�����ܶ�Ĵӻ������ж�����־
    readLen = len < logSize ? len : logSize;

    if (head < tail) { /* Case A */ //��Ч�����ڻ������в�
        ret = memcpy_s(buf, len, logBuf + head, readLen); //ֻ��Ҫ����һ������
        if (ret != EOK) {
            return -1;
        }
        g_dmesgInfo->logHead += readLen;  //�ƶ���һ�ζ�ȡ��־��λ��
        g_dmesgInfo->logSize -= readLen;  //������������ʣ����־�ĳߴ�
    } else { /* Case B */ //��Ч�����ڻ�����������
        if (readLen <= (g_logBufSize - head)) {
			//��Ҫ��ȡ�ĳߴ粻����β��������
            ret = memcpy_s(buf, len, logBuf + head, readLen); //ֻ��һ�ξͿ�����
            if (ret != EOK) {
                return -1;
            }
            g_dmesgInfo->logHead += readLen; //��һ�ζ�ȡ��λ��
            g_dmesgInfo->logSize -= readLen; //ʣ����־�ߴ�
        } else {
			//��Ҫ��ȡ2�Σ��Ȱ�β������
            ret = memcpy_s(buf, len, logBuf + head, g_logBufSize - head);
            if (ret != EOK) {
                return -1;
            }

			//�ٶ�ȡ�ײ��Ĳ�������
            ret = memcpy_s(buf + g_logBufSize - head, len - (g_logBufSize - head),
                           logBuf, readLen - (g_logBufSize - head));
            if (ret != EOK) {
                return -1;
            }
			//������һ�ζ�ȡ���ݵ���ʼλ��
            g_dmesgInfo->logHead = readLen - (g_logBufSize - head);
            g_dmesgInfo->logSize -= readLen; //ʣ�����־�ߴ�
        }
    }
    return (INT32)readLen;
}

//����־�������е����ݿ�����һ���µĻ�����
STATIC INT32 OsCopyToNew(const VOID *addr, UINT32 size) 
{
    UINT32 copyStart = 0;
    UINT32 copyLen;
    CHAR *temp = NULL;
    CHAR *newBuf = (CHAR *)addr + sizeof(DmesgInfo); //�»�������������ͷ
    UINT32 bufSize = size - sizeof(DmesgInfo); //�»������������ߴ�
    INT32 ret;

    if (g_dmesgInfo->logSize == 0) {
        return 0; //û����־����
    }

    temp = (CHAR *)malloc(g_dmesgInfo->logSize);  //������ʱ�������ڶ�ȡ־����
    if (temp == NULL) {
        return -1;
    }

    (VOID)memset_s(temp, g_dmesgInfo->logSize, 0, g_dmesgInfo->logSize);
	//������Ҫ���������ݳ��ȣ����ܳ����»���������������
    copyLen = ((bufSize < g_dmesgInfo->logSize) ? bufSize : g_dmesgInfo->logSize);
    if (bufSize < g_dmesgInfo->logSize) {
		//����»��������������Ƚ�С���򿽱����µ���־�����ϵ���־Ҫ����
        copyStart = g_dmesgInfo->logSize - bufSize;
    }

	//������ȡ��־����
    ret = OsDmesgRead(temp, g_dmesgInfo->logSize);
    if (ret <= 0) {
        PRINT_ERR("%s,%d failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
        free(temp);
        return -1;
    }

    /* if new buf size smaller than logSize */
	//���������򲿷���־���ݵ��»�����
    ret = memcpy_s(newBuf, bufSize, temp + copyStart, copyLen);
    if (ret != EOK) {
        PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
        free(temp);
        return -1;
    }
    free(temp);  //�ͷŸ�����־��ȡ����ʱ�ڴ�

    return (INT32)copyLen;
}

//����������־�������������ɻ������е���־�������»�������
STATIC UINT32 OsDmesgResetMem(const VOID *addr, UINT32 size)
{
    VOID *temp = NULL;
    INT32 copyLen;
    UINT32 intSave;

    if (size <= sizeof(DmesgInfo)) {
        return LOS_NOK; //�»�����û�а취�洢��־����
    }

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    temp = g_dmesgInfo;
	//����־���ݿ������»�����
    copyLen = OsCopyToNew(addr, size);
    if (copyLen < 0) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        return LOS_NOK;
    }

	//�����»��������¹������ͷ
    g_logBufSize = size - sizeof(DmesgInfo);
    g_dmesgInfo = (DmesgInfo *)addr;
    g_dmesgInfo->logBuf = (CHAR *)addr + sizeof(DmesgInfo);
    g_dmesgInfo->logSize = copyLen;
    g_dmesgInfo->logTail = ((copyLen == g_logBufSize) ? 0 : copyLen);
    g_dmesgInfo->logHead = 0;

    /* if old mem came from malloc */
    if (temp == g_mallocAddr) {
        g_mallocAddr = NULL;
        free(temp);  //�ͷž���־��
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);

    return LOS_OK;
}

//������־�������ߴ磬�ڲ��߼�����һ����������
//������������malloc����
STATIC UINT32 OsDmesgChangeSize(UINT32 size)
{
    VOID *temp = NULL;
    INT32 copyLen;
    CHAR *newString = NULL;
    UINT32 intSave;

    if (size == 0) {
        return LOS_NOK;
    }

	//����־������(������ͷ
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

//����̨�Ƿ���ס
UINT32 OsCheckConsoleLock(VOID)
{
    return g_consoleLock;
}

//�����Ƿ���ס
UINT32 OsCheckUartLock(VOID)
{
    return g_uartLock;
}

//��ʼ��dmesg������������������Ҫ���ڼ�¼�����׶εĵ�����Ϣ
UINT32 OsDmesgInit(VOID)
{
    CHAR* buffer = NULL;

    buffer = (CHAR *)malloc(KERNEL_LOG_BUF_SIZE + sizeof(DmesgInfo));
    if (buffer == NULL) {
        return LOS_NOK;
    }
    g_mallocAddr = buffer;
    g_dmesgInfo = (DmesgInfo *)buffer;
    g_dmesgInfo->logHead = 0; //��־���ݵ���ʼλ��
    g_dmesgInfo->logTail = 0; //��һ����־��д��λ��
    g_dmesgInfo->logSize = 0; //ʣ�໹δ��ȡ����־�洢�ռ��С
    g_dmesgInfo->logBuf = buffer + sizeof(DmesgInfo); //��־�������׵�ַ
    g_logBufSize = KERNEL_LOG_BUF_SIZE; //��־����������

    return LOS_OK;
}

//����־������д��һ���ַ�
STATIC CHAR OsLogRecordChar(CHAR c)
{
	//д���ַ�����������һ����Ҫд���ƫ��ֵ
    *(g_dmesgInfo->logBuf + g_dmesgInfo->logTail++) = c;

    if (g_dmesgInfo->logTail > BUF_MAX_INDEX) {
        g_dmesgInfo->logTail = 0; //���ﻺ����ĩβ���´δ��ײ���ʼд��
    }

    if (g_dmesgInfo->logSize < g_logBufSize) {
        (g_dmesgInfo->logSize)++; //��־�ߴ�����
    } else {
    	//��־�����´�д��λ�úͶ�ȡλ��һ��
        g_dmesgInfo->logHead = g_dmesgInfo->logTail;
    }
    return c; //���ظ�д����ַ�
}

//����־������д���ַ���
UINT32 OsLogRecordStr(const CHAR *str, UINT32 len)
{
    UINT32 i = 0;
    UINTPTR intSave;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    while (len--) {  //�����ַ���
    	//����д��ÿһ���ַ�
        (VOID)OsLogRecordChar(str[i]);
        i++;  //�������ַ�����������Ϊ�����Ժ��ѭ������
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
    return i; //����д����ַ������ȣ���len
}

//��ǰ����������������д����־�ĺ���
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
		//��Ҫд��ĳߴ糬���˻������ܳ��ȣ���ô����Ҫ��2��д��
		//��һ��д��������β��
        ret = memcpy_s(buf + tail, bufSize - tail, dst, bufSize - tail);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }
		//�ڶ��δ�ͷ��ʼд�룬������������
        ret = memcpy_s(buf, bufSize, dst + bufSize - tail, tail);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//����������Ҫд�룬�ݹ����
        OsBufFullWrite(dst + bufSize, logLen - bufSize);
    } else {
		//ʣ�����ݲ����պó��������������
        if (logLen > (bufSize - tail)) { /* need cycle back to start */
			//����β�����в��ֲ���д�룬����Ҫ��2��д��
			//��д��β��
            ret = memcpy_s(buf + tail, bufSize - tail, dst, bufSize - tail);
            if (ret != EOK) {
                PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
                return;
            }
			//Ȼ��ʣ������д��ͷ��
            ret = memcpy_s(buf, bufSize, dst + bufSize - tail, logLen - (bufSize - tail));
            if (ret != EOK) {
                PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
                return;
            }

			//��¼����Ч��־��������λ��
            g_dmesgInfo->logTail = logLen - (bufSize - tail);
			//��������״̬������head��tail����һ��
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
			//�����߻ᱣ֤�ѻ�����д��
            g_dmesgInfo->logHead = g_dmesgInfo->logTail;
        }
    }
}


//tail��ǰ��head�ں������£�д����־����
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
		//��Ҫд������ݳ�����ʣ��Ļ�����
        writeLen = bufSize - logSize; //�Ƚ�����������
        ret = memcpy_s(buf + tail, bufSize - tail, dst, writeLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//��ʱ��tail��׷��head��
        g_dmesgInfo->logTail = g_dmesgInfo->logHead;
        g_dmesgInfo->logSize = g_logBufSize; //������������
        OsBufFullWrite(dst + writeLen, logLen - writeLen); //��������������£�����д��ʣ�����־
    } else {
		//��Ҫд������ݲ��ᳬ��ʣ�໺����
        ret = memcpy_s(buf + tail, bufSize - tail, dst, logLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//��������tail��size����
        g_dmesgInfo->logTail += logLen;
        g_dmesgInfo->logSize += logLen;
    }
}

//tail��head֮���������ȴ�tail����д���ݣ����ܻ�������ڻ�����ͷ��д����
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
		//����������������β�������
        writeLen = bufSize - tail;
		//������������β��
        ret = memcpy_s(buf + tail, writeLen, dst, writeLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

        g_dmesgInfo->logSize += writeLen; //��־�ߴ���������
        g_dmesgInfo->logTail = 0;  //��ͷ����ʼ����д��־����
        if (g_dmesgInfo->logSize == g_logBufSize) { /* Tail = Head is 0 */
			//����������������д��ʣ�����־����
            OsBufFullWrite(dst + writeLen, logLen - writeLen);
        } else {
        	//��ʱtailһ��С��head����־д�뷽����tailtoend, ����дʣ����־
            OsWriteTailToHead(dst + writeLen, logLen - writeLen);
        }
    } else { /* just do serial copy */
    	//ʣ����־��������������β��
        ret = memcpy_s(buf + tail, bufSize - tail, dst, logLen);
        if (ret != EOK) {
            PRINT_ERR("%s,%d memcpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
            return;
        }

		//�������»�����
        g_dmesgInfo->logTail += logLen;
        g_dmesgInfo->logSize += logLen;
    }
}

//��¼��־��Ϣ
INT32 OsLogMemcpyRecord(const CHAR *buf, UINT32 logLen)
{
    UINT32 intSave;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
    if (OsCheckError()) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        return -1;
    }
    if (g_dmesgInfo->logSize < g_logBufSize) {
		//��־������δ��
        if (g_dmesgInfo->logHead <= g_dmesgInfo->logTail) {
			//head��tail֮ǰ
            OsWriteTailToEnd(buf, logLen);
        } else {
        	//tail��head֮ǰ
            OsWriteTailToHead(buf, logLen);
        }
    } else {
    	//����������������д��
        OsBufFullWrite(buf, logLen);
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);

    return LOS_OK;
}

//��ʾ��ǰ��־�������е����ݣ�ע���ڲ���û���޶����������ƿ������
VOID OsLogShow(VOID)
{
    UINT32 intSave;
    UINT32 index;
    UINT32 i = 0;
    CHAR *p = NULL;

    LOS_SpinLockSave(&g_dmesgSpin, &intSave);
	//��headλ�ÿ�ʼ��ȡ
    index = g_dmesgInfo->logHead;

	//��־���ݶ�ȡ����ʱ�ڴ���
    p = (CHAR *)malloc(g_dmesgInfo->logSize + 1);
    if (p == NULL) {
        LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
        return;
    }
    (VOID)memset_s(p, g_dmesgInfo->logSize + 1, 0, g_dmesgInfo->logSize + 1);

	//��ȡ�������е���־
    while (i < g_dmesgInfo->logSize) {
		//���ֽڶ�ȡ
        *(p + i) = *(g_dmesgInfo->logBuf + index++);
        if (index > BUF_MAX_INDEX) {
            index = 0; //β�������ˣ��ٶ�ͷ��
        }
        i++; //�Ѷ�ȡ���ֽ���Ŀ
        if (index == g_dmesgInfo->logTail) {
            break; //������־
        }
    }
    LOS_SpinUnlockRestore(&g_dmesgSpin, intSave);
    UartPuts(p, i, UART_WITH_LOCK); //��ȡ�����־��Ϣ���������
    free(p); //�ͷ���ʱ�ڴ��
}

//����dmesg��־�ȼ�
STATIC INT32 OsDmesgLvSet(const CHAR *level)
{
    UINT32 levelNum, ret;
    CHAR *p = NULL;

	//�ַ���ת����
    levelNum = strtoul(level, &p, 0);
    if (*p != 0) {
        PRINTK("dmesg: invalid option or parameter.\n");
        return -1;
    }

	//������־�ȼ�
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

//����dmesg��־�������ߴ�
STATIC INT32 OsDmesgMemSizeSet(const CHAR *size)
{
    UINT32 sizeVal;
    CHAR *p = NULL;

	//�ַ���ת����
    sizeVal = strtoul(size, &p, 0);
    if (sizeVal > MAX_KERNEL_LOG_BUF_SIZE) {
        goto ERR_OUT;
    }

	//���û������ߴ�
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

//��ȡ��־�ȼ�
UINT32 OsDmesgLvGet(VOID)
{
    return g_dmesgLogLevel;
}

//������־�ȼ�
UINT32 LOS_DmesgLvSet(UINT32 level)
{
    if (level > 5) { /* 5: count of level */
        return LOS_NOK;
    }

    g_dmesgLogLevel = level;
    return LOS_OK;
}

//�����־������
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

//����־�������ж�ȡ��־
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

//д��־�ļ�
INT32 OsDmesgWrite2File(const CHAR *fullpath, const CHAR *buf, UINT32 logSize)
{
    INT32 ret;

	//׷�ӷ�ʽд��־���ļ����ļ������ڣ����½��ļ�
    INT32 fd = open(fullpath, O_CREAT | O_RDWR | O_APPEND, 0644); /* 0644:file right */
    if (fd < 0) {
        return -1;
    }
    ret = write(fd, buf, logSize);
    (VOID)close(fd);
    return ret;
}

#ifdef LOSCFG_FS_VFS
//����־��������ȡ����־��д����־�ļ�
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

	//��ȡ��־�ļ���ȫ·��
    ret = vfs_normalize_path(shellWorkingDirectory, filename, &fullpath);
    if (ret != 0) {
        return -1;
    }

	//��ȡ��ʱ�ڴ棬���ڶ�ȡ��־��Ϣ���ⲿ��Ϊ�����ܺͰ�ȫ��û��ʹ����������
    buf = (CHAR *)malloc(logSize);
    if (buf == NULL) {
        goto ERR_OUT2;
    }

    if (head < tail) {
		//��־��Ϣ���в�
        ret = memcpy_s(buf, logSize, logBuf + head, logSize);
        if (ret != EOK) {
            goto ERR_OUT3;
        }
    } else {
    	//��־��Ϣ��2ͷ���ȿ���β�����ٿ���ͷ��
        ret = memcpy_s(buf, logSize, logBuf + head, bufSize - head);
        if (ret != EOK) {
            goto ERR_OUT3;
        }
        ret = memcpy_s(buf + bufSize - head, logSize - (bufSize - head), logBuf, tail);
        if (ret != EOK) {
            goto ERR_OUT3;
        }
    }

	//����־��Ϣд���ļ�
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

//dmesg��ص�����
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
			//����ض���ָ�����ļ�
            if (LOS_DmesgToFile((CHAR *)argv[2]) < 0) { /* 2:index of parameters */
                PRINTK("Dmesg write log to %s fail \n", argv[2]); /* 2:index of parameters */
                return -1;
            } else {
                PRINTK("Dmesg write log to %s success \n", argv[2]); /* 2:index of parameters */
                return LOS_OK;
            }
        } else if (!strcmp(argv[1], "-l")) {
        	//������־�ȼ�
            return OsDmesgLvSet(argv[2]); /* 2:index of parameters */
        } else if (!strcmp(argv[1], "-s")) {
			//������־�������ߴ�
            return OsDmesgMemSizeSet(argv[2]); /* 2:index of parameters */
        }
    }

ERR_OUT:
    PRINTK("dmesg: invalid option or parameter.\n");
    return -1;
}

//dmesg�������ע��
SHELLCMD_ENTRY(dmesg_shellcmd, CMD_TYPE_STD, "dmesg", XARGS, (CmdCallBackFunc)OsShellCmdDmesg);

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
#endif
