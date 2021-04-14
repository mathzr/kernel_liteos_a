# LiteOS-A�ں�<a name="ZH-CN_TOPIC_0000001096612501"></a>

-   [���](#section11660541593)
-   [Ŀ¼](#section161941989596)
-   [Լ��](#section119744591305)
-   [ʹ��˵��](#section741617511812)
    -   [׼��](#section1579912573329)
    -   [��ȡԴ��](#section11443189655)
    -   [���빹��](#section2081013992812)

-   [��ز�](#section1371113476307)

## ���<a name="section11660541593"></a>

OpenHarmony LiteOS-A�ں��ǻ���Huawei LiteOS�ں��ݽ���չ����һ���ںˣ�Huawei LiteOS������IoT���򹹽�������������������ϵͳ����IoT��ҵ���ٷ�չ�ĳ����У�OpenHarmony LiteOS-A�ں��ܹ������û�С������͹��ġ������ܵ������Լ�ͳһ���ŵ���̬ϵͳ�����������˷ḻ���ں˻��ơ�����ȫ���POSIX��׼�ӿ��Լ�ͳһ�������**HDF**��OpenHarmony Driver Foundation���ȣ�Ϊ�豸�����ṩ�˸�ͳһ�Ľ��뷽ʽ��ΪOpenHarmony��Ӧ�ÿ������ṩ�˸��ѺõĿ������顣ͼ1ΪOpenHarmony LiteOS-A�ں˼ܹ�ͼ��

**ͼ 1**  OpenHarmony LiteOS-A�ں˼ܹ�ͼ<a name="fig27311582210"></a>  
![](figures/OpenHarmony-LiteOS-A�ں˼ܹ�ͼ.png "OpenHarmony-LiteOS-A�ں˼ܹ�ͼ")

## Ŀ¼<a name="section161941989596"></a>

```
/kernel/liteos_a
������ apps                   # �û�̬��init��shellӦ�ó���
������ arch                   # ��ϵ�ܹ���Ŀ¼����arm��
��   ������ arm                # arm�ܹ�����
������ bsd                    # freebsd��ص������������ģ��������룬����USB��
������ compat                 # �ں˽ӿڼ�����Ŀ¼
��   ������ posix              # posix��ؽӿ�
������ drivers                # �ں�����
��   ������ char               # �ַ��豸
��       ������ mem            # ��������IO�豸����
��       ������ quickstart     # ϵͳ���������ӿ�Ŀ¼
��       ������ random         # ������豸����
��       ������ video          # framebuffer�������
������ fs                     # �ļ�ϵͳģ�飬��Ҫ��Դ��NuttX��Դ��Ŀ
��   ������ fat                # fat�ļ�ϵͳ
��   ������ jffs2              # jffs2�ļ�ϵͳ
��   ������ include            # ���Ⱪ¶ͷ�ļ����Ŀ¼
��   ������ nfs                # nfs�ļ�ϵͳ
��   ������ proc               # proc�ļ�ϵͳ
��   ������ ramfs              # ramfs�ļ�ϵͳ
��   ������ vfs                # vfs��
������ kernel                 # ���̡��ڴ桢IPC��ģ��
��   ������ base               # �����ںˣ��������ȡ��ڴ��ģ��
��   ������ common             # �ں�ͨ�����
��   ������ extended           # ��չ�ںˣ�������̬���ء�vdso��liteipc��ģ��
��   ������ include            # ���Ⱪ¶ͷ�ļ����Ŀ¼
��   ������ user               # ����init����
������ lib                    # �ں˵�lib��
������ net                    # ����ģ�飬��Ҫ��Դ��lwip��Դ��Ŀ
������ platform               # ֧�ֲ�ͬ��оƬƽ̨���룬��Hi3516DV300��
��   ������ hw                 # ʱ�����ж�����߼�����
��   ������ include            # ���Ⱪ¶ͷ�ļ����Ŀ¼
��   ������ uart               # ��������߼�����
������ platform               # ֧�ֲ�ͬ��оƬƽ̨���룬��Hi3516DV300��
������ security               # ��ȫ������صĴ��룬��������Ȩ�޹��������idӳ�����
������ syscall                # ϵͳ����
������ tools                  # �������߼�������úʹ���
```

## Լ��<a name="section119744591305"></a>

-   �������ԣ�C/C++��
-   ������Hi3518EV300��Hi3516DV300���壻
-   Hi3518EV300Ĭ��ʹ��jffs2�ļ�ϵͳ��Hi3516DV300Ĭ��ʹ��FAT�ļ�ϵͳ��

## ʹ��˵��<a name="section741617511812"></a>

OpenHarmony LiteOS-A�ں�֧��Hi3518EV300��[����](https://gitee.com/openharmony/docs/blob/master/quick-start/Hi3518%E5%BC%80%E5%8F%91%E6%9D%BF%E4%BB%8B%E7%BB%8D.md)����Hi3516DV300��[����](https://gitee.com/openharmony/docs/blob/master/quick-start/Hi3516%E5%BC%80%E5%8F%91%E6%9D%BF%E4%BB%8B%E7%BB%8D.md)�����壬�����߿ɻ������ֵ��忪�������Լ���Ӧ�ó���

### ׼��<a name="section1579912573329"></a>

��������Ҫ��Linux�ϴ���뻷����

-   Hi3518EV300���壺�ο�[�����](https://gitee.com/openharmony/docs/blob/master/quick-start/Hi3518%E6%90%AD%E5%BB%BA%E7%8E%AF%E5%A2%83.md)��
-   Hi3516DV300���壺�ο�[�����](https://gitee.com/openharmony/docs/blob/master/quick-start/Hi3516%E6%90%AD%E5%BB%BA%E7%8E%AF%E5%A2%83.md)��

### ��ȡԴ��<a name="section11443189655"></a>

��Linux�����������ز���ѹһ��Դ���룬��ȡԴ�루[��������](https://repo.huaweicloud.com/harmonyos/os/1.0/code-1.0.tar.gz)��������Դ���ȡ��ʽ���ο�[Դ���ȡ](https://gitee.com/openharmony/docs/blob/master/get-code/%E6%BA%90%E7%A0%81%E8%8E%B7%E5%8F%96.md)��

### ���빹��<a name="section2081013992812"></a>

�����߿�����һ��Ӧ�ó���ɲο���

-   [helloworld for Hi3518EV300](https://gitee.com/openharmony/docs/blob/master/quick-start/%E5%BC%80%E5%8F%91Hi3518%E7%AC%AC%E4%B8%80%E4%B8%AA%E7%A4%BA%E4%BE%8B%E7%A8%8B%E5%BA%8F.md)��

-   [helloworld for Hi3516DV300](https://gitee.com/openharmony/docs/blob/master/quick-start/%E5%BC%80%E5%8F%91Hi3516%E7%AC%AC%E4%B8%80%E4%B8%AA%E5%BA%94%E7%94%A8%E7%A8%8B%E5%BA%8F%E7%A4%BA%E4%BE%8B.md)��

## ��ز�<a name="section1371113476307"></a>

[drivers\_liteos](https://gitee.com/openharmony/drivers_liteos)

**[kernel\_liteos\_a](https://gitee.com/openharmony/kernel_liteos_a)**

