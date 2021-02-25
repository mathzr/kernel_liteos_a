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

#include "stdlib.h"
#include "stdio.h"
#include "unistd.h"
#include "sys/prctl.h"
#include "sys/ioctl.h"
#include "syscall.h"
#include "sys/wait.h"
#include "pthread.h"
#include "securec.h"
#include "shmsg.h"
#include "shell_pri.h"
#include "shcmd.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */


//从命令队列中取出一个待执行的命令
char *GetCmdline(ShellCB *shellCB)
{
    CmdKeyLink *cmdkey = shellCB->cmdKeyLink;
    CmdKeyLink *cmdNode = NULL;

    (void)pthread_mutex_lock(&shellCB->keyMutex);
    if ((cmdkey == NULL) || SH_ListEmpty(&cmdkey->list)) {
        (void)pthread_mutex_unlock(&shellCB->keyMutex);
        return NULL;
    }

	//从队列中取出第一个命令
    cmdNode = SH_LIST_ENTRY(cmdkey->list.pstNext, CmdKeyLink, list);
    SH_ListDelete(&(cmdNode->list));
    (void)pthread_mutex_unlock(&shellCB->keyMutex);

	//命令节点此时不能删除，因为后面还要用来保存为历史命令
    return cmdNode->cmdString; //返回命令字符串
}


//在命令执行后，将当前命令保存为历史命令
static void ShellSaveHistoryCmd(char *string, ShellCB *shellCB)
{
    CmdKeyLink *cmdHistory = shellCB->cmdHistoryKeyLink; //历史命令队列头
    CmdKeyLink *cmdkey = SH_LIST_ENTRY(string, CmdKeyLink, cmdString); //需要保存的命令节点
    CmdKeyLink *cmdNxt = NULL;

    if ((string == NULL) || (*string == '\n') || (strlen(string) == 0)) {
        return;  //空命令不起作用，没有必要保存
    }

    (void)pthread_mutex_lock(&shellCB->historyMutex);
    if (cmdHistory->count != 0) {
		//shell中已保存若干历史命令
        cmdNxt = SH_LIST_ENTRY(cmdHistory->list.pstPrev, CmdKeyLink, list);
        if (strcmp(string, cmdNxt->cmdString) == 0) {
			//这条命令最近刚保存，那么重复保存没有意义
            free((void *)cmdkey);
            (void)pthread_mutex_unlock(&shellCB->historyMutex);
            return;
        }
    }

    if (cmdHistory->count == CMD_HISTORY_LEN) {
		//历史命令已满，把最老(队列头部)的命令扔掉
        cmdNxt = SH_LIST_ENTRY(cmdHistory->list.pstNext, CmdKeyLink, list);
        SH_ListDelete(&(cmdNxt->list));
		//同时新命令插入队列尾部
        SH_ListTailInsert(&(cmdHistory->list), &(cmdkey->list));
        free((void *)cmdNxt); //扔掉最老命令
        (void)pthread_mutex_unlock(&shellCB->historyMutex);
        return;
    }

	//新命令插入队列尾部
    SH_ListTailInsert(&(cmdHistory->list), &(cmdkey->list));
    cmdHistory->count++; //历史命令数目增加

    (void)pthread_mutex_unlock(&shellCB->historyMutex);
    return;
}


//等待另外一个线程存入命令
int ShellPend(ShellCB *shellCB)
{
    if (shellCB == NULL) {
        return SH_NOK;
    }

    return sem_wait(&shellCB->shellSem);
}

//通知另一个线程取命令
int ShellNotify(ShellCB *shellCB)
{
    if (shellCB == NULL) {
        return SH_NOK;
    }

    return sem_post(&shellCB->shellSem);
}

enum {
    STAT_NOMAL_KEY,
    STAT_ESC_KEY,
    STAT_MULTI_KEY
};


//根据用户输入的方向键做具体的处理
//主要是上下方向键的历史命令浏览
static int ShellCmdLineCheckUDRL(const char ch, ShellCB *shellCB)
{
    int ret = SH_OK;
    if (ch == 0x1b) { /* 0x1b: ESC */
        shellCB->shellKeyType = STAT_ESC_KEY;
        return ret;
    } else if (ch == 0x5b) { /* 0x5b: first Key combination */
        if (shellCB->shellKeyType == STAT_ESC_KEY) {
            shellCB->shellKeyType = STAT_MULTI_KEY;
            return ret;
        }
    } else if (ch == 0x41) { /* up */
        if (shellCB->shellKeyType == STAT_MULTI_KEY) {
            OsShellHistoryShow(CMD_KEY_UP, shellCB);  //历史命令浏览
            shellCB->shellKeyType = STAT_NOMAL_KEY;
            return ret;
        }
    } else if (ch == 0x42) { /* down */
        if (shellCB->shellKeyType == STAT_MULTI_KEY) {
            shellCB->shellKeyType = STAT_NOMAL_KEY;
            OsShellHistoryShow(CMD_KEY_DOWN, shellCB); //历史命令浏览
            return ret;
        }
    } else if (ch == 0x43) { /* right */
        if (shellCB->shellKeyType == STAT_MULTI_KEY) {
            shellCB->shellKeyType = STAT_NOMAL_KEY;
            return ret;
        }
    } else if (ch == 0x44) { /* left */
        if (shellCB->shellKeyType == STAT_MULTI_KEY) {
            shellCB->shellKeyType = STAT_NOMAL_KEY;
            return ret;
        }
    }
    return SH_NOK;
}

//将当前命令放入命令链表，并通知另外一个线程来取
void ShellTaskNotify(ShellCB *shellCB)
{
    int ret;

    (void)pthread_mutex_lock(&shellCB->keyMutex);
    OsShellCmdPush(shellCB->shellBuf, shellCB->cmdKeyLink); //将命令放入命令队列
    (void)pthread_mutex_unlock(&shellCB->keyMutex);

    ret = ShellNotify(shellCB); //唤醒另一个任务来取命令
    if (ret != SH_OK) {
        printf("command execute failed, \"%s\"", shellCB->shellBuf);
    }
}

//处理输入的回车符
void ParseEnterKey(OutputFunc outputFunc, ShellCB *shellCB)
{
    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if (shellCB->shellBufOffset == 0) {
		//命令行只有一个回车符的情况，有必要通知另一个线程处理吗?
		//从某方面来说，通知另外一个线程，还是有意义的，因为命令队列里面可能缓存了之前的命令
		//可以做到催促其取命令并执行的作用
        shellCB->shellBuf[shellCB->shellBufOffset] = '\n';
        shellCB->shellBuf[shellCB->shellBufOffset + 1] = '\0';
        goto NOTIFY;
    }

    if (shellCB->shellBufOffset <= (SHOW_MAX_LEN - 1)) {
		//回车符预示着命令行字符串结束
        shellCB->shellBuf[shellCB->shellBufOffset] = '\0';
    }
NOTIFY:
    outputFunc("\n"); //在界面显示换行符
    shellCB->shellBufOffset = 0;  //下一个命令从0号位置开始缓存
    ShellTaskNotify(shellCB);  //通知另外一个线程取命令字符串并处理
}


//处理删除字符按键
void ParseDeleteKey(OutputFunc outputFunc, ShellCB *shellCB)
{
    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if ((shellCB->shellBufOffset > 0) && (shellCB->shellBufOffset <= (SHOW_MAX_LEN - 1))) {
		//将原命令行缓冲中最后一个字符覆盖成空字符，到达删除字符的效果
        shellCB->shellBuf[shellCB->shellBufOffset - 1] = '\0';
		//后面再输入字符，就应该在刚才删除字符的位置
        shellCB->shellBufOffset--;
        outputFunc("\b \b"); //在界面产生删除最后一个字符的效果
    }
}


//处理tab按键
void ParseTabKey(OutputFunc outputFunc, ShellCB *shellCB)
{
    int ret;

    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if ((shellCB->shellBufOffset > 0) && (shellCB->shellBufOffset < (SHOW_MAX_LEN - 1))) {
		//tab键按下的作用是做命令行的自动补全
        ret = OsTabCompletion(shellCB->shellBuf, &shellCB->shellBufOffset);
        if (ret > 1) {
			//在自动补全后，如果有多个匹配结果，那么这些结果也会得到显示
			//最后，还需要将补全后的命令再显示一遍，方便
			//用户输入回车直接执行(已完成自动补全)
			//或者用户输入剩余部分(自动补充了若干字符，没有补充成完整的命令)
            outputFunc(SHELL_PROMPT"%s", shellCB->shellBuf);
        }
    }
}


//处理常规字符的输入
void ParseNormalChar(char ch, OutputFunc outputFunc, ShellCB *shellCB)
{
    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if ((ch != '\0') && (shellCB->shellBufOffset < (SHOW_MAX_LEN - 1))) {
		//常规字符记录入命令行缓存
        shellCB->shellBuf[shellCB->shellBufOffset] = ch;
        shellCB->shellBufOffset++;
		//并显示在界面上即可
        outputFunc("%c", ch);
    }

    shellCB->shellKeyType = STAT_NOMAL_KEY;
}


//输入一个字符的处理过程
//内部根据不同的字符做不同的具体处理
void ShellCmdLineParse(char c, OutputFunc outputFunc, ShellCB *shellCB)
{
    const char ch = c;
    int ret;

    if ((shellCB->shellBufOffset == 0) && (ch != '\n') && (ch != '\0')) {
		//处理命令行第1个字符之前，先将已有缓存内容清空
        (void)memset_s(shellCB->shellBuf, SHOW_MAX_LEN, 0, SHOW_MAX_LEN);
    }

    switch (ch) { //然后根据输入的字符做分类处理
        case '\r':
        case '\n': /* enter */ 
            ParseEnterKey(outputFunc, shellCB); //回车键
            break;
        case '\b': /* backspace */
        case 0x7F: /* delete(0x7F) */
            ParseDeleteKey(outputFunc, shellCB); //删除键
            break;
        case '\t': /* tab */
            ParseTabKey(outputFunc, shellCB); //tab键
            break;
        default:
            /* parse the up/down/right/left key */
            ret = ShellCmdLineCheckUDRL(ch, shellCB); //方向按键
            if (ret == SH_OK) {
                return;
            }
            ParseNormalChar(ch, outputFunc, shellCB); //常规按键
            break;
    }

    return;
}

unsigned int ShellMsgNameGet(CmdParsed *cmdParsed, const char *cmdType)
{
    (void)cmdParsed;
    (void)cmdType;
    return SH_ERROR;
}


//获取命令的名称, 即命令行最开始的词组
char *GetCmdName(const char *cmdline, unsigned int len)
{
    unsigned int loop; //命令名称中的字符位置
    const char *tmpStr = NULL; //命令行中当前字符的位置
    bool quotes = FALSE; //初始状态为双引号外
    char *cmdName = NULL; //存放命令名称
    if (cmdline == NULL) {
        return NULL;
    }

	//新申请一块内存来存储命令的名称
	//这个名称肯定不会超过原来的命令行长度
    cmdName = (char *)malloc(len + 1);
    if (cmdName == NULL) {
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return NULL;
    }

    /* Scan the 'cmdline' string for command */
    /* Notice: Command string must not have any special name */
	//扫描命令行
    for (tmpStr = cmdline, loop = 0; (*tmpStr != '\0') && (loop < len); ) {
        /* If reach a double quotes, switch the quotes matching status */
        if (*tmpStr == '\"') {
            SWITCH_QUOTES_STATUS(quotes); //切换当前状态为双引号内or双引号外
            /* Ignore the double quote charactor itself */
            tmpStr++; //命令名称不包含双引号
            continue;
        }
        /* If detected a space which the quotes matching status is false */
        /* which said has detected the first space for seperator, finish this scan operation */
        if ((*tmpStr == ' ') && (QUOTES_STATUS_CLOSE(quotes))) {
            break;  //在双引号外面遇到空格时，说明命令名称结束
        }
		//非空格字符，或者双引号内的空格字符，是命令名称的一部分
        cmdName[loop] = *tmpStr++;
        loop++;
    }
    cmdName[loop] = '\0'; //命令名称的结尾符

    return cmdName;  //返回命令名称字符串
}


//执行命令
static void DoCmdExec(const char *cmdName, const char *cmdline, unsigned int len, const CmdParsed *cmdParsed)
{
    int ret;
    pid_t forkPid;

    if (strncmp(cmdline, SHELL_EXEC_COMMAND, SHELL_EXEC_COMMAND_BYTES) == 0) {
		//命令以 "exec "开头
		//则使用子进程来运行新命令
        forkPid = fork();
        if (forkPid < 0) {
            printf("Faild to fork from shell\n");
            return;
        } else if (forkPid == 0) {
			//子进程中
            ret = setpgrp();  //父子进程属于同一个进程组
            if (ret == -1) {
                exit(1);
            }

			//在子进程中执行新的程序，paramArray[0]为程序名，cmdParsed->paramArray为参数列表
            ret = execve((const char *)cmdParsed->paramArray[0], (char * const *)cmdParsed->paramArray, NULL);
            if (ret == -1) {
                perror("execve");
                exit(-1);
            }
        }
    } else {
    	//其他命令通过系统调用，在内核中执行
        (void)syscall(__NR_shellexec, cmdName, cmdline);
    }
}


//解析和执行命令
static void ParseAndExecCmdline(CmdParsed *cmdParsed, const char *cmdline, unsigned int len)
{
    int i;
    unsigned int ret;
    char shellWorkingDirectory[PATH_MAX + 1] = { 0 };
    char *cmdlineOrigin = NULL;
    char *cmdName = NULL;

	//先把命令行备份一下
    cmdlineOrigin = strdup(cmdline);
    if (cmdlineOrigin == NULL) {
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return;
    }

	//获取命令行的命令名称，即最前面的词组
    cmdName = GetCmdName(cmdline, len);
    if (cmdName == NULL) {
        free(cmdlineOrigin);
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return;
    }

	//根据命令行解析参数列表
    ret = OsCmdParse((char *)cmdline, cmdParsed);
    if (ret != SH_OK) {
        printf("cmd parse failure in %s[%d]\n", __FUNCTION__, __LINE__);
        goto OUT;
    }

	//执行命令，这里传入原始命令cmdlineOrigin，因为cmdline已经被拆分成参数列表了(其中的空格已经换成了空字符)
    DoCmdExec(cmdName, cmdlineOrigin, len, cmdParsed);

    if (getcwd(shellWorkingDirectory, PATH_MAX) != NULL) {
		//执行命令后，根据实际情况更新当前工作目录
        (void)OsShellSetWorkingDirtectory(shellWorkingDirectory, (PATH_MAX + 1));
    }

OUT:
	//执行完命令后，删除参数列表
    for (i = 0; i < cmdParsed->paramCnt; i++) {
        if (cmdParsed->paramArray[i] != NULL) {
            free(cmdParsed->paramArray[i]);
            cmdParsed->paramArray[i] = NULL;
        }
    }
	//释放命令名称
    free(cmdName);
	//以及备份的命令行
    free(cmdlineOrigin);

	//cmdline由调用者负责释放
}


//对命令行进行预处理
unsigned int PreHandleCmdline(const char *input, char **output, unsigned int *outputlen)
{
    unsigned int shiftLen, execLen, newLen;
    unsigned int removeLen = strlen("./"); /* "./" needs to be removed if it exists */
    unsigned int ret;
    char *newCmd = NULL;
    char *execCmd = SHELL_EXEC_COMMAND;
    const char *cmdBuf = input;
    unsigned int cmdBufLen = strlen(cmdBuf);	
    char *shiftStr = (char *)malloc(cmdBufLen + 1); //删除多余空格的命令不会比原来的命令更长
    errno_t err;

    if (shiftStr == NULL) {
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return SH_NOK;
    }
    (void)memset_s(shiftStr, cmdBufLen + 1, 0, cmdBufLen + 1);

    /* Call function 'OsCmdKeyShift' to squeeze and clear useless or overmuch space if string buffer */
	//删除多余的空格(重复的空格只保留1个，并删除命令行开始和结束位置的空格)，新命令字符串结果存入shiftStr
    ret = OsCmdKeyShift(cmdBuf, shiftStr, cmdBufLen + 1);
    shiftLen = strlen(shiftStr);
    if ((ret != SH_OK) || (shiftLen == 0)) {
        ret = SH_NOK;
        goto END_FREE_SHIFTSTR;
    }
	//删除多余空格后，一般就算预处理结果了
    *output = shiftStr;  
    *outputlen = shiftLen;

    /* Check and parse "./", located at the first two charaters of the cmd */
    if ((shiftLen > removeLen) && (shiftStr[0] == '.') && (shiftStr[1] == '/')) {
		//除非命令是以"./"开始
        execLen = strlen(execCmd);
		//这个时候"./command args ..."需要替换成"exec command args ..."
        newLen = execLen + shiftLen - removeLen; /* i.e., newLen - execLen == shiftLen - removeLen */
        newCmd = (char *)malloc(newLen + 1);
        if (newCmd == NULL) {
            ret = SH_NOK;
            printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
            goto END_FREE_SHIFTSTR;
        }

        err = memcpy_s(newCmd, newLen, execCmd, execLen); //先拷贝 "exec "到新字符串
        if (err != EOK) {
            printf("memcpy_s failure in %s[%d]\n", __FUNCTION__, __LINE__);
            ret = SH_NOK;
            goto END_FREE_NEWCMD;
        } 

		//然后拷贝"./"之后的部分到"exec "之后
        err = memcpy_s(newCmd + execLen, newLen - execLen, shiftStr + removeLen, shiftLen - removeLen);
        if (err != EOK) {
            printf("memcpy_s failure in %s[%d]\n", __FUNCTION__, __LINE__);
            ret = SH_NOK;
            goto END_FREE_NEWCMD;
        }
        newCmd[newLen] = '\0'; //记住最后的字符串结尾

		//刷新结果
        *output = newCmd;
        *outputlen = newLen;
        ret = SH_OK;
        goto END_FREE_SHIFTSTR;
    } else {
        ret = SH_OK;
        goto END;
    }
END_FREE_NEWCMD:
    free(newCmd);
END_FREE_SHIFTSTR:
    free(shiftStr);
END:
    return ret;
}


//执行字符串对应的命令
static void ExecCmdline(const char *cmdline)
{
    unsigned int ret;
    char *output = NULL;
    unsigned int outputlen;
    CmdParsed cmdParsed;

    if (cmdline == NULL) {
        return;
    }

    /* strip out unnecessary characters */
	//先对命令进行预处理，即去除多余空格，和处理"./"情况
    ret = PreHandleCmdline(cmdline, &output, &outputlen);
    if (ret == SH_NOK) {
        return;
    }

    (void)memset_s(&cmdParsed, sizeof(CmdParsed), 0, sizeof(CmdParsed));
	//然后解析和执行命令
    ParseAndExecCmdline(&cmdParsed, output, outputlen);
    free(output); //最后释放预处理后的命令行字符串
}


//回收僵尸状态的子进程，这些子进程就是shell在执行"./xxx"这样的命令生成的
void RecycleZombieChild(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        continue;
    }
}


//shell命令处理线程的主逻辑
static void ShellCmdProcess(ShellCB *shellCB)
{
    char *buf = NULL;
    while (1) {
        /* recycle zombine child process */
        RecycleZombieChild(); //先回收僵尸子进程
        buf = GetCmdline(shellCB); //读取另一个线程放入的命令
        if (buf == NULL) {
            break;  //没有命令，则本函数退出
        }

        ExecCmdline(buf);  //执行这个命令
        ShellSaveHistoryCmd(buf, shellCB); //将此命令保存为历史命令
        shellCB->cmdMaskKeyLink = shellCB->cmdHistoryKeyLink; //更新历史命令游标
        printf(SHELL_PROMPT); //在界面上显示命令提示符
    }
}


//shell命令处理线程
void *ShellTask(void *argv)
{
    int ret;
    ShellCB *shellCB = (ShellCB *)argv;

    if (shellCB == NULL) {
        return NULL;
    }

	//先给线程取个名字
    ret = prctl(PR_SET_NAME, "ShellTask");
    if (ret != SH_OK) {
        return NULL;
    }

    printf(SHELL_PROMPT);  //首先显示命令行提示
    while (1) {
		//循环等待另一个线程来唤醒，对方会存放命令到队列中，然后唤醒我
        ret = ShellPend(shellCB);
        if (ret == SH_OK) {
			//从命令队列里面取走命令并执行命令
            ShellCmdProcess(shellCB);
        } else if (ret != SH_OK) {
            break; //基本上不会走到这个逻辑来
        }
    }

    return NULL;
}


//创建shell命令执行线程
int ShellTaskInit(ShellCB *shellCB)
{
    unsigned int ret;
    size_t stackSize = SHELL_TASK_STACKSIZE;
    void *arg = NULL;
    pthread_attr_t attr;

    if (shellCB == NULL) {
        return SH_NOK;
    }

    ret = pthread_attr_init(&attr);
    if (ret != SH_OK) {
        return SH_NOK;
    }

    pthread_attr_setstacksize(&attr, stackSize); //设置线程栈尺寸
    arg = (void *)shellCB;
	//并创建线程
    ret = pthread_create(&shellCB->shellTaskHandle, &attr, &ShellTask, arg);
    if (ret != SH_OK) {
        return SH_NOK;
    }

    return ret;
}


//通知内核我是命令行解析线程
static int ShellKernelReg(unsigned int shellHandle)
{
    return ioctl(0, CONSOLE_CONTROL_REG_USERTASK, shellHandle);
}


//命令行解析线程主函数
void *ShellEntry(void *argv)
{
    char ch;
    int ret;
    int n;
    pid_t tid = syscall(__NR_gettid);
    ShellCB *shellCB = (ShellCB *)argv;

    if (shellCB == NULL) {
        return NULL;
    }

    (void)memset_s(shellCB->shellBuf, SHOW_MAX_LEN, 0, SHOW_MAX_LEN);

    ret = prctl(PR_SET_NAME, "ShellEntry");
    if (ret != SH_OK) {
        return NULL;
    }

	//将线程ID通知内核，我是shell处理线程
    ret = ShellKernelReg((int)tid);
    if (ret != 0) {
		//系统只能支持一个命令行解析线程
        printf("another shell is already running!\n");
        exit(-1);
    }

    while (1) {
        /* is console ready for shell ? */
        if (ret != SH_OK)
            break;

        n = read(0, &ch, 1); //从标准输入读入一个字符
        if (n == 1) {
			//处理用户输入的这个字符
            ShellCmdLineParse(ch, (OutputFunc)printf, shellCB);
        }
    }
    return NULL;
}


//创建shell命令处理线程
int ShellEntryInit(ShellCB *shellCB)
{
    int ret;
    size_t stackSize = SHELL_ENTRY_STACKSIZE;
    void *arg = NULL;
    pthread_attr_t attr;

    if (shellCB == NULL) {
        return SH_NOK;
    }

    ret = pthread_attr_init(&attr);
    if (ret != SH_OK) {
        return SH_NOK;
    }

    pthread_attr_setstacksize(&attr, stackSize); //设置此线程的栈尺寸
    arg = (void *)shellCB;
	//创建shell入口线程
    ret = pthread_create(&shellCB->shellEntryHandle, &attr, &ShellEntry, arg);
    if (ret != SH_OK) {
        return SH_NOK;
    }

    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
