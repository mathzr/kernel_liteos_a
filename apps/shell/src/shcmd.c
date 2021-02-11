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

#include "shcmd.h"
#include "show.h"
#include "stdlib.h"
#include "unistd.h"
#include "dirent.h"
#include "securec.h"

#ifdef  __cplusplus
#if  __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

#define SHELL_INIT_MAGIC_FLAG 0xABABABAB
#define CTRL_C 0x03 /* 0x03: ctrl+c ASCII */


//释放解析后的命令行参数
static void OsFreeCmdPara(CmdParsed *cmdParsed)
{
    unsigned int i;
    for (i = 0; i < cmdParsed->paramCnt; i++) {
        if ((cmdParsed->paramArray[i]) != NULL) {
            free((cmdParsed->paramArray[i]));
            cmdParsed->paramArray[i] = NULL;
        }
    }
}


//获取tab按键前最后一个字符串(字符串分隔)
/* 输入参数：
 * *tabStr 整个命令字符串
 * parsed 解析后的字符串数组(参数列表)
 * tabStrLen 命令字符串长度
 * 输出参数：*tabStr tab按键前最后一个字符串，基于这个字符串来做自动补全
 * 返回值： 是否成功
 * 注意：本函数在用户输入TAB键时调用，用于辅助进行自动补全
 */
static int OsStrSeparateTabStrGet(const char **tabStr, CmdParsed *parsed, unsigned int tabStrLen)
{
    char *shiftStr = NULL;
	//申请2个命令行的长度
    char *tempStr = (char *)malloc(SHOW_MAX_LEN << 1);
    if (tempStr == NULL) {
        return (int)SH_ERROR;
    }

    (void)memset_s(tempStr, SHOW_MAX_LEN << 1, 0, SHOW_MAX_LEN << 1);
	//tempStr和shiftStr各占一半的空间
    shiftStr = tempStr + SHOW_MAX_LEN;

	//tempStr存放原始命令
    if (strncpy_s(tempStr, SHOW_MAX_LEN - 1, *tabStr, tabStrLen)) {
        free(tempStr);
        return (int)SH_ERROR;
    }

    parsed->cmdType = CMD_TYPE_STD;  //标准命令

    /* cut useless or repeat space */
	//删除多余和重复空格后的命令存入shiftStr
    if (OsCmdKeyShift(tempStr, shiftStr, SHOW_MAX_LEN - 1)) {
        free(tempStr);
        return (int)SH_ERROR;
    }

    /* get exact position of string to complete */
    /* situation different if end space lost or still exist */
    if ((strlen(shiftStr) == 0) || (tempStr[strlen(tempStr) - 1] != shiftStr[strlen(shiftStr) - 1])) {
		//空命令行或者以空格结尾的命令行
        *tabStr = "";  //则未来基于空字符串进行命令自动补全
    } else {
    	//其它情况则以空格分割命令行，切割成参数列表
        if (OsCmdTokenSplit(shiftStr, ' ', parsed)) {
            free(tempStr);
            return (int)SH_ERROR;
        }
		//基于最后一个参数来做自动补全
        *tabStr = parsed->paramArray[parsed->paramCnt - 1];
    }

    free(tempStr);
    return SH_OK;
}


//shell程序中也记录一下当前工作目录
char *OsShellGetWorkingDirtectory()
{
    return OsGetShellCb()->shellWorkingDirectory;
}

//更新shell程序中的当前工作目录
int OsShellSetWorkingDirtectory(const char *dir, size_t len)
{
    if (dir == NULL) {
        return SH_NOK;
    }

    int ret = strncpy_s(OsGetShellCb()->shellWorkingDirectory, sizeof(OsGetShellCb()->shellWorkingDirectory),
                        dir, len);
    if (ret != SH_OK) {
        return SH_NOK;
    }
    return SH_OK;
}

/*
 * 从命令行中拆分成目录名和需要匹配的字符串
 * 输入参数:
 * tabStr：完整命令行参数
 * tabStrLen: 命令行参数长度
 * 输出参数：
 * strPath: 需要搜索的目录
 * nameLooking: 此目录下需要搜索的名称
 * 返回值：成功or失败
 */
static int OsStrSeparate(const char *tabStr, char *strPath, char *nameLooking, unsigned int tabStrLen)
{
    char *strEnd = NULL;
    char *cutPos = NULL;
    CmdParsed parsed = {0};
    char *shellWorkingDirectory = OsShellGetWorkingDirtectory();
    int ret;

	//求出最后一个空格到TAB键之间的字符串
    ret = OsStrSeparateTabStrGet(&tabStr, &parsed, tabStrLen);
    if (ret != SH_OK) {
        return ret;
    }

    /* get fullpath str */
    if (*tabStr != '/') {
		//如果不是绝对路径，则补充成绝对路径(先记录当前工作目录--相对路径指相对于当前工作目录的路径)
        if (strncpy_s(strPath, CMD_MAX_PATH, shellWorkingDirectory, CMD_MAX_PATH - 1)) {
            OsFreeCmdPara(&parsed);
            return (int)SH_ERROR;
        }
		//如果当前工作目录不是根目录，则还需要在当前工作目录和相对路径中间补充一个分隔符
        if (strcmp(shellWorkingDirectory, "/")) {
            if (strncat_s(strPath, CMD_MAX_PATH - 1, "/", CMD_MAX_PATH - strlen(strPath) - 1)) {
                OsFreeCmdPara(&parsed);
                return (int)SH_ERROR;
            }
        }
    }

	//记录绝对路径
	//或者拼接当前工作路径和相对路径形成绝对路径
    if (strncat_s(strPath, CMD_MAX_PATH - 1, tabStr, CMD_MAX_PATH - strlen(strPath) - 1)) {
        OsFreeCmdPara(&parsed);
        return (int)SH_ERROR;
    }

    /* split str by last '/' */
    strEnd = strrchr(strPath, '/');
    if (strEnd != NULL) {
		//拷贝最后一个/字符和TAB键之间的字符串
        if (strncpy_s(nameLooking, CMD_MAX_PATH, strEnd + 1, CMD_MAX_PATH - 1)) { /* get cmp str */
            OsFreeCmdPara(&parsed);
            return (int)SH_ERROR;
        }
    }

	//在最后一个/字符后面放入空字符，即strPath以/结尾，例如/usr/lib/
    cutPos = strrchr(strPath, '/');
    if (cutPos != NULL) {
        *(cutPos + 1) = '\0';
    }

    OsFreeCmdPara(&parsed);
    return SH_OK;
}


//翻页控制
static int OsShowPageInputControl(void)
{
    char readChar;

    while (1) {
        if (read(STDIN_FILENO, &readChar, 1) != 1) { /* get one char from stdin */
            printf("\n");
            return (int)SH_ERROR;
        }
		//如果用户键入q或者Q或者CTRL+C, 则换行后退出显示
        if ((readChar == 'q') || (readChar == 'Q') || (readChar == CTRL_C)) {
            printf("\n");
            return 0;
        } else if (readChar == '\r') {  //用户键入了回车键以后，则把 --more-- 那8个字符删除，并允许显示下一页
            printf("\b \b\b \b\b \b\b \b\b \b\b \b\b \b\b \b");
            return 1;
        }
    }
}

//多页显示控制
static int OsShowPageControl(unsigned int timesPrint, unsigned int lineCap, unsigned int count)
{
    if (NEED_NEW_LINE(timesPrint, lineCap)) {
		//需要换行的情况下，换一行
        printf("\n");
        if (SCREEN_IS_FULL(timesPrint, lineCap) && (timesPrint < count)) {
            printf("--More--");  //需要换页的情况下，输出提示
            return OsShowPageInputControl(); //然后等待用户来控制
        }
    }
    return 1;  //其它情况允许继续显示
}


//在预测到需要显示的内容会超过1页时，
//提示用户是否显示所有的匹配
static int OsSurePrintAll(unsigned int count)
{
    char readChar = 0;
    printf("\nDisplay all %u possibilities?(y/n)", count);  //显示提示信息
    while (1) {
        if (read(0, &readChar, 1) != 1) {
            return (int)SH_ERROR;
        }
		//用户输入n或者N或者CTRL+C，则不显示
        if ((readChar == 'n') || (readChar == 'N') || (readChar == CTRL_C)) {
            printf("\n");
            return 0;
        } else if ((readChar == 'y') || (readChar == 'Y') || (readChar == '\r')) {
            return 1; //用户输入y或者Y或者回车，则允许显示
        }
    }
}


//显示所有匹配的项目
/*
 * count : 总共需要显示的项目数
 * strPath : 目录路径
 * nameLooking: 需要匹配的字符串
 * printLen : 每个项目显示的宽度
 */
static int OsPrintMatchList(unsigned int count, const char *strPath, const char *nameLooking, unsigned int printLen)
{
    unsigned int timesPrint = 0;
    unsigned int lineCap;
    int ret;
    DIR *openDir = NULL;
    struct dirent *readDir = NULL;
	//格式字符串的宽度，即"%-xxxs          " 的长度，
    char formatChar[10] = {0}; /* 10:for formatChar length */

	//保留2个字符的原因是，每个显示项目末尾额外显示2个空格
	//这样才能清楚的辨别不同的显示项目
	//每个显示项目不能超过1行
    printLen = (printLen > (DEFAULT_SCREEN_WIDTH - 2)) ? (DEFAULT_SCREEN_WIDTH - 2) : printLen; /* 2:revered 2 bytes */
	//每个显示项目预留2个空字符，每行最多显示lineCap个项目
    lineCap = DEFAULT_SCREEN_WIDTH / (printLen + 2); /* 2:DEFAULT_SCREEN_WIDTH revered 2 bytes */
	//每个显示项目的格式字符串存入formatChar中
    if (snprintf_s(formatChar, sizeof(formatChar) - 1, 7, "%%-%us  ", printLen) < 0) { /* 7:format-len */
        return (int)SH_ERROR;
    }

    if (count > (lineCap * DEFAULT_SCREEN_HEIGNT)) {
		//所有的显示项目会超过1屏的情况，需要向用户提问是否继续显示
        ret = OsSurePrintAll(count);
        if (ret != 1) {
            return ret;
        }
    }
    openDir = opendir(strPath);  //先打开项目所在的目录
    if (openDir == NULL) {
        return (int)SH_ERROR;
    }

    printf("\n");
	//遍历目录中的每一个项目
    for (readDir = readdir(openDir); readDir != NULL; readDir = readdir(openDir)) {		
        if (strncmp(nameLooking, readDir->d_name, strlen(nameLooking)) != 0) {
            continue;
        }
		//如果匹配我们的名称，那么进行显示
        printf(formatChar, readDir->d_name);
        timesPrint++;
		//进行换行显示和换页控制
        ret = OsShowPageControl(timesPrint, lineCap, count);
        if (ret != 1) {
			//用户换页控制时退出了，则关闭目录并退出
            if (closedir(openDir) < 0) {
                return (int)SH_ERROR;
            }
            return ret;
        }
    }

    printf("\n");
	//显示完成，也关闭目录退出
    if (closedir(openDir) < 0) {
        return (int)SH_ERROR;
    }

    return SH_OK;
}

//对2个字符串比较前n个字符
//针对第2个字符串，只保留2个字符串相同的部分
static void StrncmpCut(const char *s1, char *s2, size_t n)
{
    if ((n == 0) || (s1 == NULL) || (s2 == NULL)) {
        return;
    }
    do {
        if (*s1 && *s2 && (*s1 == *s2)) {
            s1++;
            s2++;
        } else {
            break;
        }
    } while (--n != 0);
    if (n > 0) {
        /* NULL pad the remaining n-1 bytes */
        while (n-- != 0) {
            *s2++ = 0;
        }
    }
    return;
}


/*
 * 自动补全命令行
 * result : 补全后的字符串
 * target : 补全前的字符串
 * cmdKey : 命令行缓存
 * *len : 补全前的命令行缓存长度
 * 输出值 : *len 补全后的命令行缓存长度
 *          cmdKey 命令行缓存
 */
static void OsCompleteStr(char *result, const char *target, char *cmdKey, unsigned int *len)
{
    unsigned int size = strlen(result) - strlen(target);  //需要自动补充的字符数
    char *des = cmdKey + *len;  //当前需要补全字符的位置
    char *src = result + strlen(target); //结果字符串数据自动补全位置

    while (size-- > 0) {
        printf("%c", *src);  //将补全的字符显示在屏幕上
        if (*len == (SHOW_MAX_LEN - 1)) {
            *des = '\0';
            break;
        }
        *des++ = *src++;  //需要补全的字符存入命令缓存
        (*len)++;
    }
}


//执行TAB键自动匹配查找
static int OsExecNameMatch(const char *strPath, const char *nameLooking, char *strObj, unsigned int *maxLen)
{
    int count = 0;
    DIR *openDir = NULL;
    struct dirent *readDir = NULL;

    openDir = opendir(strPath);  //打开TAB键之前的这个目录
    if (openDir == NULL) {
        return (int)SH_ERROR;
    }

	//遍历上述目录
    for (readDir = readdir(openDir); readDir != NULL; readDir = readdir(openDir)) {
        if (strncmp(nameLooking, readDir->d_name, strlen(nameLooking)) != 0) {
            continue;
        }
		//寻找nameLooking匹配的项
        if (count == 0) {
			//将最开始匹配的项纪录下来
            if (strncpy_s(strObj, CMD_MAX_PATH, readDir->d_name, CMD_MAX_PATH - 1)) {
                (void)closedir(openDir);
                return (int)SH_ERROR;
            }
			//它的长度也记录下来
            *maxLen = strlen(readDir->d_name);
        } else {
            /* strncmp&cut the same strings of name matched */
			//后续匹配的项目只记录共性的字符串前缀
			// 假定ab匹配 abcd和abcef, 那么 strObj应该为abc， *maxLen为5			
            StrncmpCut(readDir->d_name, strObj, strlen(strObj));
            if (strlen(readDir->d_name) > *maxLen) {
				//记录所有匹配项的最大长度
                *maxLen = strlen(readDir->d_name);
            }
        }
        count++;  //匹配的项目数
    }

    if (closedir(openDir) < 0) {
        return (int)SH_ERROR;
    }

    return count;  //返回匹配的项数
}

static int OsTabMatchFile(char *cmdKey, unsigned int *len)
{
    unsigned int maxLen = 0;
    int count;
    char *strOutput = NULL;
    char *strCmp = NULL;
    char *dirOpen = (char *)malloc(CMD_MAX_PATH * 3); /* 3:dirOpen\strOutput\strCmp */
    if (dirOpen == NULL) {
        return (int)SH_ERROR;
    }

    (void)memset_s(dirOpen, CMD_MAX_PATH * 3, 0, CMD_MAX_PATH * 3); /* 3:dirOpen\strOutput\strCmp */
    strOutput = dirOpen + CMD_MAX_PATH;
    strCmp = strOutput + CMD_MAX_PATH;

    if (OsStrSeparate(cmdKey, dirOpen, strCmp, *len)) {
        free(dirOpen);
        return (int)SH_ERROR;
    }

    count = OsExecNameMatch(dirOpen, strCmp, strOutput, &maxLen);
    /* one or more matched */
    if (count >= 1) {
        OsCompleteStr(strOutput, strCmp, cmdKey, len);

        if (count == 1) {
            free(dirOpen);
            return 1;
        }
        if (OsPrintMatchList((unsigned int)count, dirOpen, strCmp, maxLen) == -1) {
            free(dirOpen);
            return (int)SH_ERROR;
        }
    }

    free(dirOpen);
    return count;
}

/*
 * Description: Pass in the string and clear useless space ,which inlcude:
 *                1) The overmatch space which is not be marked by Quote's area
 *                   Squeeze the overmatch space into one space
 *                2) Clear all space before first vaild charatctor
 * Input:       cmdKey : Pass in the buff string, which is ready to be operated
 *              cmdOut : Pass out the buffer string ,which has already been operated
 *              size : cmdKey length
 */
unsigned int OsCmdKeyShift(const char *cmdKey, char *cmdOut, unsigned int size)
{
    char *output = NULL;
    char *outputBak = NULL;
    unsigned int len;
    int ret;
    bool quotes = FALSE;

    if ((cmdKey == NULL) || (cmdOut == NULL)) {
        return (unsigned int)SH_ERROR;
    }

    len = strlen(cmdKey);
    if ((*cmdKey == '\n') || (len >= size)) {
        return (unsigned int)SH_ERROR;
    }
    output = (char *)malloc(len + 1);
    if (output == NULL) {
        printf("malloc failure in %s[%d]", __FUNCTION__, __LINE__);
        return (unsigned int)SH_ERROR;
    }

    /* Backup the 'output' start address */
    outputBak = output;
    /* Scan each charactor in 'cmdKey',and squeeze the overmuch space and ignore invaild charactor */
    for (; *cmdKey != '\0'; cmdKey++) {
        /* Detected a Double Quotes, switch the matching status */
        if (*(cmdKey) == '\"') {
            SWITCH_QUOTES_STATUS(quotes);
        }
        /* Ignore the current charactor in following situation */
        /* 1) Quotes matching status is FALSE (which said that the space is not been marked by double quotes) */
        /* 2) Current charactor is a space */
        /* 3) Next charactor is a space too, or the string is been seeked to the end already(\0) */
        /* 4) Invaild charactor, such as single quotes */
        if ((*cmdKey == ' ') && ((*(cmdKey + 1) == ' ') || (*(cmdKey + 1) == '\0')) && QUOTES_STATUS_CLOSE(quotes)) {
            continue;
        }
        if (*cmdKey == '\'') {
            continue;
        }
        *output = *cmdKey;
        output++;
    }
    *output = '\0';
    /* Restore the 'output' start address */
    output = outputBak;
    len = strlen(output);
    /* Clear the space which is located at the first charactor in buffer */
    if (*output == ' ') {
        output++;
        len--;
    }
    /* Copy out the buffer which is been operated already */
    ret = strncpy_s(cmdOut, size, output, len);
    if (ret != SH_OK) {
        printf("%s,%d strncpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
        free(outputBak);
        return SH_ERROR;
    }
    cmdOut[len] = '\0';

    free(outputBak);
    return SH_OK;
}
int OsTabCompletion(char *cmdKey, unsigned int *len)
{
    int count;
    char *cmdMainStr = cmdKey;

    if ((cmdKey == NULL) || (len == NULL)) {
        return (int)SH_ERROR;
    }

    /* cut left space */
    while (*cmdMainStr == 0x20) {
        cmdMainStr++;
    }

    count = OsTabMatchFile(cmdKey, len);

    return count;
}

unsigned int OsShellKeyInit(ShellCB *shellCB)
{
    CmdKeyLink *cmdKeyLink = NULL;
    CmdKeyLink *cmdHistoryLink = NULL;

    if (shellCB == NULL) {
        return SH_ERROR;
    }

    cmdKeyLink = (CmdKeyLink *)malloc(sizeof(CmdKeyLink));
    if (cmdKeyLink == NULL) {
        printf("Shell CmdKeyLink memory alloc error!\n");
        return SH_ERROR;
    }
    cmdHistoryLink = (CmdKeyLink *)malloc(sizeof(CmdKeyLink));
    if (cmdHistoryLink == NULL) {
        free(cmdKeyLink);
        printf("Shell CmdHistoryLink memory alloc error!\n");
        return SH_ERROR;
    }

    cmdKeyLink->count = 0;
    SH_ListInit(&(cmdKeyLink->list));
    shellCB->cmdKeyLink = (void *)cmdKeyLink;

    cmdHistoryLink->count = 0;
    SH_ListInit(&(cmdHistoryLink->list));
    shellCB->cmdHistoryKeyLink = (void *)cmdHistoryLink;
    shellCB->cmdMaskKeyLink = (void *)cmdHistoryLink;
    return SH_OK;
}

void OsShellKeyDeInit(CmdKeyLink *cmdKeyLink)
{
    CmdKeyLink *cmdtmp = NULL;
    if (cmdKeyLink == NULL) {
        return;
    }

    while (!SH_ListEmpty(&(cmdKeyLink->list))) {
        cmdtmp = SH_LIST_ENTRY(cmdKeyLink->list.pstNext, CmdKeyLink, list);
        SH_ListDelete(&cmdtmp->list);
        free(cmdtmp);
    }

    cmdKeyLink->count = 0;
    free(cmdKeyLink);
}

void OsShellCmdPush(const char *string, CmdKeyLink *cmdKeyLink)
{
    CmdKeyLink *cmdNewNode = NULL;
    unsigned int len;

    if ((string == NULL) || (strlen(string) == 0)) {
        return;
    }

    len = strlen(string);
    cmdNewNode = (CmdKeyLink *)malloc(sizeof(CmdKeyLink) + len + 1);
    if (cmdNewNode == NULL) {
        return;
    }

    (void)memset_s(cmdNewNode, sizeof(CmdKeyLink) + len + 1, 0, sizeof(CmdKeyLink) + len + 1);
    if (strncpy_s(cmdNewNode->cmdString, len + 1, string, len)) {
        free(cmdNewNode);
        return;
    }

    SH_ListTailInsert(&(cmdKeyLink->list), &(cmdNewNode->list));

    return;
}

void OsShellHistoryShow(unsigned int value, ShellCB *shellCB)
{
    CmdKeyLink *cmdtmp = NULL;
    CmdKeyLink *cmdNode = shellCB->cmdHistoryKeyLink;
    CmdKeyLink *cmdMask = shellCB->cmdMaskKeyLink;
    int ret;

    (void)pthread_mutex_lock(&shellCB->historyMutex);
    if (value == CMD_KEY_DOWN) {
        if (cmdMask == cmdNode) {
            goto END;
        }

        cmdtmp = SH_LIST_ENTRY(cmdMask->list.pstNext, CmdKeyLink, list);
        if (cmdtmp != cmdNode) {
            cmdMask = cmdtmp;
        } else {
            goto END;
        }
    } else if (value == CMD_KEY_UP) {
        cmdtmp = SH_LIST_ENTRY(cmdMask->list.pstPrev, CmdKeyLink, list);
        if (cmdtmp != cmdNode) {
            cmdMask = cmdtmp;
        } else {
            goto END;
        }
    }

    while (shellCB->shellBufOffset--) {
        printf("\b \b");
    }
    printf("%s", cmdMask->cmdString);
    shellCB->shellBufOffset = strlen(cmdMask->cmdString);
    (void)memset_s(shellCB->shellBuf, SHOW_MAX_LEN, 0, SHOW_MAX_LEN);
    ret = memcpy_s(shellCB->shellBuf, SHOW_MAX_LEN, cmdMask->cmdString, shellCB->shellBufOffset);
    if (ret != SH_OK) {
        printf("%s, %d memcpy failed!\n", __FUNCTION__, __LINE__);
        goto END;
    }
    shellCB->cmdMaskKeyLink = (void *)cmdMask;

END:
    (void)pthread_mutex_unlock(&shellCB->historyMutex);
    return;
}

unsigned int OsCmdExec(CmdParsed *cmdParsed, char *cmdStr)
{
    /* TODO: complete the usrspace command */
    unsigned int ret = SH_OK;
    if (cmdParsed && cmdStr) {
        ret = SH_NOK;
    }

    return ret;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
