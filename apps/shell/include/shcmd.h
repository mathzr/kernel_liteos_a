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


#ifndef _SHCMD_H
#define _SHCMD_H

#include "string.h"
#include "stdlib.h"
#include "shell_list.h"
#include "shcmdparse.h"
#include "sherr.h"
#include "show.h"

#ifdef  __cplusplus
#if  __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */


//命令链表
typedef struct {
    unsigned int count;  //链表中的命令个数，特别是历史命令的时候比较有用
    SH_List list;  //将命令连接起来
    char cmdString[0]; //本条命令的字符串
} CmdKeyLink;

//每行能打印的字符串个数是lineCap
//所以每打印lineCap个字符串就得换行了
#define NEED_NEW_LINE(timesPrint, lineCap) ((timesPrint) % (lineCap) == 0)

//每行能打印的字符串数与屏幕行数的乘积为每一屏最多能打印的字符串个数
//超过这个数目的字符串一屏打印不完
#define SCREEN_IS_FULL(timesPrint, lineCap) ((timesPrint) >= ((lineCap) * DEFAULT_SCREEN_HEIGNT))

extern unsigned int OsCmdExec(CmdParsed *cmdParsed, char *cmdStr);
extern unsigned int OsCmdKeyShift(const char *cmdKey, char *cmdOut, unsigned int size);
extern int OsTabCompletion(char *cmdKey, unsigned int *len);
extern void OsShellCmdPush(const char *string, CmdKeyLink *cmdKeyLink);
extern void OsShellHistoryShow(unsigned int value, ShellCB *shellCB);
extern unsigned int OsShellKeyInit(ShellCB *shellCB);
extern void OsShellKeyDeInit(CmdKeyLink *cmdKeyLink);
extern int OsShellSetWorkingDirtectory(const char *dir, size_t len);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

#endif /* _SHCMD_H */
