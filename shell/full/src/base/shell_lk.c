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

#include "shell_lk.h"
#include "shcmd.h"
#ifdef LOSCFG_SHELL_DMESG
#include "dmesg_pri.h"
#endif
#include "los_printf_pri.h"
#include "unistd.h"
#include "stdlib.h"
#include "stdio.h"
#include "securec.h"

#ifdef LOSCFG_SHELL_LK
#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

//命令行内核日志处理

//模块标识
typedef enum {
    MODULE0 = 0,
    MODULE1 = 1,
    MODULE2 = 2,
    MODULE3 = 3,
    MODULE4 = 4,
} MODULE_FLAG;


//日志管理控制块
typedef struct {
    INT32 module_level; //模块级别
    INT32 trace_level;  //调试级别
    FILE *fp;  //日志文件
} Logger;

STATIC INT32 g_tracelevel;
STATIC INT32 g_modulelevel;

STATIC Logger g_logger = { 0 };

VOID OsLkDefaultFunc(INT32 level, const CHAR *func, INT32 line, const CHAR *fmt, va_list ap);

LK_FUNC g_osLkHook = (LK_FUNC)OsLkDefaultFunc;

STATIC INLINE INT32 OsLkTraceLvGet(VOID)
{
    return g_tracelevel;
}

const CHAR *OsLkCurLogLvGet(VOID)  //获取当前日志等级
{
    return OsLogLvGet(g_tracelevel);
}

//设置trace等级
VOID OsLkTraceLvSet(INT32 level)
{
    g_tracelevel = level;
    g_logger.trace_level = level;
    return;
}

//设置模块等级
VOID OsLkModuleLvSet(INT32 level)
{
    g_modulelevel = level;
    g_logger.module_level = level;
    return;
}

//读取模块等级
INT32 OsLkModuleLvGet(VOID)
{
    return g_modulelevel;
}


//设置日志文件名称
VOID OsLkLogFileSet(const CHAR *str)
{
    FILE *fp = NULL;
    FILE *oldfp = g_logger.fp; //暂存旧日志文件

    if (str == NULL) {
        return;
    }
    fp = fopen(str, "w+"); //创建并打开新日志文件,为了后续的写操作
    if (fp == NULL) {
        printf("Error can't open the %s file\n",str);
        return;
    }

    g_logger.fp = fp; //记录新创建的日志文件
    if (oldfp != NULL) {
        fclose(oldfp); //关闭旧日志文件
    }
}

//获取当前日志文件描述符
FILE *OsLogFpGet(VOID)
{
    return g_logger.fp;
}

//log命令的处理入口
INT32 CmdLog(INT32 argc, const CHAR **argv)
{
    size_t level;
    size_t module;
    CHAR *p = NULL;

	//log命令使用提示文本
    if ((argc != 2) || (argv == NULL)) { /* 2:count of parameter */
        PRINTK("Usage: log level <num>\n");
        PRINTK("Usage: log module <num>\n");
        PRINTK("Usage: log path <PATH>\n");
        return -1;
    }

    if (!strncmp(argv[0], "level", strlen(argv[0]) + 1)) {
        level = strtoul(argv[1], &p, 0);
        if ((*p != 0) || (level > LOS_TRACE_LEVEL) || (level < LOS_EMG_LEVEL)) {
            PRINTK("current log level %s\n", OsLkCurLogLvGet());
            PRINTK("log %s [num] can access as 0:EMG 1:COMMON 2:ERROR 3:WARN 4:INFO 5:DEBUG\n", argv[0]);
        } else {
            OsLkTraceLvSet(level); //设置log level num
            PRINTK("Set current log level %s\n", OsLkCurLogLvGet());
        }
    } else if (!strncmp(argv[0], "module", strlen(argv[0]) + 1)) {
        module = strtoul(argv[1], &p, 0);
        if ((*p != 0) || (module > MODULE4) || (module < MODULE0)) {
            PRINTK("log %s can't access %s\n", argv[0], argv[1]);
            PRINTK("not support yet\n");
            return -1;
        } else {
            OsLkModuleLvSet(module); //设置log moudule num
            PRINTK("not support yet\n");
        }
    } else if (!strncmp(argv[0], "path", strlen(argv[0]) + 1)) {
        OsLkLogFileSet(argv[1]);  //设置log path ...
        PRINTK("not support yet\n"); 
    } else {
        PRINTK("Usage: log level <num>\n");
        PRINTK("Usage: log module <num>\n");
        PRINTK("Usage: log path <PATH>\n");
        return -1;
    }

    return 0;
}

#ifdef LOSCFG_SHELL_DMESG
//记录日志等级字符串
STATIC INLINE VOID OsLogCycleRecord(INT32 level)
{
    UINT32 tmpLen;
    if (level != LOS_COMMON_LEVEL && (level > LOS_EMG_LEVEL && level <= LOS_TRACE_LEVEL)) {
        tmpLen = strlen(OsLogLvGet(level));
        const CHAR* tmpPtr = OsLogLvGet(level);
        (VOID)OsLogRecordStr(tmpPtr, tmpLen);
    }
}
#endif

//内核日志默认输出函数
VOID OsLkDefaultFunc(INT32 level, const CHAR *func, INT32 line, const CHAR *fmt, va_list ap)
{
    if (level > OsLkTraceLvGet()) {
#ifdef LOSCFG_SHELL_DMESG
        if ((UINT32)level <= OsDmesgLvGet()) {
            OsLogCycleRecord(level);
            DmesgPrintf(fmt, ap);  //输出到dmesg缓存
        }
#endif
        return;
    }
    if ((level != LOS_COMMON_LEVEL) && ((level > LOS_EMG_LEVEL) && (level <= LOS_TRACE_LEVEL))) {
        dprintf("[%s]", OsLogLvGet(level)); //输出到控制台
    }
    LkDprintf(fmt, ap); //输出到控制台
}

//内核日志默认输出函数--变参版本
VOID LOS_LkPrint(INT32 level, const CHAR *func, INT32 line, const CHAR *fmt, ...)
{
    va_list ap;
    if (g_osLkHook != NULL) {
        va_start(ap, fmt);
        g_osLkHook(level, func, line, fmt, ap);
        va_end(ap);
    }
}

VOID LOS_LkRegHook(LK_FUNC hook)
{
    g_osLkHook = hook;
}

VOID OsLkLoggerInit(const CHAR *str)
{
    (VOID)str;
    (VOID)memset_s(&g_logger, sizeof(Logger), 0, sizeof(Logger));
    OsLkTraceLvSet(TRACE_DEFAULT);
    LOS_LkRegHook(OsLkDefaultFunc);
#ifdef LOSCFG_SHELL_DMESG
    (VOID)LOS_DmesgLvSet(TRACE_DEFAULT);
#endif
}

#ifdef LOSCFG_SHELL_CMD_DEBUG
SHELLCMD_ENTRY(log_shellcmd, CMD_TYPE_EX, "log", 1, (CmdCallBackFunc)CmdLog); //注册log命令
#endif

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */
#endif
