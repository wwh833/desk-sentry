# 桌面哨兵 DeskSentry

> Windows 桌面自动整理工具 —— 文件归档进文件夹,快捷方式分类平铺,值守防"图标回弹"

![icon](assets-icon.png)

v1.5 起默认**混合整理**:

- **文件**(文档 / 图片 / 视频 / 音乐 / 压缩 / 安装包 / 代码 …)→ 自动归档进对应的分类文件夹
- **软件快捷方式**(`.lnk` / `.url`)→ 留在桌面,按分类顺序**列优先平铺排列**

针对抖音、WeGame 这类会**反复往桌面重建快捷方式**的程序,提供值守模式:新东西冒出来 3 秒内自动归位(文件归档、快捷方式排列)。

## 功能特性

- **混合整理(默认)**:一步执行"文件进文件夹 + 软件分类平铺",系统入口永远在最前
- **13 个分类**:软件类 5 个(学习 / 游戏 / 影音 / 通讯 / 工具)+ 文件类 8 个(文档 / 图片 / 视频 / 音乐 / 压缩 / 安装包 / **代码** / 其他)
- **系统入口保留**:此电脑、回收站、Microsoft Edge 永不移动
- **图形界面 + 命令行**:双击 exe 打开 GUI;`--cli` 交互菜单;参数齐全可脚本化
- **值守模式 `--watch`**:后台轮询桌面,新文件自动归档、新快捷方式自动归位
- **安全第一**:默认只读预览;移动记录与图标坐标都写日志,`--undo` 一键整批回滚(文件回原位、图标回原坐标)
- **复核机制**:每次摆放后用 MSAA 回读逐项比对,零出入才写日志
- **完美中文支持**:全程宽字符 API + 宽字面量数据表,源码 UTF-8、编译器执行字符集 GBK/UTF-8 结果完全一致
- **重名自动处理**:目标已存在时自动加 ` (1)` 后缀,绝不覆盖

## 使用方法

### 图形界面(默认)

双击 `desk_sentry.exe` 即可,浏览器打开本地页面(数据不出本机):预览 / 立即整理 / 撤销 / 值守开关 / 分类明细 / 活动日志。

### 交互菜单

`desk_sentry --cli`:

```
====================================================
  桌面哨兵  DeskSentry  v1.5
====================================================
  [1] 预览(只看不动)
  [2] 执行整理(文件进文件夹,快捷方式平铺)
  [3] 撤销上一次整理
  [4] 查看分类规则
  [5] 自动值守(新图标冒出来就自动归类)
  [6] 更换目录
  [0] 退出
```

### 命令行

| 命令 | 说明 |
|---|---|
| `desk_sentry` / `desk_sentry --gui` | 打开图形界面 |
| `desk_sentry --cli` | 打开命令行交互菜单 |
| `desk_sentry` (无动作参数时预览可用 `--cli` 选 1) | 默认只读预览 |
| `desk_sentry --apply` | **混合整理**:文件归档进文件夹 + 快捷方式分类平铺(记录可撤销) |
| `desk_sentry --apply --folders` | 旧版模式:桌面所有东西(含快捷方式)全部归档进文件夹 |
| `desk_sentry --undo` | 撤销上一次整理(文件回原位、图标回原坐标) |
| `desk_sentry --rules` | 查看分类规则表 |
| `desk_sentry --watch 3` | 值守模式,每 3 秒扫描一次,Ctrl+C 结束 |
| `desk_sentry --dir "D:\下载"` | 整理指定目录(非桌面目录自动用归档模式) |

参数可组合:`desk_sentry --watch 5 --dir "D:\下载"`

## 分类规则

**快捷方式**(`.lnk` / `.url`)按文件名关键词匹配,顺序即优先级,先命中先生效:

| 分类 | 部分关键词 |
|---|---|
| 游戏类 | steam、wegame、blizzard、overwatch、valorant、守望先锋、英雄联盟、原神…… |
| 影音类 | bilibili、iqiyi、youku、douyin、potplayer、哔哩哔哩、抖音、腾讯视频…… |
| 通讯类 | wechat、钉钉、飞书、telegram、qq、微信、腾讯会议、微博…… |
| 工具类 | wallpaper、图吧工具箱、ccleaner、winrar、everything、snipaste、鲁大师…… |
| 学习类 | photoshop、pycharm、intellij、jdk、python、office、dev-c++、学习、教程…… |

**文件**只按扩展名归类(不吃关键词,避免"游戏名的视频"进错文件夹):`.doc/.pdf/.txt… → 文档类`、`.jpg/.png… → 图片类`、`.mp4/.mkv… → 视频类`、`.mp3/.flac… → 音乐类`、`.zip/.7z… → 压缩类`、`.exe/.msi… → 安装包类`、**`.java/.cpp/.py/.js… → 代码类`**。

兜底规则:未知扩展一律进「其他类」(分类零遗漏);Steam 协议的 `.url` 一律归游戏类。完整规则运行 `desk_sentry --rules` 查看。

## 编译

### 小熊猫C++(Red Panda Dev-C++)

直接打开 `desk_sentry.cpp`,F9 编译运行。**F9 单文件编译(不含 `.rc`、不链额外库)即可通过**:所有外部 DLL 函数(oleacc / oleaut32 / ws2_32)都用 `LoadLibrary` + `GetProcAddress` 动态获取,不加任何 `-l` 库;产物没有自定义图标(功能不受影响)。

### 命令行(带图标)

```bat
windres -O coff -o app.o desk_sentry.rc
g++ -std=c++11 -O2 -Wall -static-libgcc -static-libstdc++ -o desk_sentry.exe desk_sentry.cpp app.o -lshell32
```

`-static-libgcc -static-libstdc++` 必须带上,否则双击运行会提示缺少 `libstdc++-6.dll`。

## 文件说明

| 文件 | 说明 |
|---|---|
| `desk_sentry.cpp` | 主程序源码(C++11,无第三方依赖) |
| `desk_sentry_gui.html` | 图形界面页面(本地 HTTP 服务加载) |
| `desk_sentry.rc` | 资源脚本,把图标嵌入 exe |
| `desk_sentry.ico` | 程序图标(16~256px 共 7 个尺寸) |
| `assets-icon.png` | 图标预览图 |

## 实现要点

1. **中文零乱码**:所有涉及中文的数据表使用宽字面量 `L"学习类"`,直接编译成 UTF-16 码点,不受 `-fexec-charset` 影响;命令行用 `CommandLineToArgvW` 解析,中文路径安全
2. **读 = MSAA(无障碍接口)**:跨进程 `SendMessage` 读结构体消息(`LVM_GETITEMTEXT/GETITEMPOSITION/GETITEMRECT`)全部失败,改用 oleacc 的 `AccessibleObjectFromWindow` 读图标名与屏幕坐标(动态加载,不链 `-loleacc`)
3. **写 = `LVM_SETITEMPOSITION`**:纯值参数跨进程有效,可把图标精确摆到目标格、精确还原
4. **网格反推**:原点取现有图标最小 x/y,间距取同行/同列最小正间距(观测值必是真实网格整数倍,目标必落系统吸附线);推不出时兜底 `SPI_GETICONMETRICS`
5. **摆放后复核**:等辅助功能层跟上后 MSAA 回读逐项比对,零出入才写日志,如实上报出入数
6. **稳定轮询**:归档后 shell 同步图标列表有延迟(实测 1 秒多还有幽灵图标),轮询等"该消失的消失、该出现的出现"再读列表排列
7. **日志与回滚**:移动记录 `源\t目标` + 位置记录 `@POS\t名字\tx\ty` 双格式同日志;`--undo` 先还原文件、等图标回列表、再恢复坐标,最后清理搬空的分类文件夹
8. **值守实现**:轮询 + 分片睡眠(200ms),`SetConsoleCtrlHandler` 捕获 Ctrl+C 优雅退出,不杀进程

## 版本历史

| 版本 | 说明 |
|---|---|
| v1.5 | 混合整理:文件按扩展名归档(新增「代码类」)、快捷方式分类平铺;归档/还原后稳定轮询 |
| v1.4 | 平铺排列模式(MSAA 读 + `LVM_SETITEMPOSITION` 写 + 网格反推 + 摆放复核) |
| v1.3 | 分类零遗漏(未知类型兜底「其他类」) |
| v1.1 | 首次发布(五大分类归档 + 值守模式) |
