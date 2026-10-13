# 桌面哨兵 DeskSentry

> Windows 桌面图标自动分类整理工具 —— 一键归类,值守模式防"图标回弹"

![icon](assets-icon.png)

把桌面上散落的快捷方式按类别自动归入文件夹,保持桌面整洁。针对抖音、WeGame 这类会**反复往桌面重建快捷方式**的程序,提供值守模式:新图标冒出来 3 秒内自动收走。

## 功能特性

- **五大分类**:学习类 / 游戏类 / 影音类 / 通讯类 / 工具类,规则表带优先级,先命中先生效
- **系统入口保留**:此电脑、回收站、Microsoft Edge 永不移动
- **交互菜单**:双击即可使用,预览 / 整理 / 撤销 / 规则 / 值守 一目了然
- **值守模式 `--watch`**:后台轮询桌面,自动归类新出现的图标
- **安全第一**:默认只读预览,确认后才动手;每次移动写日志,`--undo` 一键整批回滚
- **完美中文支持**:全程宽字符 API + 宽字面量数据表,源码 UTF-8、编译器执行字符集 GBK/UTF-8 结果完全一致
- **重名自动处理**:目标已存在时自动加 ` (1)` 后缀,绝不覆盖

## 使用方法

### 交互菜单(推荐)

双击 `desk_sentry.exe` 或桌面快捷方式:

```
====================================================
  桌面哨兵  DeskSentry  v1.1
====================================================
  [1] 预览(只看不动)
  [2] 执行整理
  [3] 撤销上一次整理
  [4] 查看分类规则
  [5] 自动值守(新图标冒出来就自动归类)
  [6] 更换目录
  [0] 退出
```

### 命令行

| 命令 | 说明 |
|---|---|
| `desk_sentry` | 打开交互菜单 |
| `desk_sentry --apply` | 执行整理,写入日志 |
| `desk_sentry --undo` | 撤销上一次整理(整批回滚) |
| `desk_sentry --rules` | 查看分类规则表 |
| `desk_sentry --watch 3` | 值守模式,每 3 秒扫描一次,Ctrl+C 结束 |
| `desk_sentry --dir "D:\下载"` | 整理指定目录(默认桌面) |

参数可组合:`desk_sentry --watch 5 --dir "D:\下载"`

## 分类规则

| 分类 | 部分关键词 |
|---|---|
| 游戏类 | steam、wegame、blizzard、overwatch、valorant、守望先锋、英雄联盟、原神…… |
| 影音类 | bilibili、iqiyi、youku、douyin、potplayer、哔哩哔哩、抖音、腾讯视频…… |
| 工具类 | wallpaper、图吧工具箱、ccleaner、winrar、everything、snipaste、鲁大师…… |
| 通讯类 | wechat、钉钉、飞书、telegram、qq、微信、腾讯会议、微博…… |
| 学习类 | photoshop、pycharm、intellij、jdk、python、office、dev-c++、学习、教程…… |

完整规则运行 `desk_sentry --rules` 查看;兜底规则:Steam 协议的 `.url` 一律归游戏类。

## 编译

### 小熊猫C++(Red Panda Dev-C++)

直接打开 `desk_sentry.cpp`,F9 编译运行。**注意**:IDE 单文件编译不包含 `desk_sentry.rc`,产物没有自定义图标(功能不受影响)。

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
| `desk_sentry.rc` | 资源脚本,把图标嵌入 exe |
| `desk_sentry.ico` | 程序图标(16~256px 共 7 个尺寸) |
| `assets-icon.png` | 图标预览图 |

## 实现要点

1. **中文零乱码**:所有涉及中文的数据表使用宽字面量 `L"学习类"`,直接编译成 UTF-16 码点,不受 `-fexec-charset` 影响;命令行用 `CommandLineToArgvW` 解析,中文路径安全
2. **跨编码一致**:已实测默认编译与 `-fexec-charset=GBK` 编译,分类输出逐行完全一致
3. **日志与回滚**:每次移动记录 `<源路径>\t<目标路径>`,`--undo` 逆序还原并清理搬空的分类文件夹
4. **值守实现**:轮询 + 分片睡眠(200ms),`SetConsoleCtrlHandler` 捕获 Ctrl+C 优雅退出,不杀进程
