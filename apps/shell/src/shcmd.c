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


//�ͷŽ�����ĸ������в���
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


//��ȡtab����ǰ���һ������
/* ���������
 * *tabStr tab����ʱ���������ַ���
 * parsed ��������ַ�������(�����б�)
 * tabStrLen tab����ʱ��ǰ�����г���
 * ���������*tabStr tab����ʱ���һ�����飬tab����Ŀ���ǻ�������������Զ���ȫ
 * ����ֵ�� �Ƿ�ɹ�
 * ע�⣺���������û�����TAB��ʱ���ã����ڸ��������Զ���ȫ
 */
static int OsStrSeparateTabStrGet(const char **tabStr, CmdParsed *parsed, unsigned int tabStrLen)
{
    char *shiftStr = NULL;
	//����2�������еĳ���
    char *tempStr = (char *)malloc(SHOW_MAX_LEN << 1);
    if (tempStr == NULL) {
        return (int)SH_ERROR;
    }

    (void)memset_s(tempStr, SHOW_MAX_LEN << 1, 0, SHOW_MAX_LEN << 1);
	//tempStr��shiftStr��ռһ�������г��ȵĿռ�
    shiftStr = tempStr + SHOW_MAX_LEN;

	//tempStr���ԭʼ����
    if (strncpy_s(tempStr, SHOW_MAX_LEN - 1, *tabStr, tabStrLen)) {
        free(tempStr);
        return (int)SH_ERROR;
    }

    parsed->cmdType = CMD_TYPE_STD;  //��׼����

    /* cut useless or repeat space */
	//ɾ��������ظ��ո����������shiftStr
    if (OsCmdKeyShift(tempStr, shiftStr, SHOW_MAX_LEN - 1)) {
        free(tempStr);
        return (int)SH_ERROR;
    }

    /* get exact position of string to complete */
    /* situation different if end space lost or still exist */
    if ((strlen(shiftStr) == 0) || (tempStr[strlen(tempStr) - 1] != shiftStr[strlen(shiftStr) - 1])) {
		//�������л����Կո��β��������
        *tabStr = "";  //��ô�ڿ��ַ������������Զ���ȫ
    } else {
        //��������������һ������Ļ���������ȫ
    	//�Ȱ��ո��ֳɲ����б�
        if (OsCmdParse(shiftStr, parsed)) {            free(tempStr);
            return (int)SH_ERROR;
        }
		//���һ����������������Ҫ�Ĵ��飬��������������Զ���ȫ
        *tabStr = parsed->paramArray[parsed->paramCnt - 1];
    }

    free(tempStr);
    return SH_OK;
}


//��ȡshell�����м�¼�ĵ�ǰ����Ŀ¼
char *OsShellGetWorkingDirtectory()
{
    return OsGetShellCb()->shellWorkingDirectory;
}

//����shell�����м�¼�ĵ�ǰ����Ŀ¼
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
 * ���������в�ֳ�Ŀ¼������Ҫƥ����ַ���
 * �������:
 * tabStr��tab����֮ǰ���������ַ���
 * tabStrLen: tab����֮ǰ���������ַ�������
 * ���������
 * strPath: ��Ҫ������Ŀ¼
 * nameLooking: ��Ŀ¼����Ҫ����������
 * ����ֵ���ɹ�orʧ��
 */
static int OsStrSeparate(const char *tabStr, char *strPath, char *nameLooking, unsigned int tabStrLen)
{
    char *strEnd = NULL;
    char *cutPos = NULL;
    CmdParsed parsed = {0};
    char *shellWorkingDirectory = OsShellGetWorkingDirtectory(); //��ǰ����Ŀ¼
    int ret;

	//ȡ��tab����ǰ���һ������
    ret = OsStrSeparateTabStrGet(&tabStr, &parsed, tabStrLen);
    if (ret != SH_OK) {
        return ret;
    }

    /* get fullpath str */
    if (*tabStr != '/') { //������鲻����'/'���ſ�ʼ��˵�����Ǿ���·�����������·��
		//��ô����·����Ҫ��ǰ�油���ϵ�ǰ����Ŀ¼
        if (strncpy_s(strPath, CMD_MAX_PATH, shellWorkingDirectory, CMD_MAX_PATH - 1)) {
            OsFreeCmdPara(&parsed);
            return (int)SH_ERROR;
        }
		//�����ǰ����Ŀ¼���Ǹ�Ŀ¼����ô����Ҫ��·����ĩβ��һ��'/'���ţ��統ǰ����Ŀ¼Ϊ/usr/bin
		//��ô�����strPath��Ϊ/usr/bin/
        if (strcmp(shellWorkingDirectory, "/")) {
            if (strncat_s(strPath, CMD_MAX_PATH - 1, "/", CMD_MAX_PATH - strlen(strPath) - 1)) {
                OsFreeCmdPara(&parsed);
                return (int)SH_ERROR;
            }
        }
    }
	
	//Ȼ����������û������·��
    if (strncat_s(strPath, CMD_MAX_PATH - 1, tabStr, CMD_MAX_PATH - strlen(strPath) - 1)) {
        OsFreeCmdPara(&parsed);
        return (int)SH_ERROR;
    }

    /* split str by last '/' */
    strEnd = strrchr(strPath, '/');  //�����������·�������һ��'/'����
    if (strEnd != NULL) {
		//��������֮��Ĳ��֣��������Ǵ�ƥ����ļ�����Ŀ¼
        if (strncpy_s(nameLooking, CMD_MAX_PATH, strEnd + 1, CMD_MAX_PATH - 1)) { /* get cmp str */
            OsFreeCmdPara(&parsed);
            return (int)SH_ERROR;
        }
    }

	//�����һ��/�ַ����������ַ�����strPath��/��β������"/usr/bin/xxx"��Ϊ"/usr/bin/\0"
    cutPos = strrchr(strPath, '/');
    if (cutPos != NULL) {
        *(cutPos + 1) = '\0';
    }

    OsFreeCmdPara(&parsed);
    return SH_OK;
}


//��ҳ����
static int OsShowPageInputControl(void)
{
    char readChar;

    while (1) {
        if (read(STDIN_FILENO, &readChar, 1) != 1) { /* get one char from stdin */
            printf("\n");
            return (int)SH_ERROR;
        }
		//����û�����q����Q����CTRL+C, ���к��˳���ʾ
        if ((readChar == 'q') || (readChar == 'Q') || (readChar == CTRL_C)) {
            printf("\n");
            return 0;
        } else if (readChar == '\r') {  //�û������˻س����Ժ���� --more-- ��8���ַ�ɾ������������ʾ��һҳ
            printf("\b \b\b \b\b \b\b \b\b \b\b \b\b \b\b \b");
            return 1;
        }
    }
}

//��ҳ��ʾ����
static int OsShowPageControl(unsigned int timesPrint, unsigned int lineCap, unsigned int count)
{
    if (NEED_NEW_LINE(timesPrint, lineCap)) {
		//��Ҫ���е�����£���һ��
        printf("\n");
        if (SCREEN_IS_FULL(timesPrint, lineCap) && (timesPrint < count)) {
            printf("--More--");  //��Ҫ��ҳ������£������ʾ
            return OsShowPageInputControl(); //Ȼ��ȴ��û�������
        }
    }
    return 1;  //����������������ʾ
}


//��Ԥ�⵽��Ҫ��ʾ�����ݻᳬ��1ҳʱ��
//��ʾ�û��Ƿ���ʾ���е�ƥ��
static int OsSurePrintAll(unsigned int count)
{
    char readChar = 0;
    printf("\nDisplay all %u possibilities?(y/n)", count);  //��ʾ��ʾ��Ϣ
    while (1) {
        if (read(STDIN_FILENO, &readChar, 1) != 1) {
            return (int)SH_ERROR;
        }
		//�û�����n����N����CTRL+C������ʾ
        if ((readChar == 'n') || (readChar == 'N') || (readChar == CTRL_C)) {
            printf("\n");
            return 0;
        } else if ((readChar == 'y') || (readChar == 'Y') || (readChar == '\r')) {
            return 1; //�û�����y����Y���߻س�����������ʾ
        }
    }
}


//��ʾ����ƥ�����Ŀ
/*
 * count : �ܹ���Ҫ��ʾ����Ŀ��
 * strPath : Ŀ¼·��
 * nameLooking: ��Ҫƥ����ַ���
 * printLen : ÿ����Ŀ��ʾ�Ŀ��
 */
static int OsPrintMatchList(unsigned int count, const char *strPath, const char *nameLooking, unsigned int printLen)
{
    unsigned int timesPrint = 0;
    unsigned int lineCap;
    int ret;
    DIR *openDir = NULL;
    struct dirent *readDir = NULL;
	//��ʽ�ַ����Ŀ�ȣ���"%-xxxs          " �ĳ��ȣ�
    char formatChar[10] = {0}; /* 10:for formatChar length */

	//����2���ַ���ԭ���ǣ�ÿ����ʾ��Ŀĩβ������ʾ2���ո�
	//������������ı��ͬ����ʾ��Ŀ
	//ÿ����ʾ��Ŀ���ܳ���1��
    printLen = (printLen > (DEFAULT_SCREEN_WIDTH - 2)) ? (DEFAULT_SCREEN_WIDTH - 2) : printLen; /* 2:revered 2 bytes */
	//ÿ����ʾ��ĿԤ��2�����ַ���ÿ�������ʾlineCap����Ŀ
    lineCap = DEFAULT_SCREEN_WIDTH / (printLen + 2); /* 2:DEFAULT_SCREEN_WIDTH revered 2 bytes */
	//ÿ����ʾ��Ŀ�ĸ�ʽ�ַ�������formatChar��
    if (snprintf_s(formatChar, sizeof(formatChar) - 1, 7, "%%-%us  ", printLen) < 0) { /* 7:format-len */
        return (int)SH_ERROR;
    }

    if (count > (lineCap * DEFAULT_SCREEN_HEIGNT)) {
		//���е���ʾ��Ŀ�ᳬ��1�����������Ҫ���û������Ƿ������ʾ
        ret = OsSurePrintAll(count);
        if (ret != 1) {
            return ret;
        }
    }
    openDir = opendir(strPath);  //�ȴ���Ŀ��Ҫƥ���Ŀ¼
    if (openDir == NULL) {
        return (int)SH_ERROR;
    }

    printf("\n");
	//����Ŀ¼�е�ÿһ����Ŀ
    for (readDir = readdir(openDir); readDir != NULL; readDir = readdir(openDir)) {		
        if (strncmp(nameLooking, readDir->d_name, strlen(nameLooking)) != 0) {
            continue; //������ƥ�����Ŀ
        }
		//���ƥ�����ǵ����ƣ���ô������ʾ����Ȼ��ʱd_name�ĳ��Ȳ����nameLooking��
        printf(formatChar, readDir->d_name);
        timesPrint++; //ͳ������ʾ����Ŀ��
		//���л�����ʾ�ͻ�ҳ����
        ret = OsShowPageControl(timesPrint, lineCap, count);
        if (ret != 1) {
			//�û���ҳ����ʱ�˳��ˣ���ر�Ŀ¼���˳�
            if (closedir(openDir) < 0) {
                return (int)SH_ERROR;
            }
            return ret;
        }
    }

    printf("\n");
	//��ʾ��ɣ�Ҳ�ر�Ŀ¼�˳�
    if (closedir(openDir) < 0) {
        return (int)SH_ERROR;
    }

    return SH_OK;
}

//��2���ַ����Ƚ�ǰn���ַ�
//��Ե�2���ַ�����ֻ����2���ַ�����ͬ�Ĳ���
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
 * �Զ���ȫ������
 * result : ��ȫ����ַ���
 * target : ��ȫǰ���ַ���
 * cmdKey : �����л���
 * *len : ��ȫǰ�������л��泤��
 * ���ֵ : *len ��ȫ��������л��泤��
 *          cmdKey �����л���
 */
static void OsCompleteStr(char *result, const char *target, char *cmdKey, unsigned int *len)
{
    unsigned int size = strlen(result) - strlen(target);  //��Ҫ�Զ���ȫ���ַ���
    char *des = cmdKey + *len;  //��ǰ��Ҫ��ȫ�ַ��Ļ���λ��
    char *src = result + strlen(target); //����ַ��������Զ���ȫλ��

    while (size-- > 0) {
        printf("%c", *src);  //����ȫ���ַ���ʾ����Ļ��
        if (*len == (SHOW_MAX_LEN - 1)) {
            *des = '\0';
            break;
        }
        *des++ = *src++;  //��Ҫ��ȫ���ַ����������
        (*len)++; //�����������һ���ַ�
    }
}


//��strPathĿ¼�²����ַ���ΪnameLooking��ʼ����Ŀ
//ƥ��Ĺ����ַ��Ӵ�����strObj, ƥ������Ŀ�ĳ��ȴ���*maxLen
static int OsExecNameMatch(const char *strPath, const char *nameLooking, char *strObj, unsigned int *maxLen)
{
    int count = 0;
    DIR *openDir = NULL;
    struct dirent *readDir = NULL;

    openDir = opendir(strPath);  //�ȴ�Ŀ¼
    if (openDir == NULL) {
        return (int)SH_ERROR;
    }

	//��������Ŀ¼
    for (readDir = readdir(openDir); readDir != NULL; readDir = readdir(openDir)) {
        if (strncmp(nameLooking, readDir->d_name, strlen(nameLooking)) != 0) {
            continue; //������ƥ�����Ŀ
        }
		//������ƥ�����Ŀ�Ĵ���
        if (count == 0) {
			//ƥ��ĵ�һ��ȼ�¼��strObj��
            if (strncpy_s(strObj, CMD_MAX_PATH, readDir->d_name, CMD_MAX_PATH - 1)) {
                (void)closedir(openDir);
                return (int)SH_ERROR;
            }
			//���ĳ���Ҳ��¼����
            *maxLen = strlen(readDir->d_name);
        } else {
            /* strncmp&cut the same strings of name matched */
			//����ƥ�����Ŀ��strObj�󽻼�
			// �ٶ���ab��ƥ�� abcd��abcef, ��ô strObjӦ��Ϊabc�� *maxLenΪ5			
            StrncmpCut(readDir->d_name, strObj, strlen(strObj));
            if (strlen(readDir->d_name) > *maxLen) {
				//��¼����ƥ�������󳤶�
                *maxLen = strlen(readDir->d_name);
            }
        }
        count++;  //ƥ�����Ŀ��
    }

    if (closedir(openDir) < 0) { //���ر�Ŀ¼
        return (int)SH_ERROR;
    }

    return count;  //������ƥ�������
}


//����TAB���Ժ�Ե�ǰ����ĩβ���ļ���(Ŀ¼��)����ƥ����Զ���ȫ
static int OsTabMatchFile(char *cmdKey, unsigned int *len)
{
    unsigned int maxLen = 0;
    int count;
    char *strOutput = NULL;
    char *strCmp = NULL;
	//ֱ������3���ַ����ռ�
	//�ֱ�����Ҫ�򿪵�Ŀ¼���ƣ�ƥ���Ľ���ַ�����ƥ��ǰ�Ĵ��Ƚ��ַ���
    char *dirOpen = (char *)malloc(CMD_MAX_PATH * 3); /* 3:dirOpen\strOutput\strCmp */
    if (dirOpen == NULL) {
        return (int)SH_ERROR;
    }

	//��3���ַ�������ʼ��ַ���ú�
    (void)memset_s(dirOpen, CMD_MAX_PATH * 3, 0, CMD_MAX_PATH * 3); /* 3:dirOpen\strOutput\strCmp */
    strOutput = dirOpen + CMD_MAX_PATH;
    strCmp = strOutput + CMD_MAX_PATH;

	//��������л�ȡ��ƥ���Ŀ¼�ʹ�ƥ����ַ���(�ļ�����Ŀ¼����һ����)
    if (OsStrSeparate(cmdKey, dirOpen, strCmp, *len)) {
        free(dirOpen);
        return (int)SH_ERROR;
    }

	//ִ������ƥ�䶯����ƥ���Ľ���ַ�������strOutput, �˽���������Զ���ȫ
	//ƥ����ļ�����Ŀ¼��ͨ��count����
    count = OsExecNameMatch(dirOpen, strCmp, strOutput, &maxLen);
    /* one or more matched */
    if (count >= 1) {
		//����ƥ�������������Զ���ȫ����
        OsCompleteStr(strOutput, strCmp, cmdKey, len);

        if (count == 1) {
            free(dirOpen);
            return 1;
        }
		//����ж���ƥ�䣬����Ҫ��ÿ��ƥ�������ʾ����
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
 //�������ж�����ظ��Ŀո�����󣬴���cmdOut��
 //�����п�ͷ�Ŀո�
 //������ĩβ�Ŀո�
 //�������м��ظ��ո�ֻ����1����˫�����ڵĿո����
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
	//�����洢�м���
    output = (char *)malloc(len + 1);
    if (output == NULL) {
        printf("malloc failure in %s[%d]\n", __FUNCTION__, __LINE__);
        return (unsigned int)SH_ERROR;
    }

    /* Backup the 'output' start address */
    outputBak = output;  //�ȱ����ַ�����ʼ��ַ
    /* Scan each charactor in 'cmdKey',and squeeze the overmuch space and ignore invaild charactor */
	//����������ո���ַ���
    for (; *cmdKey != '\0'; cmdKey++) {
        /* Detected a Double Quotes, switch the matching status */
        if (*(cmdKey) == '\"') {
			//һ������˫���ţ����л�˫����״̬��������ǰ��˫�����ڣ�������
            SWITCH_QUOTES_STATUS(quotes);
        }
        /* Ignore the current charactor in following situation */
        /* 1) Quotes matching status is FALSE (which said that the space is not been marked by double quotes) */
        /* 2) Current charactor is a space */
        /* 3) Next charactor is a space too, or the string is been seeked to the end already(\0) */
        /* 4) Invaild charactor, such as single quotes */
        if ((*cmdKey == ' ') && ((*(cmdKey + 1) == ' ') || (*(cmdKey + 1) == '\0')) && QUOTES_STATUS_CLOSE(quotes)) {
			//���������ո������£�ֻ�������һ���ո�
			//�ַ���ĩβ�Ŀո�Ҳ������
			//��˫�����еĿո���Ҫ����
            continue;
        }
        if (*cmdKey == '\'') {
            continue;  //�����Ų�����
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
		//����ǰ���㷨������ַ����ʼ�ɿո�
		//��ô������һ���ո�
		//����Ҳ��Ҫ����ȥ��
        output++;
        len--;
    }
    /* Copy out the buffer which is been operated already */
	//���յĽ������cmdOut�в�����ʼ�ͽ����ո����м��������ո�(˫�����г���)
    ret = strncpy_s(cmdOut, size, output, len);
    if (ret != SH_OK) {
        printf("%s,%d strncpy_s failed, err:%d!\n", __FUNCTION__, __LINE__, ret);
        free(outputBak);
        return SH_ERROR;
    }
	//�������ַ�����β��
    cmdOut[len] = '\0';

    free(outputBak);
    return SH_OK;
}

//�������Զ���ȫ����
int OsTabCompletion(char *cmdKey, unsigned int *len)
{
    int count;    

    if ((cmdKey == NULL) || (len == NULL)) {
        return (int)SH_ERROR;
    }
        count = OsTabMatchFile(cmdKey, len);

    return count;
}


//��ʼ��shell��ص����������������ʷ����
unsigned int OsShellKeyInit(ShellCB *shellCB)
{
    CmdKeyLink *cmdKeyLink = NULL;
    CmdKeyLink *cmdHistoryLink = NULL;

    if (shellCB == NULL) {
        return SH_ERROR;
    }

	//������������ͷ�ڵ��ڴ�
    cmdKeyLink = (CmdKeyLink *)malloc(sizeof(CmdKeyLink));
    if (cmdKeyLink == NULL) {
        printf("Shell CmdKeyLink memory alloc error!\n");
        return SH_ERROR;
    }
	//����������ʷ����ͷ�ڵ��ڴ�
    cmdHistoryLink = (CmdKeyLink *)malloc(sizeof(CmdKeyLink));
    if (cmdHistoryLink == NULL) {
        free(cmdKeyLink);
        printf("Shell CmdHistoryLink memory alloc error!\n");
        return SH_ERROR;
    }

	//��ʼ����������
    cmdKeyLink->count = 0;
    SH_ListInit(&(cmdKeyLink->list));
    shellCB->cmdKeyLink = (void *)cmdKeyLink;

	//��ʼ��������ʷ����
    cmdHistoryLink->count = 0;
    SH_ListInit(&(cmdHistoryLink->list));
    shellCB->cmdHistoryKeyLink = (void *)cmdHistoryLink;
	//��ʼ��������ʷ�α꣬����ǰѡ�����ĸ���ʷ�����ʼ״̬δѡ���κ�����
    shellCB->cmdMaskKeyLink = (void *)cmdHistoryLink;
    return SH_OK;
}


//��������������������ʷ����
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


//��������������������
void OsShellCmdPush(const char *string, CmdKeyLink *cmdKeyLink)
{
    CmdKeyLink *cmdNewNode = NULL;
    unsigned int len;

    if ((string == NULL) || (strlen(string) == 0)) {
        return;
    }

	//���������ַ�����������
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

	//����FIFO��ʽ�����������
    SH_ListTailInsert(&(cmdKeyLink->list), &(cmdNewNode->list));

    return;
}

//���û��������·����ʱ����ʾ��ʷ����
void OsShellHistoryShow(unsigned int value, ShellCB *shellCB)
{
    CmdKeyLink *cmdtmp = NULL;
    CmdKeyLink *cmdNode = shellCB->cmdHistoryKeyLink; //������ʷ����
    CmdKeyLink *cmdMask = shellCB->cmdMaskKeyLink; //��ǰ��ʷ�α�
    int ret;

    (void)pthread_mutex_lock(&shellCB->historyMutex);
    if (value == CMD_KEY_DOWN) {
        if (cmdMask == cmdNode) {
            goto END; //�ڰ��·����ʱ���Ѿ���������µ�����
        }

		//�����µķ��������һ������
        cmdtmp = SH_LIST_ENTRY(cmdMask->list.pstNext, CmdKeyLink, list);
        if (cmdtmp != cmdNode) {
            cmdMask = cmdtmp;  //���������α�
        } else {
            goto END; //�Ѿ��������µ�����
        }
    } else if (value == CMD_KEY_UP) {
    	//�����ϵķ��������һ������
        cmdtmp = SH_LIST_ENTRY(cmdMask->list.pstPrev, CmdKeyLink, list);
        if (cmdtmp != cmdNode) {
            cmdMask = cmdtmp; //���������α�
        } else {
            goto END;  //�Ѿ���������ϵ�����
        }
    }

	//���������е�����
    while (shellCB->shellBufOffset--) {
		//������Ļ��ɾ������ʾ������
        printf("\b \b");  //����һ����ʾ�ո��ٻ��ˡ��ﵽɾ��һ�����ŵ�Ч��
    }
    printf("%s", cmdMask->cmdString);  //����Ļ����ʾ�����α��Ӧ��������û����ܵ������·������Ч��
    //ˢ�������
    shellCB->shellBufOffset = strlen(cmdMask->cmdString);
    (void)memset_s(shellCB->shellBuf, SHOW_MAX_LEN, 0, SHOW_MAX_LEN);
    ret = memcpy_s(shellCB->shellBuf, SHOW_MAX_LEN, cmdMask->cmdString, shellCB->shellBufOffset);
    if (ret != SH_OK) {
        printf("%s, %d memcpy failed!\n", __FUNCTION__, __LINE__);
        goto END;
    }
    shellCB->cmdMaskKeyLink = (void *)cmdMask;  //��¼�����α�

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
