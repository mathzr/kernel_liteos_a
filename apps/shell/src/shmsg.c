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


//�����������ȡ��һ����ִ�е�����
char *GetCmdline(ShellCB *shellCB)
{
    CmdKeyLink *cmdkey = shellCB->cmdKeyLink;
    CmdKeyLink *cmdNode = NULL;

    (void)pthread_mutex_lock(&shellCB->keyMutex);
    if ((cmdkey == NULL) || SH_ListEmpty(&cmdkey->list)) {
        (void)pthread_mutex_unlock(&shellCB->keyMutex);
        return NULL;
    }

    cmdNode = SH_LIST_ENTRY(cmdkey->list.pstNext, CmdKeyLink, list);
    SH_ListDelete(&(cmdNode->list));
    (void)pthread_mutex_unlock(&shellCB->keyMutex);

    return cmdNode->cmdString;
}


//������ִ�к󣬽���ǰ�����Ϊ��ʷ����
static void ShellSaveHistoryCmd(char *string, ShellCB *shellCB)
{
    CmdKeyLink *cmdHistory = shellCB->cmdHistoryKeyLink;
    CmdKeyLink *cmdkey = SH_LIST_ENTRY(string, CmdKeyLink, cmdString);
    CmdKeyLink *cmdNxt = NULL;

    if ((string == NULL) || (*string == '\n') || (strlen(string) == 0)) {
        return;  //����������ã�û�б�Ҫ����
    }

    (void)pthread_mutex_lock(&shellCB->historyMutex);
    if (cmdHistory->count != 0) {
		//�Ѵ�����ʷ����������
        cmdNxt = SH_LIST_ENTRY(cmdHistory->list.pstPrev, CmdKeyLink, list);
        if (strcmp(string, cmdNxt->cmdString) == 0) {
			//�ظ�������ͬ�����û������
            free((void *)cmdkey);
            (void)pthread_mutex_unlock(&shellCB->historyMutex);
            return;
        }
    }

    if (cmdHistory->count == CMD_HISTORY_LEN) {
		//��ʷ����������������(����ͷ��)�������ӵ�
        cmdNxt = SH_LIST_ENTRY(cmdHistory->list.pstNext, CmdKeyLink, list);
        SH_ListDelete(&(cmdNxt->list));
		//������������β��
        SH_ListTailInsert(&(cmdHistory->list), &(cmdkey->list));
        free((void *)cmdNxt);
        (void)pthread_mutex_unlock(&shellCB->historyMutex);
        return;
    }

	//������������β��
    SH_ListTailInsert(&(cmdHistory->list), &(cmdkey->list));
    cmdHistory->count++; //��ʷ������Ŀ����

    (void)pthread_mutex_unlock(&shellCB->historyMutex);
    return;
}


//�ȴ�����һ���̴߳�������
int ShellPend(ShellCB *shellCB)
{
    if (shellCB == NULL) {
        return SH_NOK;
    }

    return sem_wait(&shellCB->shellSem);
}

//֪ͨ��һ���߳�ȡ����
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


//�����û�����ķ����������Ĵ���
//��Ҫ�����·��������ʷ�������
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
            OsShellHistoryShow(CMD_KEY_UP, shellCB);
            shellCB->shellKeyType = STAT_NOMAL_KEY;
            return ret;
        }
    } else if (ch == 0x42) { /* down */
        if (shellCB->shellKeyType == STAT_MULTI_KEY) {
            shellCB->shellKeyType = STAT_NOMAL_KEY;
            OsShellHistoryShow(CMD_KEY_DOWN, shellCB);
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

//����ǰ�����������������֪ͨ����һ���߳���ȡ
void ShellTaskNotify(ShellCB *shellCB)
{
    int ret;

    (void)pthread_mutex_lock(&shellCB->keyMutex);
    OsShellCmdPush(shellCB->shellBuf, shellCB->cmdKeyLink);
    (void)pthread_mutex_unlock(&shellCB->keyMutex);

    ret = ShellNotify(shellCB);
    if (ret != SH_OK) {
        printf("command execute failed, \"%s\"", shellCB->shellBuf);
    }
}

//��������Ļس���
void ParseEnterKey(OutputFunc outputFunc, ShellCB *shellCB)
{
    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if (shellCB->shellBufOffset == 0) {
		//������ֻ��һ���س�����������б�Ҫ֪ͨ��һ���̴߳�����?
		//��ĳ������˵��֪ͨ����һ���̣߳�����������ģ���Ϊ�������������ܻ�����֮ǰ������
		//ͨ�������Ǹ��̣߳�Ҳ������������ĴӶ�������ȡ�����ִ��
        shellCB->shellBuf[shellCB->shellBufOffset] = '\n';
        shellCB->shellBuf[shellCB->shellBufOffset + 1] = '\0';
        goto NOTIFY;
    }

    if (shellCB->shellBufOffset <= (SHOW_MAX_LEN - 1)) {
		//�س���Ԥʾ�������н���
        shellCB->shellBuf[shellCB->shellBufOffset] = '\0';
    }
NOTIFY:
    outputFunc("\n"); //�ڽ�����ʾ���з�
    shellCB->shellBufOffset = 0;  //��һ�������0��λ�ÿ�ʼ����
    ShellTaskNotify(shellCB);  //֪ͨ����һ���̴߳�������
}


//����ɾ���ַ�
void ParseDeleteKey(OutputFunc outputFunc, ShellCB *shellCB)
{
    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if ((shellCB->shellBufOffset > 0) && (shellCB->shellBufOffset <= (SHOW_MAX_LEN - 1))) {
		//��ԭ�����л��������һ���ַ����ǳɿ��ַ�������ɾ���ַ���Ч��
        shellCB->shellBuf[shellCB->shellBufOffset - 1] = '\0';
		//�����ַ������ȼ���
        shellCB->shellBufOffset--;
        outputFunc("\b \b"); //�ڽ������ɾ�����һ���ַ���Ч��
    }
}


//����tab����
void ParseTabKey(OutputFunc outputFunc, ShellCB *shellCB)
{
    int ret;

    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if ((shellCB->shellBufOffset > 0) && (shellCB->shellBufOffset < (SHOW_MAX_LEN - 1))) {
		//tab�����µ��������������е��Զ���ȫ
        ret = OsTabCompletion(shellCB->shellBuf, &shellCB->shellBufOffset);
        if (ret > 1) {
			//���Զ���ȫ������ж��ƥ��������ô��Щ���Ҳ��õ���ʾ
			//��󣬻���Ҫ����ȫ�����������ʾһ�飬�û����Լ�������ʣ�µ���Ҫ�ֶ���ȫ������
            outputFunc(SHELL_PROMPT"%s", shellCB->shellBuf);
        }
    }
}


//�������ַ�������
void ParseNormalChar(char ch, OutputFunc outputFunc, ShellCB *shellCB)
{
    if ((shellCB == NULL) || (outputFunc == NULL)) {
        return;
    }

    if ((ch != '\0') && (shellCB->shellBufOffset < (SHOW_MAX_LEN - 1))) {
		//�����ַ���¼�������л���
        shellCB->shellBuf[shellCB->shellBufOffset] = ch;
        shellCB->shellBufOffset++;
		//����ʾ�ڽ����ϼ���
        outputFunc("%c", ch);
    }

    shellCB->shellKeyType = STAT_NOMAL_KEY;
}


//����һ���ַ��Ĵ������
//�ڲ����ݲ�ͬ���ַ�����ͬ�ľ��崦��
void ShellCmdLineParse(char c, OutputFunc outputFunc, ShellCB *shellCB)
{
    const char ch = c;
    int ret;

    if ((shellCB->shellBufOffset == 0) && (ch != '\n') && (ch != '\0')) {
        (void)memset_s(shellCB->shellBuf, SHOW_MAX_LEN, 0, SHOW_MAX_LEN);
    }

    switch (ch) {
        case '\r':
        case '\n': /* enter */
            ParseEnterKey(outputFunc, shellCB);
            break;
        case '\b': /* backspace */
        case 0x7F: /* delete(0x7F) */
            ParseDeleteKey(outputFunc, shellCB);
            break;
        case '\t': /* tab */
            ParseTabKey(outputFunc, shellCB);
            break;
        default:
            /* parse the up/down/right/left key */
            ret = ShellCmdLineCheckUDRL(ch, shellCB);
            if (ret == SH_OK) {
                return;
            }
            ParseNormalChar(ch, outputFunc, shellCB);
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


//��ȡ���������, ���������ʼ�Ĵ���
char *GetCmdName(const char *cmdline, unsigned int len)
{
    unsigned int loop;
    const char *tmpStr = NULL;
    bool quotes = FALSE; //��ʼ״̬Ϊ˫������
    char *cmdName = NULL;
    if (cmdline == NULL) {
        return NULL;
    }

	//������һ���ڴ����洢���������
	//������ƿ϶����ᳬ��ԭ���������г���
    cmdName = (char *)malloc(len + 1);
    if (cmdName == NULL) {
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return NULL;
    }

    /* Scan the 'cmdline' string for command */
    /* Notice: Command string must not have any special name */
	//ɨ��������
    for (tmpStr = cmdline, loop = 0; (*tmpStr != '\0') && (loop < len); ) {
        /* If reach a double quotes, switch the quotes matching status */
        if (*tmpStr == '\"') {
            SWITCH_QUOTES_STATUS(quotes); //�л���ǰ״̬Ϊ˫������or˫������
            /* Ignore the double quote charactor itself */
            tmpStr++; //�������Ʋ�����˫����
            continue;
        }
        /* If detected a space which the quotes matching status is false */
        /* which said has detected the first space for seperator, finish this scan operation */
        if ((*tmpStr == ' ') && (QUOTES_STATUS_CLOSE(quotes))) {
            break;  //��˫�������������ո�ʱ��˵���������ƽ���
        }
		//�ǿո��ַ�������˫�����ڵĿո��ַ������������Ƶ�һ����
        cmdName[loop] = *tmpStr++;
        loop++;
    }
    cmdName[loop] = '\0'; //�������ƵĽ�β��

    return cmdName;
}


//ִ������
static void DoCmdExec(const char *cmdName, const char *cmdline, unsigned int len, const CmdParsed *cmdParsed)
{
    int ret;
    pid_t forkPid;

    if (strncmp(cmdline, SHELL_EXEC_COMMAND, SHELL_EXEC_COMMAND_BYTES) == 0) {
		//������ "exec "��ͷ
		//��ʹ���ӽ���������������
        forkPid = fork();
        if (forkPid < 0) {
            printf("Faild to fork from shell\n");
            return;
        } else if (forkPid == 0) {
			//�ӽ�����
            ret = setpgrp();  //���ӽ�������ͬһ��������
            if (ret == -1) {
                exit(1);
            }

			//���ӽ�����ִ���µĳ���paramArray[0]Ϊ��������cmdParsed->paramArrayΪ�����б�
            ret = execve((const char *)cmdParsed->paramArray[0], (char * const *)cmdParsed->paramArray, NULL);
            if (ret == -1) {
                perror("execve");
                exit(-1);
            }
        }
    } else {
    	//���������ڵ�ǰ���������У�����ͨ��ϵͳ���ã����ں���ִ��
        (void)syscall(__NR_shellexec, cmdName, cmdline);
    }
}


//������ִ������
static void ParseAndExecCmdline(CmdParsed *cmdParsed, const char *cmdline, unsigned int len)
{
    int i;
    unsigned int ret;
    char shellWorkingDirectory[PATH_MAX + 1] = { 0 };
    char *cmdlineOrigin = NULL;
    char *cmdName = NULL;

	//�Ȱ������б���һ��
    cmdlineOrigin = strdup(cmdline);
    if (cmdlineOrigin == NULL) {
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return;
    }

	//��ȡ�����е��������ƣ�����ǰ��Ĵ����ַ���
    cmdName = GetCmdName(cmdline, len);
    if (cmdName == NULL) {
        free(cmdlineOrigin);
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return;
    }

	//���������н����ɲ����б�
    ret = OsCmdParse((char *)cmdline, cmdParsed);
    if (ret != SH_OK) {
        printf("cmd parse failure in %s[%d]\n", __FUNCTION__, __LINE__);
        goto OUT;
    }

	//ִ������
    DoCmdExec(cmdName, cmdlineOrigin, len, cmdParsed);

    if (getcwd(shellWorkingDirectory, PATH_MAX) != NULL) {
		//ִ������󣬸���ʵ��������µ�ǰ����Ŀ¼
        (void)OsShellSetWorkingDirtectory(shellWorkingDirectory, (PATH_MAX + 1));
    }

OUT:
	//ִ���������ɾ�������б�
    for (i = 0; i < cmdParsed->paramCnt; i++) {
        if (cmdParsed->paramArray[i] != NULL) {
            free(cmdParsed->paramArray[i]);
            cmdParsed->paramArray[i] = NULL;
        }
    }
	//�ͷ���������
    free(cmdName);
	//�Լ����ݵ�������
    free(cmdlineOrigin);
}


//�������н���Ԥ����
unsigned int PreHandleCmdline(const char *input, char **output, unsigned int *outputlen)
{
    unsigned int shiftLen, execLen, newLen;
    unsigned int removeLen = strlen("./"); /* "./" needs to be removed if it exists */
    unsigned int ret;
    char *newCmd = NULL;
    char *execCmd = SHELL_EXEC_COMMAND;
    const char *cmdBuf = input;
    unsigned int cmdBufLen = strlen(cmdBuf);	
    char *shiftStr = (char *)malloc(cmdBufLen + 1); //ɾ������ո��������ԭ�����������
    errno_t err;

    if (shiftStr == NULL) {
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return SH_NOK;
    }
    (void)memset_s(shiftStr, cmdBufLen + 1, 0, cmdBufLen + 1);

    /* Call function 'OsCmdKeyShift' to squeeze and clear useless or overmuch space if string buffer */
	//ɾ������Ŀո��������ַ����������shiftStr
    ret = OsCmdKeyShift(cmdBuf, shiftStr, cmdBufLen + 1);
    shiftLen = strlen(shiftStr);
    if ((ret != SH_OK) || (shiftLen == 0)) {
        ret = SH_NOK;
        goto END_FREE_SHIFTSTR;
    }
	//ɾ������ո��һ�����Ԥ��������
    *output = shiftStr;  
    *outputlen = shiftLen;

    /* Check and parse "./", located at the first two charaters of the cmd */
    if ((shiftLen > removeLen) && (shiftStr[0] == '.') && (shiftStr[1] == '/')) {
		//������������"./"��ʼ
        execLen = strlen(execCmd);
		//���ʱ��"./command args ..."��Ҫ�滻��"exec command args ..."
        newLen = execLen + shiftLen - removeLen; /* i.e., newLen - execLen == shiftLen - removeLen */
        newCmd = (char *)malloc(newLen + 1);
        if (newCmd == NULL) {
            ret = SH_NOK;
            printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
            goto END_FREE_SHIFTSTR;
        }

        err = memcpy_s(newCmd, newLen, execCmd, execLen); //�ȿ��� "exec "�����ַ���
        if (err != EOK) {
            printf("memcpy_s failure in %s[%d]\n", __FUNCTION__, __LINE__);
            ret = SH_NOK;
            goto END_FREE_NEWCMD;
        } 

		//Ȼ�󿽱�"./"֮��Ĳ��ֵ�"exec "֮��
        err = memcpy_s(newCmd + execLen, newLen - execLen, shiftStr + removeLen, shiftLen - removeLen);
        if (err != EOK) {
            printf("memcpy_s failure in %s[%d]\n", __FUNCTION__, __LINE__);
            ret = SH_NOK;
            goto END_FREE_NEWCMD;
        }
        newCmd[newLen] = '\0'; //��ס�����ַ�����β

		//ˢ�½��
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


//ִ���ַ�����Ӧ������
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
	//�ȶ��������Ԥ������ȥ������ո񣬺ʹ���"./"���
    ret = PreHandleCmdline(cmdline, &output, &outputlen);
    if (ret == SH_NOK) {
        return;
    }

    (void)memset_s(&cmdParsed, sizeof(CmdParsed), 0, sizeof(CmdParsed));
	//Ȼ�������ִ������
    ParseAndExecCmdline(&cmdParsed, output, outputlen);
    free(output);
}


//���ս�ʬ״̬���ӽ��̣���Щ�ӽ��̾���shell��ִ��"./xxx"�������������ɵ�
void RecycleZombieChild(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        continue;
    }
}


//shell������̵߳����߼�
static void ShellCmdProcess(ShellCB *shellCB)
{
    char *buf = NULL;
    while (1) {
        /* recycle zombine child process */
        RecycleZombieChild(); //�Ȼ��ս�ʬ�ӽ���
        buf = GetCmdline(shellCB); //��ȡ��һ���̷߳��������
        if (buf == NULL) {
            break;
        }

        ExecCmdline(buf);  //ִ���������
        ShellSaveHistoryCmd(buf, shellCB); //���������Ϊ��ʷ����
        shellCB->cmdMaskKeyLink = shellCB->cmdHistoryKeyLink; //������ʷ�����α�
        printf(SHELL_PROMPT); //�ڽ�������ʾ������ʾ��
    }
}


//shell������߳�
void *ShellTask(void *argv)
{
    int ret;
    ShellCB *shellCB = (ShellCB *)argv;

    if (shellCB == NULL) {
        return NULL;
    }

	//��ȡ������
    ret = prctl(PR_SET_NAME, "ShellTask");
    if (ret != SH_OK) {
        return NULL;
    }

    printf(SHELL_PROMPT);  //������ʾ��������ʾ
    while (1) {
		//ѭ���ȴ���һ���߳������ѣ��Է�������������У�Ȼ������
        ret = ShellPend(shellCB);
        if (ret == SH_OK) {
			//�������������ȡ�����ִ������
            ShellCmdProcess(shellCB);
        } else if (ret != SH_OK) {
            break;
        }
    }

    return NULL;
}


//����shell����ִ���߳�
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

    pthread_attr_setstacksize(&attr, stackSize);
    arg = (void *)shellCB;
    ret = pthread_create(&shellCB->shellTaskHandle, &attr, &ShellTask, arg);
    if (ret != SH_OK) {
        return SH_NOK;
    }

    return ret;
}


//֪ͨ�ں����������н����߳�
static int ShellKernelReg(unsigned int shellHandle)
{
    return ioctl(0, CONSOLE_CONTROL_REG_USERTASK, shellHandle);
}


//�����н����߳�������
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

	//���߳�ID֪ͨ�ںˣ�����shell�����߳�
    ret = ShellKernelReg((int)tid);
    if (ret != 0) {
        printf("another shell is already running!\n");
        exit(-1);
    }

    while (1) {
        /* is console ready for shell ? */
        if (ret != SH_OK)
            break;

        n = read(0, &ch, 1);
        if (n == 1) {
			//�����û����룬��һ�������֪ͨ��һ���̼߳�������
            ShellCmdLineParse(ch, (OutputFunc)printf, shellCB);
        }
    }
    return NULL;
}


//����shell������߳�
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

    pthread_attr_setstacksize(&attr, stackSize);
    arg = (void *)shellCB;
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
