// ============================================================================
//  桌面哨兵 DeskSentry (desk_sentry.cpp)
//  —— 桌面图标自动分类排列工具(Windows / 小熊猫C++ 可直接编译)
//
//  用途:识别桌面上的一切 —— 文件归档进分类文件夹,快捷方式按分类顺序平铺排列,
//        值守模式下,新冒出来的文件几秒内自动归档、图标自动归位到所属分类。
//  分类:学习 / 游戏 / 影音 / 通讯 / 工具(软件,平铺在桌面)
//        文档 / 图片 / 视频 / 音乐 / 压缩 / 安装包 / 代码 / 其他(文件,归档进文件夹)—— 共 13 类
//
//  用法:
//      desk_sentry                  打开图形界面(双击 exe 即可使用)
//      desk_sentry --gui            打开图形界面
//      desk_sentry --cli            打开命令行交互菜单
//      desk_sentry --apply          混合整理:文件归档进文件夹,快捷方式分类平铺(记录可撤销)
//      desk_sentry --apply --folders 旧版模式:桌面所有东西(含快捷方式)全部归档进文件夹
//      desk_sentry --undo           撤销上一次整理(文件回原位、图标回原坐标)
//      desk_sentry --rules          查看分类规则表
//      desk_sentry --watch [秒]     值守模式:新图标出现即自动整理(默认 3 秒一查)
//      desk_sentry --dir "路径"     整理指定目录(非桌面目录自动用归档模式)
//
//  设计要点:
//    1. 全程宽字符(W 系列 API),中文路径、中文文件名不乱码;
//    2. 所有中文数据表用宽字面量 L"..." 存放,直接编译成 UTF-16 码点,
//       不受源文件编码和 -fexec-charset 影响(GBK/UTF-8 编译结果一致);
//    3. 规则表有优先级,先匹配先生效;"此电脑/回收站/Edge" 等系统入口强制保留;
//    4. 排列只改图标坐标不碰文件,旧坐标逐条写日志;归档移动同样写日志,都支持一键撤销;
//    5. 默认只读预览,加 --apply 或菜单选 2 才动手 —— 误操作零风险。
// ============================================================================

// ---------------------------------------------------------------------------
//  Winsock:动态加载 ws2_32.dll,不链接 -lws2_32 —— 这样小熊猫 F9 单文件编译
//  也能通过。头文件按官方要求放在 windows.h 之前。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <commctrl.h>      // 桌面图标列表(LVM_*/LVITEMW)—— 只用消息与结构体,不链 -lcomctl32
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <oleauto.h>     // BSTR 等(只用类型;SysFreeString 动态加载,不链 -loleaut32)
#include <oleacc.h>      // IAccessible(只用接口;AccessibleObjectFromWindow 动态加载,不链 -loleacc)

#include <string>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <vector>

// ---------------------------------------------------------------- 运行时编码自适应
// 把编译器产出的窄字符串字面量解码成宽字符串:
// 先严格按 UTF-8 解析;失败说明执行字符集是 GBK/ANSI,再按 ANSI 页解析。
static std::wstring decodeLiteral(const char* s, int n)
{
    std::wstring r;
    if (n <= 0) return r;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, n, NULL, 0);
    if (need > 0) {
        r.resize(need);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, n, &r[0], need);
        return r;
    }
    need = MultiByteToWideChar(CP_ACP, 0, s, n, NULL, 0);
    if (need > 0) {
        r.resize(need);
        MultiByteToWideChar(CP_ACP, 0, s, n, &r[0], need);
    }
    return r;
}

// 用户自定义字面量:写 L"中文"_w 即可得到 std::wstring,少写一层构造
static std::wstring operator""_w(const wchar_t* s, std::size_t n)
{
    return std::wstring(s, n);
}

// 这套 MinGW 的 libstdc++ 没有提供 std::to_wstring,自己补一个
static std::wstring toW(long v)
{
    std::wostringstream oss;
    oss << v;
    return oss.str();
}

// ---------------------------------------------------------------- 输出
static std::string toUtf8(const std::wstring& ws)
{
    std::string r;
    if (ws.empty()) return r;
    int need = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    if (need > 0) {
        r.resize(need);
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &r[0], need, NULL, NULL);
    }
    return r;
}

// GUI 模式的活动日志缓冲:所有输出在打到控制台的同时,也进这里,
// 网页端通过 /api/activity 增量拉取,实现"值守动态实时上屏"。
static CRITICAL_SECTION g_capCs;
static volatile LONG g_capCsState = 0;   // 0=未初始化 1=正在初始化 2=完成
static std::wstring g_capBuf;            // 保留 [g_capBase, g_capBase+size) 范围内的文本
static unsigned long g_capBase = 0;      // 已被裁掉的字符数
static const size_t CAP_MAX = 32768, CAP_TRIM = 24576;

static void ensureCapCs()
{
    if (InterlockedCompareExchange(&g_capCsState, 1, 0) == 0) {
        InitializeCriticalSection(&g_capCs);
        InterlockedExchange(&g_capCsState, 2);
    } else {
        while (g_capCsState != 2) Sleep(1);
    }
}

static void capAppend(const std::wstring& s)
{
    if (s.empty()) return;
    ensureCapCs();
    EnterCriticalSection(&g_capCs);
    g_capBuf += s;
    if (g_capBuf.size() > CAP_MAX) {
        size_t drop = g_capBuf.size() - CAP_TRIM;
        g_capBuf.erase(0, drop);
        g_capBase += (unsigned long)drop;
    }
    LeaveCriticalSection(&g_capCs);
}

static void out(const std::wstring& s)
{
    capAppend(s);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &written, NULL);
    } else {
        std::string u8 = toUtf8(s);
        if (!u8.empty()) WriteFile(h, u8.data(), (DWORD)u8.size(), &written, NULL);
    }
}

static void outLn(const std::wstring& s)
{
    out(s);
    out(L"\n");
}

// ---------------------------------------------------------------- 小工具
static std::wstring lowerAscii(const std::wstring& s)
{
    std::wstring r = s;
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] >= L'A' && r[i] <= L'Z') r[i] = (wchar_t)(r[i] - L'A' + L'a');
    }
    return r;
}

// 不区分 ASCII 大小写的子串匹配(中文按码点直接比较,天然正确)
static bool containsNoCase(const std::wstring& text, const std::wstring& key)
{
    if (key.empty() || key.size() > text.size()) return false;
    std::wstring t = lowerAscii(text);
    std::wstring k = lowerAscii(key);
    for (size_t i = 0; i + k.size() <= t.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < k.size(); ++j) {
            if (t[i + j] != k[j]) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

static std::wstring extensionOf(const std::wstring& name)
{
    size_t dot = name.find_last_of(L'.');
    size_t slash = name.find_last_of(L"\\/");
    if (dot == std::wstring::npos) return L"";
    if (slash != std::wstring::npos && dot < slash) return L"";
    return lowerAscii(name.substr(dot));
}

static std::wstring joinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

// ---------------------------------------------------------------- 路径
static std::wstring desktopDir()
{
    wchar_t buf[MAX_PATH];
    buf[0] = 0;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, buf))) {
        if (buf[0]) return std::wstring(buf);
    }
    wchar_t up[MAX_PATH];
    up[0] = 0;
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(up) + L"\\Desktop";
    return L".";
}

static std::wstring exeDir()
{
    wchar_t buf[MAX_PATH];
    buf[0] = 0;
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return p.substr(0, pos);
}

static bool fileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dirExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 目录里除了 . 和 .. 之外还有没有别的东西
static bool isDirEmpty(const std::wstring& path)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(path, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool empty = true;
    do {
        std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") { empty = false; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

// ---------------------------------------------------------------- 分类规则
struct Rule {
    std::wstring cat;
    std::wstring key;
};

// 报告时用的分类顺序(文件夹创建/展示/撤销清理都按此顺序)
static const wchar_t* const kCategoryOrder[] = {
    // —— 软件/主题类:按文件名关键词匹配(快捷方式用) ——
    L"学习类", L"游戏类", L"影音类", L"通讯类", L"工具类",
    // —— 文件类:按扩展名匹配(文件归档用) ——
    L"文档类", L"图片类", L"视频类", L"音乐类", L"压缩类", L"安装包类", L"代码类", L"其他类"
};

// { 分类, 关键词 } —— 顺序即优先级:先匹配到的规则生效
static const wchar_t* const kRules[][2] = {
    // ---- 游戏类 ----
    { L"游戏类", L"steam" },
    { L"游戏类", L"wegame" },
    { L"游戏类", L"counter-strike" },
    { L"游戏类", L"cs2" },
    { L"游戏类", L"backrooms" },
    { L"游戏类", L"forest" },
    { L"游戏类", L"lovechoice" },
    { L"游戏类", L"watt" },
    { L"游戏类", L"blizzard" },
    { L"游戏类", L"battle.net" },
    { L"游戏类", L"overwatch" },
    { L"游戏类", L"valorant" },
    { L"游戏类", L"crossfire" },
    { L"游戏类", L"minecraft" },
    { L"游戏类", L"genshin" },
    { L"游戏类", L"game" },
    { L"游戏类", L"游戏" },
    { L"游戏类", L"守望先锋" },
    { L"游戏类", L"穿越火线" },
    { L"游戏类", L"无畏契约" },
    { L"游戏类", L"暴雪战网" },
    { L"游戏类", L"英雄联盟" },
    { L"游戏类", L"原神" },

    // ---- 影音类 ----
    { L"影音类", L"bilibili" },
    { L"影音类", L"iqiyi" },
    { L"影音类", L"youku" },
    { L"影音类", L"douyin" },
    { L"影音类", L"tiktok" },
    { L"影音类", L"tencent video" },
    { L"影音类", L"potplayer" },
    { L"影音类", L"xunlei" },
    { L"影音类", L"thunder" },
    { L"影音类", L"哔哩哔哩" },
    { L"影音类", L"爱奇艺" },
    { L"影音类", L"腾讯视频" },
    { L"影音类", L"优酷" },
    { L"影音类", L"抖音" },
    { L"影音类", L"迅雷" },
    { L"影音类", L"音乐" },
    { L"影音类", L"视频" },

    // ---- 工具类 ----
    { L"工具类", L"wallpaper" },
    { L"工具类", L"壁纸" },
    { L"工具类", L"atk" },
    { L"工具类", L"图吧" },
    { L"工具类", L"toolbox" },
    { L"工具类", L"ccleaner" },
    { L"工具类", L"winrar" },
    { L"工具类", L"7-zip" },
    { L"工具类", L"7zip" },
    { L"工具类", L"everything" },
    { L"工具类", L"snipaste" },
    { L"工具类", L"notepad" },
    { L"工具类", L"nvidia" },
    { L"工具类", L"geforce" },
    { L"工具类", L"鲁大师" },
    { L"工具类", L"工具" },

    // ---- 通讯类 ----
    { L"通讯类", L"wechat" },
    { L"通讯类", L"weixin" },
    { L"通讯类", L"tencent meeting" },
    { L"通讯类", L"tencentmeeting" },
    { L"通讯类", L"dingtalk" },
    { L"通讯类", L"telegram" },
    { L"通讯类", L"whatsapp" },
    { L"通讯类", L"outlook" },
    { L"通讯类", L"qq" },
    { L"通讯类", L"微信" },
    { L"通讯类", L"腾讯会议" },
    { L"通讯类", L"钉钉" },
    { L"通讯类", L"飞书" },
    { L"通讯类", L"微博" },
    { L"通讯类", L"邮箱" },

    // ---- 学习类(放最后,避免误伤上面的规则) ----
    { L"学习类", L"photoshop" },
    { L"学习类", L"pycharm" },
    { L"学习类", L"intellij" },
    { L"学习类", L"idea" },
    { L"学习类", L"dev-c++" },
    { L"学习类", L"red panda" },
    { L"学习类", L"jdk" },
    { L"学习类", L"jre" },
    { L"学习类", L"java" },
    { L"学习类", L"eclipse" },
    { L"学习类", L"matlab" },
    { L"学习类", L"office" },
    { L"学习类", L"python" },
    { L"学习类", L"anaconda" },
    { L"学习类", L"typora" },
    { L"学习类", L"postman" },
    { L"学习类", L"code" },
    { L"学习类", L"学习" },
    { L"学习类", L"教程" }
};

// 永远留在桌面的系统入口
static const wchar_t* const kKeepOnDesktop[] = {
    L"此电脑", L"回收站", L"microsoft edge"
};

// { 扩展名, 文件分类 } —— 名字没命中关键词时,按扩展名归类(顺序即优先级)
static const wchar_t* const kExtRules[][2] = {
    // ---- 文档类 ----
    { L".doc", L"文档类" }, { L".docx", L"文档类" }, { L".xls", L"文档类" },
    { L".xlsx", L"文档类" }, { L".ppt", L"文档类" }, { L".pptx", L"文档类" },
    { L".pdf", L"文档类" }, { L".txt", L"文档类" }, { L".md", L"文档类" },
    { L".rtf", L"文档类" }, { L".csv", L"文档类" }, { L".odt", L"文档类" },
    { L".ods", L"文档类" }, { L".odp", L"文档类" }, { L".epub", L"文档类" },
    { L".mobi", L"文档类" },
    // 源码/脚本/工程配置 → 代码类
    { L".c", L"代码类" }, { L".cpp", L"代码类" }, { L".h", L"代码类" },
    { L".hpp", L"代码类" }, { L".py", L"代码类" }, { L".java", L"代码类" },
    { L".js", L"代码类" }, { L".jsx", L"代码类" },
    { L".tsx", L"代码类" }, { L".html", L"代码类" }, { L".css", L"代码类" },
    { L".json", L"代码类" }, { L".xml", L"代码类" }, { L".bat", L"代码类" },
    { L".cmd", L"代码类" }, { L".ps1", L"代码类" }, { L".sh", L"代码类" },
    { L".ipynb", L"代码类" }, { L".sql", L"代码类" }, { L".vue", L"代码类" },
    { L".go", L"代码类" }, { L".rs", L"代码类" }, { L".cc", L"代码类" },
    { L".cxx", L"代码类" }, { L".cs", L"代码类" }, { L".rb", L"代码类" },
    { L".php", L"代码类" }, { L".swift", L"代码类" }, { L".kt", L"代码类" },
    { L".scala", L"代码类" }, { L".lua", L"代码类" }, { L".dart", L"代码类" },
    // ---- 图片类 ----
    { L".jpg", L"图片类" }, { L".jpeg", L"图片类" }, { L".png", L"图片类" },
    { L".gif", L"图片类" }, { L".bmp", L"图片类" }, { L".webp", L"图片类" },
    { L".svg", L"图片类" }, { L".ico", L"图片类" }, { L".psd", L"图片类" },
    { L".ai", L"图片类" }, { L".heic", L"图片类" }, { L".tif", L"图片类" },
    { L".tiff", L"图片类" }, { L".raw", L"图片类" }, { L".cr2", L"图片类" },
    { L".nef", L"图片类" }, { L".dng", L"图片类" },
    // ---- 视频类 ----
    { L".mp4", L"视频类" }, { L".mkv", L"视频类" }, { L".avi", L"视频类" },
    { L".mov", L"视频类" }, { L".wmv", L"视频类" }, { L".flv", L"视频类" },
    { L".ts", L"视频类" }, { L".rmvb", L"视频类" }, { L".webm", L"视频类" },
    { L".m4v", L"视频类" }, { L".mpg", L"视频类" }, { L".mpeg", L"视频类" },
    // ---- 音乐类 ----
    { L".mp3", L"音乐类" }, { L".flac", L"音乐类" }, { L".wav", L"音乐类" },
    { L".aac", L"音乐类" }, { L".ogg", L"音乐类" }, { L".m4a", L"音乐类" },
    { L".wma", L"音乐类" }, { L".ape", L"音乐类" }, { L".opus", L"音乐类" },
    // ---- 压缩类 ----
    { L".zip", L"压缩类" }, { L".rar", L"压缩类" }, { L".7z", L"压缩类" },
    { L".tar", L"压缩类" }, { L".gz", L"压缩类" }, { L".bz2", L"压缩类" },
    { L".xz", L"压缩类" }, { L".tgz", L"压缩类" }, { L".cab", L"压缩类" },
    // ---- 安装包类 ----
    { L".exe", L"安装包类" }, { L".msi", L"安装包类" }, { L".msix", L"安装包类" },
    { L".appx", L"安装包类" }, { L".apk", L"安装包类" }, { L".iso", L"安装包类" },
    { L".img", L"安装包类" }
};

static void buildRules(std::vector<Rule>& rules)
{
    const size_t n = sizeof(kRules) / sizeof(kRules[0]);
    for (size_t i = 0; i < n; ++i) {
        Rule r;
        r.cat = kRules[i][0];
        r.key = kRules[i][1];
        rules.push_back(r);
    }
}

static bool isKeptOnDesktop(const std::wstring& name)
{
    const size_t n = sizeof(kKeepOnDesktop) / sizeof(kKeepOnDesktop[0]);
    for (size_t i = 0; i < n; ++i) {
        if (containsNoCase(name, kKeepOnDesktop[i])) return true;
    }
    return false;
}

// 桌面上的一切都参与整理;只排除系统杂物(desktop.ini 等)
static bool isSortableObject(const std::wstring& name)
{
    if (containsNoCase(name, L"desktop.ini"_w)) return false;
    if (containsNoCase(name, L"thumbs.db"_w)) return false;
    return true;
}

// 读取文件开头若干字节(用于解析 .url 内容)
static std::string readFileHead(const std::wstring& path, size_t maxBytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string data;
    char buf[512];
    DWORD got = 0;
    while (data.size() < maxBytes && ReadFile(h, buf, sizeof(buf), &got, NULL) && got > 0) {
        data.append(buf, got);
        if (got < sizeof(buf)) break;
    }
    CloseHandle(h);
    return data;
}

// Steam 的 .url 快捷方式指向 steam://rungameid/xxx —— 兜底规则
static bool isSteamUrl(const std::wstring& path)
{
    std::string head = readFileHead(path, 1024);
    if (head.empty()) return false;
    return containsNoCase(decodeLiteral(head.data(), (int)head.size()), L"URL=steam://"_w);
}

// 名字没命中关键词 → 按扩展名归入文件类;未知类型兜底"其他类"(保证零遗漏)
static std::wstring classifyByExtension(const std::wstring& ext)
{
    const size_t n = sizeof(kExtRules) / sizeof(kExtRules[0]);
    for (size_t i = 0; i < n; ++i) {
        if (ext == kExtRules[i][0]) return kExtRules[i][1];
    }
    return L"其他类";
}

static std::wstring classify(const std::wstring& name, const std::wstring& fullPath,
                             const std::vector<Rule>& rules)
{
    for (size_t i = 0; i < rules.size(); ++i) {
        if (containsNoCase(name, rules[i].key)) return rules[i].cat;
    }
    if (extensionOf(name) == L".url" && isSteamUrl(fullPath)) return L"游戏类";
    return classifyByExtension(extensionOf(name));
}

// 快捷方式(.lnk/.url)= 软件,留在桌面平铺;其余都是文件,归档进文件夹
static bool isShortcutName(const std::wstring& name)
{
    std::wstring e = extensionOf(name);
    return e == L".lnk" || e == L".url";
}

// ---------------------------------------------------------------- 日志(支持撤销)
static std::wstring logPath()
{
    return joinPath(exeDir(), L"desk_sentry.log");
}

struct MoveRecord {
    std::wstring src;
    std::wstring dst;
};

// 排列模式的记录:把某个桌面图标从旧坐标恢复
struct PosRecord {
    std::wstring text;   // 图标显示名(撤销时按名字匹配)
    int x, y;
};

static bool appendLog(const std::vector<MoveRecord>& records)
{
    if (records.empty()) return true;
    std::wstring text;
    for (size_t i = 0; i < records.size(); ++i) {
        text += records[i].src + L"\t" + records[i].dst + L"\n";
    }
    std::string utf8 = toUtf8(text);
    HANDLE h = CreateFileW(logPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    return true;
}

static bool appendPosLog(const std::vector<PosRecord>& records)
{
    if (records.empty()) return true;
    std::wstring text;
    for (size_t i = 0; i < records.size(); ++i) {
        text += L"@POS\t" + records[i].text + L"\t" + toW(records[i].x) + L"\t" + toW(records[i].y) + L"\n";
    }
    std::string utf8 = toUtf8(text);
    HANDLE h = CreateFileW(logPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    return true;
}

static bool readLog(std::vector<MoveRecord>& records, std::vector<PosRecord>* posRecords = NULL)
{
    std::string raw = readFileHead(logPath(), 1024 * 1024);
    if (raw.empty()) return false;
    std::wstring text = decodeLiteral(raw.data(), (int)raw.size());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;
        if (!line.empty() && line[line.size() - 1] == L'\r') line.erase(line.size() - 1);
        // 拆字段(移动记录 2 段;@POS 记录 4 段)
        std::vector<std::wstring> fields;
        size_t f = 0;
        for (;;) {
            size_t t = line.find(L'\t', f);
            if (t == std::wstring::npos) { fields.push_back(line.substr(f)); break; }
            fields.push_back(line.substr(f, t - f));
            f = t + 1;
        }
        if (fields[0] == L"@POS") {
            if (posRecords && fields.size() >= 4) {
                PosRecord p;
                p.text = fields[1];
                p.x = (int)wcstol(fields[2].c_str(), NULL, 10);
                p.y = (int)wcstol(fields[3].c_str(), NULL, 10);
                posRecords->push_back(p);
            }
            continue;
        }
        if (fields.size() < 2) continue;
        MoveRecord rec;
        rec.src = fields[0];
        rec.dst = fields[1];
        records.push_back(rec);
    }
    return !records.empty() || (posRecords && !posRecords->empty());
}

// ---------------------------------------------------------------- 目标路径处理
// 目标已存在时自动追加 " (1)"、" (2)" …
static std::wstring uniquePath(const std::wstring& dir, const std::wstring& name)
{
    std::wstring candidate = joinPath(dir, name);
    if (!fileExists(candidate) && !dirExists(candidate)) return candidate;

    std::wstring stem = name;
    std::wstring ext;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int i = 1; i < 1000; ++i) {
        std::wostringstream oss;
        oss << L" (" << i << L")";
        candidate = joinPath(dir, stem + oss.str() + ext);
        if (!fileExists(candidate) && !dirExists(candidate)) return candidate;
    }
    return L"";
}

static bool ensureDir(const std::wstring& path)
{
    if (dirExists(path)) return true;
    return CreateDirectoryW(path.c_str(), NULL) != 0;
}

// ---------------------------------------------------------------- 扫描
struct Item {
    std::wstring name;
    std::wstring cat;   // 空串 = 未分类
    bool keep;          // true = 系统入口,保留在桌面
};

static void scanDir(const std::wstring& dir, const std::vector<Rule>& rules, std::vector<Item>& items)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;   // 隐藏/系统文件不动
        std::wstring name = fd.cFileName;
        if (containsNoCase(name, L"desk_sentry"_w) ||
            containsNoCase(name, L"desktop_sorter"_w)) continue;   // 忽略本工具的产物(含旧名)
        if (!isSortableObject(name)) continue;

        Item it;
        it.name = name;
        it.keep = false;
        it.cat = L"";
        if (isKeptOnDesktop(name)) {
            it.keep = true;
        } else {
            it.cat = classify(name, joinPath(dir, name), rules);
        }
        items.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ----------------------------------------------------------------桌面模式判定(混合/归档)
static bool g_forceFolders = false;   // --folders:强制用旧的"归档进文件夹"模式

static std::wstring normPath(std::wstring p)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, NULL);
    std::wstring s = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : p;
    while (!s.empty() && (s[s.size() - 1] == L'\\' || s[s.size() - 1] == L'/')) s.erase(s.size() - 1);
    return lowerAscii(s);
}

// 目标是桌面 → 混合整理(文件归档 + 快捷方式平铺);其他目录或 --folders → 全部归档
static bool arrangeMode(const std::wstring& dir)
{
    if (g_forceFolders) return false;
    return normPath(dir) == normPath(desktopDir());
}

struct LvFindCtx { HWND found; };

// 桌面图标列表 = Progman/WorkerW -> SHELLDLL_DefView -> SysListView32
// 只认这两个宿主窗口,绝不会误伤资源管理器文件夹窗口
static BOOL CALLBACK enumLvProc(HWND top, LPARAM lp)
{
    LvFindCtx* ctx = (LvFindCtx*)lp;
    wchar_t cls[64];
    cls[0] = 0;
    GetClassNameW(top, cls, 64);
    if (lstrcmpiW(cls, L"Progman") != 0 && lstrcmpiW(cls, L"WorkerW") != 0) return TRUE;
    HWND def = FindWindowExW(top, NULL, L"SHELLDLL_DefView", NULL);
    if (!def) return TRUE;
    ctx->found = FindWindowExW(def, NULL, L"SysListView32", NULL);
    return ctx->found ? FALSE : TRUE;
}

static HWND findDesktopListView()
{
    LvFindCtx ctx;
    ctx.found = NULL;
    EnumWindows(enumLvProc, (LPARAM)&ctx);
    return ctx.found;
}

struct LvIcon {
    std::wstring text;   // 列表里显示的名字
    int x, y;            // 当前坐标
    int group;           // 0 = 保留/系统入口,1..n = kCategoryOrder 序号+1
};

// 扩展名被系统隐藏时,图标显示的是去掉最后一段后缀的名字
static std::wstring displayName(const std::wstring& name)
{
    size_t dot = name.find_last_of(L'.');
    size_t slash = name.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return name;
    return name.substr(0, dot);
}

// 图标文本 → 分组:分类文件夹名/扫描到的文件 → 对应分类;其余(系统入口、回收站等)→ 0
static int iconGroup(const std::wstring& text, const std::vector<Item>& items)
{
    const size_t ncat = sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]);
    for (size_t c = 0; c < ncat; ++c) {
        if (text == kCategoryOrder[c]) return (int)c + 1;
    }
    if (isKeptOnDesktop(text)) return 0;
    for (int pass = 0; pass < 2; ++pass) {          // 0 = 完整名 1 = 显示名(隐藏扩展名)
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].keep || items[i].cat.empty()) continue;
            bool hit = (pass == 0) ? (items[i].name == text)
                                   : (displayName(items[i].name) == text);
            if (!hit) continue;
            for (size_t c = 0; c < ncat; ++c) {
                if (items[i].cat == kCategoryOrder[c]) return (int)c + 1;
            }
            return 0;
        }
    }
    return 0;   // namespace 图标(此电脑/回收站/网络…)保留原样
}

// -------------------------------------------- MSAA 读取(跨进程)
// 实测:跨进程 SendMessage 读结构体消息(LVM_GETITEMTEXT/GETITEMPOSITION/GETITEMRECT)
// 全部失败(不回传指针数据),而 MSAA(oleacc 的进程内代理)读名字与坐标都正常;
// 写入用 LVM_SETITEMPOSITION(纯值参数)实测有效,可精确还原。
// F9 兼容:oleacc.dll / oleaut32.dll 都是运行时动态加载,不链任何新库。
typedef HRESULT (WINAPI *FnAccessibleObjectFromWindow)(HWND, DWORD, REFIID, void**);
typedef void (WINAPI *FnSysFreeString)(BSTR);

static const GUID kIID_IAccessible = {
    0x618736e0, 0x3c3d, 0x11cf, {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static FnAccessibleObjectFromWindow accFn()
{
    static HMODULE h = NULL;
    static FnAccessibleObjectFromWindow fn = NULL;
    if (!fn) {
        h = LoadLibraryW(L"oleacc.dll");
        if (h) fn = (FnAccessibleObjectFromWindow)GetProcAddress(h, "AccessibleObjectFromWindow");
    }
    return fn;
}

static void freeBstr(BSTR b)
{
    if (!b) return;
    static HMODULE h = LoadLibraryW(L"oleaut32.dll");
    static FnSysFreeString fn = h ? (FnSysFreeString)GetProcAddress(h, "SysFreeString") : NULL;
    if (fn) fn(b);
}

// 用 MSAA 读列表:名字 + 屏幕坐标(转客户区坐标)。
// 子项顺序 = 列表下标;桌面列表尾部有 1 个空名幽灵子项,跳过不计。
static bool readLvIcons(HWND lv, std::vector<LvIcon>& out)
{
    out.clear();
    FnAccessibleObjectFromWindow pAOFW = accFn();
    if (!pAOFW) return false;
    IAccessible* acc = NULL;
    if (FAILED(pAOFW(lv, OBJID_CLIENT, kIID_IAccessible, (void**)&acc)) || !acc) return false;
    long n = 0;
    acc->get_accChildCount(&n);
    RECT lvr;
    GetWindowRect(lv, &lvr);
    for (long i = 1; i <= n; ++i) {
        VARIANT vc;
        memset(&vc, 0, sizeof(vc));
        vc.vt = VT_I4;
        vc.lVal = i;
        BSTR b = NULL;
        acc->get_accName(vc, &b);
        std::wstring text = b ? b : L"";
        freeBstr(b);
        if (text.empty()) continue;                 // 幽灵子项
        long x = 0, y = 0, w = 0, h = 0;
        acc->accLocation(&x, &y, &w, &h, vc);
        LvIcon ic;
        ic.text = text;
        ic.x = (int)x - lvr.left;
        ic.y = (int)y - lvr.top;
        ic.group = -1;
        out.push_back(ic);
    }
    acc->Release();
    return !out.empty();
}

// 等 shell 把文件变动同步进图标列表:该消失的(归档走的)消失、该出现的(还原的)出现。
// 实测归档完成 1 秒多后列表里仍残留旧图标,直接读会把幽灵算进排列,必须先等它稳定。
static void waitForLvSettle(HWND lv, const std::vector<std::wstring>& goneNames,
                            const std::vector<std::wstring>& appearNames)
{
    if (!lv) return;
    if (goneNames.empty() && appearNames.empty()) return;
    for (int tries = 0; tries < 30; ++tries) {          // 最多等 6 秒
        std::vector<LvIcon> chk;
        if (readLvIcons(lv, chk)) {
            bool ok = true;
            for (size_t i = 0; i < chk.size() && ok; ++i) {          // 该消失的必须都不在
                for (size_t j = 0; j < goneNames.size(); ++j) {
                    if (goneNames[j] == chk[i].text || displayName(goneNames[j]) == chk[i].text) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok) {
                for (size_t a = 0; a < appearNames.size() && ok; ++a) {   // 该出现的必须都在
                    bool found = false;
                    for (size_t i = 0; i < chk.size(); ++i) {
                        if (appearNames[a] == chk[i].text ||
                            displayName(appearNames[a]) == chk[i].text) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) ok = false;
                }
            }
            if (ok) return;
        }
        Sleep(200);
    }
}

// 分组升序的稳定排序(组内保持桌面原有先后;不依赖 <algorithm>)
static void stableByGroup(std::vector<int>& idx, const std::vector<LvIcon>& icons)
{
    for (size_t i = 1; i < idx.size(); ++i) {
        int key = idx[i];
        size_t j = i;
        while (j > 0 && icons[idx[j - 1]].group > icons[key].group) {
            idx[j] = idx[j - 1];
            --j;
        }
        idx[j] = key;
    }
}

// 网格推导:原点取现有图标的最小 x/y(观测值必然在系统网格线上);
// 间距取同行/同列的最小正间距 —— 观测间距必是真实网格间距的整数倍,
// 所以"原点 + n×间距"算出的目标坐标一定落在系统网格线上,
// 开着"对齐网格"时吸附结果与目标一致,复核不会出现假偏差。
static void gridFrom(const std::vector<LvIcon>& icons, int& ox, int& oy, int& pw, int& ph)
{
    ox = oy = 0;
    pw = ph = 0;
    if (icons.empty()) return;
    ox = icons[0].x;
    oy = icons[0].y;
    for (size_t i = 0; i < icons.size(); ++i) {
        if (icons[i].x < ox) ox = icons[i].x;
        if (icons[i].y < oy) oy = icons[i].y;
    }
    for (size_t i = 0; i < icons.size(); ++i) {
        for (size_t j = i + 1; j < icons.size(); ++j) {
            if (icons[i].y == icons[j].y) {
                int d = icons[i].x - icons[j].x;
                if (d < 0) d = -d;
                if (d > 0 && (pw == 0 || d < pw)) pw = d;
            }
            if (icons[i].x == icons[j].x) {
                int d = icons[i].y - icons[j].y;
                if (d < 0) d = -d;
                if (d > 0 && (ph == 0 || d < ph)) ph = d;
            }
        }
    }
    if (pw <= 0 || ph <= 0) {   // 某个轴推不出(比如只有一列)→ 用系统图标间距兜底,偏差由复核如实上报
        ICONMETRICSW im;
        memset(&im, 0, sizeof(im));
        im.cbSize = sizeof(im);
        bool ok = SystemParametersInfoW(SPI_GETICONMETRICS, sizeof(im), &im, 0) != 0;
        if (pw <= 0) pw = (ok && im.iHorzSpacing >= 40) ? im.iHorzSpacing : 75;
        if (ph <= 0) ph = (ok && im.iVertSpacing >= 40) ? im.iVertSpacing : 75;
    }
}

struct ArrangePlan {
    HWND lv;
    std::vector<LvIcon> icons;
    std::vector<int> order;    // 排列顺序(存 LV 下标)
};

static bool loadArrangePlan(const std::vector<Item>& items, ArrangePlan& plan)
{
    plan.lv = findDesktopListView();
    if (!plan.lv) return false;
    if (!readLvIcons(plan.lv, plan.icons)) return false;
    for (size_t i = 0; i < plan.icons.size(); ++i) {
        plan.icons[i].group = iconGroup(plan.icons[i].text, items);
    }
    plan.order.resize(plan.icons.size());
    for (size_t i = 0; i < plan.order.size(); ++i) plan.order[i] = (int)i;
    stableByGroup(plan.order, plan.icons);
    return true;
}

// 执行排列:逐格摆放 + 复核;旧坐标写进 snapshot,返回没摆到位的数量(-1 = 复核读不回)
static int applyArrange(const ArrangePlan& plan, std::vector<PosRecord>& snapshot)
{
    snapshot.clear();
    for (size_t i = 0; i < plan.icons.size(); ++i) {
        PosRecord p;
        p.text = plan.icons[i].text;
        p.x = plan.icons[i].x;
        p.y = plan.icons[i].y;
        snapshot.push_back(p);
    }

    int ox = 0, oy = 0, pw = 0, ph = 0;
    gridFrom(plan.icons, ox, oy, pw, ph);
    RECT rc;
    GetWindowRect(plan.lv, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (GetSystemMetrics(SM_CMONITORS) <= 1) {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {   // 单显示器:别压到任务栏下面
            W = wa.right - wa.left;
            H = wa.bottom - wa.top;
        }
    }
    if (ox >= W) ox = 0;
    if (oy >= H) oy = 0;
    int maxCols = (W - ox) / pw;
    if (maxCols < 1) maxCols = 1;
    int rowsFit = (H - oy) / ph;
    if (rowsFit < 1) rowsFit = 1;
    size_t count = plan.order.size();
    bool columnMajor = (count <= (size_t)maxCols * (size_t)rowsFit);   // 放得下:按 Windows 习惯逐列往下排

    std::vector<POINT> target(count);
    for (size_t k = 0; k < count; ++k) {
        int col, row;
        if (columnMajor) { col = (int)k / rowsFit; row = (int)k % rowsFit; }
        else { col = (int)k % maxCols; row = (int)k / maxCols; }
        target[k].x = ox + col * pw;
        target[k].y = oy + row * ph;
    }

    SendMessageW(plan.lv, WM_SETREDRAW, FALSE, 0);
    for (size_t k = 0; k < count; ++k) {
        SendMessageW(plan.lv, LVM_SETITEMPOSITION, (WPARAM)plan.order[k],
                     (LPARAM)MAKELPARAM(target[k].x, target[k].y));
    }
    SendMessageW(plan.lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(plan.lv, NULL, TRUE);
    UpdateWindow(plan.lv);
    Sleep(450);                                   // 等辅助功能层跟上(复核要用 MSAA 回读)

    // 复核:读回坐标,数一数没摆到位的
    std::vector<LvIcon> now;
    if (readLvIcons(plan.lv, now) && now.size() == plan.icons.size()) {
        int miss = 0;
        for (size_t k = 0; k < count; ++k) {
            int idx = plan.order[k];
            if (now[idx].x != target[k].x || now[idx].y != target[k].y) ++miss;
        }
        return miss;
    }
    return -1;
}

// ---------------------------------------------------------------- 模式
enum Mode { MODE_PREVIEW, MODE_APPLY, MODE_UNDO, MODE_RULES };

static void printHelp()
{
    outLn(L"桌面哨兵 DeskSentry —— 桌面图标与文件自动分类排列工具"_w);
    outLn(L""_w);
    outLn(L"  不带任何参数运行:打开图形界面(双击 exe 即可使用)"_w);
    outLn(L""_w);
    outLn(L"  --gui             打开图形界面"_w);
    outLn(L"  --cli             打开命令行交互菜单"_w);
    outLn(L"  --apply           混合整理:文件归档进文件夹,快捷方式分类平铺(记录可撤销)"_w);
    outLn(L"  --folders         与 --apply/--watch 组合:旧版模式,桌面全部东西归档进文件夹"_w);
    outLn(L"  --undo            撤销上一次整理(文件回原位、图标回原坐标)"_w);
    outLn(L"  --rules           查看分类规则表"_w);
    outLn(L"  --watch [秒]      值守模式:新图标出现即自动整理(默认 3 秒一查)"_w);
    outLn(L"  --dir 路径        整理指定目录(非桌面目录自动用归档模式)"_w);
    outLn(L""_w);
    outLn(L"  参数可组合,例如:desk_sentry --watch 5 --dir \"D:\\下载\""_w);
}

static void showRules()
{
    outLn(L"分类规则(按顺序匹配,先命中先生效):"_w);
    std::wstring last;
    for (size_t i = 0; i < sizeof(kRules) / sizeof(kRules[0]); ++i) {
        if (kRules[i][0] != last) {
            last = kRules[i][0];
            outLn(L""_w);
            out(std::wstring(L"  [") + kRules[i][0] + L"]");
        }
        out(std::wstring(L" ") + kRules[i][1]);
    }
    outLn(L""_w);
    outLn(L""_w);
    outLn(L"  文件类(名字没命中关键词时按扩展名归类):"_w);
    outLn(L"    文档类:doc/docx/xls/xlsx/ppt/pptx/pdf/txt/md/csv/epub/源码脚本"_w);
    outLn(L"    图片类:jpg/png/gif/bmp/webp/svg/psd/heic/RAW"_w);
    outLn(L"    视频类:mp4/mkv/avi/mov/rmvb/webm/ts    音乐类:mp3/flac/wav/ape"_w);
    outLn(L"    压缩类:zip/rar/7z/tar/gz/cab           安装包类:exe/msi/apk/iso"_w);
    outLn(L"  兜底:Steam 协议的 .url 归游戏类;其余一切未知类型(含陌生快捷方式)→ 其他类"_w);
    outLn(L"  保留:此电脑 / 回收站 / Microsoft Edge 不参与整理;隐藏文件/桌面.ini 不动"_w);
    outLn(L"  排列:系统入口在最前,其后按上表顺序从左上角逐格平铺(默认不进文件夹)"_w);
}

static int doUndo()
{
    std::vector<MoveRecord> records;
    std::vector<PosRecord> pos;          // 排列模式的位置记录
    if (!readLog(records, &pos)) {
        outLn(L"没有找到可撤销的记录(日志: " + logPath() + L")"_w);
        return 1;
    }
    int ok = 0, fail = 0;
    std::vector<std::wstring> restoredNames;   // 已还原的文件(要等图标回到列表才能恢复坐标)
    for (size_t i = 0; i < records.size(); ++i) {
        const std::wstring& dst = records[i].dst;
        const std::wstring& src = records[i].src;
        if (!fileExists(dst)) {
            outLn(L"  跳过(已不存在): " + dst);
            continue;
        }
        std::wstring back = src;
        if (fileExists(back)) {
            // 原位置已被占用,就在原目录里找个不冲突的名字
            size_t slash = src.find_last_of(L"\\/");
            std::wstring srcDir = (slash == std::wstring::npos) ? L"." : src.substr(0, slash);
            std::wstring base = dst.substr(dst.find_last_of(L"\\/") + 1);
            back = uniquePath(srcDir, base);
        }
        if (back.empty()) {
            ++fail;
            outLn(L"  失败(原位置被占用): " + src);
            continue;
        }
        if (MoveFileW(dst.c_str(), back.c_str())) {
            ++ok;
            outLn(L"  已还原: " + back);
            restoredNames.push_back(back.substr(back.find_last_of(L"\\/") + 1));
        } else {
            ++fail;
            outLn(L"  失败: " + dst);
        }
    }
    // ---- 排列记录:后写的先恢复,把图标坐标放回原处 ----
    // 等 shell 把刚还原的文件图标加回列表,坐标恢复才找得到它们
    if (!pos.empty()) {
        waitForLvSettle(findDesktopListView(), std::vector<std::wstring>(), restoredNames);
    }
    int posOk = 0, posSkip = 0;
    bool posDead = false;
    if (!pos.empty()) {
        HWND lv = findDesktopListView();
        if (!lv) {
            posDead = true;
            outLn(L"  桌面不可用,图标位置暂未恢复(日志保留,稍后可再试 --undo)。"_w);
        } else {
            std::vector<LvIcon> icons;
            readLvIcons(lv, icons);
            SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
            for (size_t i = pos.size(); i-- > 0; ) {
                int idx = -1;
                for (size_t j = 0; j < icons.size(); ++j) {
                    if (icons[j].text == pos[i].text) { idx = (int)j; break; }
                }
                if (idx < 0) { ++posSkip; continue; }
                SendMessageW(lv, LVM_SETITEMPOSITION, (WPARAM)idx,
                             (LPARAM)MAKELPARAM(pos[i].x, pos[i].y));
                ++posOk;
            }
            SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(lv, NULL, TRUE);
            UpdateWindow(lv);
            if (posOk > 0) outLn(L"  已恢复 " + toW(posOk) + L" 个图标的原坐标。"_w);
            if (posSkip > 0) outLn(L"  跳过(图标已不在桌面): " + toW(posSkip) + L" 个。"_w);
        }
    }

    if (fail == 0 && !posDead) {
        DeleteFileW(logPath().c_str());
        // 顺手清掉撤销后变空的分类文件夹(仅归档模式会产生)
        int removed = 0;
        if (!records.empty()) {
            size_t slashAt = records[0].src.find_last_of(L"\\/");
            std::wstring root = (slashAt == std::wstring::npos) ? L"." : records[0].src.substr(0, slashAt);
            size_t ncat = sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]);
            for (size_t c = 0; c < ncat; ++c) {
                std::wstring dir = root + L"\\" + kCategoryOrder[c];
                if (dirExists(dir) && isDirEmpty(dir) && RemoveDirectoryW(dir.c_str())) ++removed;
            }
        }
        if (!records.empty() && !pos.empty()) {
            outLn(L"撤销完成:还原 " + toW(ok) + L" 项,恢复 " + toW(posOk) + L" 个图标位置,日志已清除。"_w);
        } else if (!pos.empty()) {
            outLn(L"撤销完成:恢复 " + toW(posOk) + L" 个图标原坐标,日志已清除。"_w);
        } else {
            outLn(L"撤销完成:还原 " + toW(ok) + L" 项,日志已清除。"_w);
        }
        if (removed > 0) outLn(L"清理了 " + toW(removed) + L" 个空文件夹。"_w);
    } else if (fail == 0 && posDead) {
        outLn(L"撤销完成:文件已还原,但桌面位置未恢复 —— 日志保留,稍后再试。"_w);
    } else {
        outLn(L"撤销完成:还原 " + toW(ok) + L" 项,失败 " + toW(fail) + L" 项。"_w);
    }
    return fail == 0 ? 0 : 1;
}

// 把 todo[i] 移入 dir\todoCat[i](重名自动加 (1) 后缀);逐项打印,返回失败数
static int archiveFiles(const std::wstring& dir,
                        const std::vector<std::wstring>& todo,
                        const std::vector<std::wstring>& todoCat,
                        std::vector<MoveRecord>& records, int& ok)
{
    ok = 0;
    int fail = 0;
    const size_t ncat = sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]);
    for (size_t c = 0; c < ncat; ++c) {
        std::wstring cat = kCategoryOrder[c];
        std::wstring target = joinPath(dir, cat);
        for (size_t i = 0; i < todo.size(); ++i) {
            if (todoCat[i] != cat) continue;
            std::wstring src = joinPath(dir, todo[i]);
            if (!ensureDir(target)) {
                ++fail;
                outLn(L"  创建目录失败: " + target);
                continue;
            }
            std::wstring dst = uniquePath(target, todo[i]);
            if (dst.empty()) {
                ++fail;
                outLn(L"  目标重名过多,跳过: " + todo[i]);
                continue;
            }
            if (MoveFileW(src.c_str(), dst.c_str())) {
                MoveRecord rec;
                rec.src = src;
                rec.dst = dst;
                records.push_back(rec);
                ++ok;
                outLn(L"  已移动: " + todo[i] + L"  -> " + cat);
            } else {
                ++fail;
                outLn(L"  移动失败: " + todo[i] + L" (错误码 " + toW(GetLastError()) + L")");
            }
        }
    }
    return fail;
}

// 打印排列清单(预览不带格号,执行带格号);含自动排列检查与规则说明
static void printArrangeList(const ArrangePlan& plan, bool withSeq)
{
    if (GetWindowLongPtrW(plan.lv, GWL_STYLE) & LVS_AUTOARRANGE) {
        outLn(L"注意:桌面开着「自动排列图标」,Windows 会强制按名称重排,分类顺序不生效。"_w);
        outLn(L"  桌面右键 → 查看 → 取消勾选 自动排列图标,再执行即可按分类排好。"_w);
        outLn(L""_w);
    }
    size_t k = 0;
    int seq = 0;
    while (k < plan.order.size()) {
        int g = plan.icons[plan.order[k]].group;
        size_t e = k;
        while (e < plan.order.size() && plan.icons[plan.order[e]].group == g) ++e;
        size_t cnt = e - k;
        std::wstring title = (g <= 0) ? std::wstring(L"系统入口/保留") : std::wstring(kCategoryOrder[g - 1]);
        outLn(L"[" + title + L"] " + toW((long)cnt) + L" 项"_w);
        for (; k < e; ++k) {
            ++seq;
            outLn(L"    " + plan.icons[plan.order[k]].text +
                  (withSeq ? (L"  -> 第" + toW(seq) + L"格") : L""));
        }
        outLn(L""_w);
    }
    outLn(L"排列规则:系统入口在最前,其余按分类顺序从左上角逐格平铺。"_w);
}

static int run(Mode mode, const std::wstring& dir)
{
    if (!dirExists(dir)) {
        outLn(L"目录不存在: " + dir);
        return 1;
    }

    std::vector<Rule> rules;
    buildRules(rules);

    std::vector<Item> items;
    scanDir(dir, rules, items);

    // ---- 分流 ----
    // 混合模式(桌面):文件(非快捷方式)按扩展名归档进文件夹;快捷方式只排列不归档。
    // 归档模式(--folders/其他目录):全部已分类文件照旧归档。
    std::vector<std::wstring> todo;          // 待归档文件名
    std::vector<std::wstring> todoCat;        // 与 todo 平行的目标分类
    std::vector<std::wstring> unclassified;   // 归档模式:没匹配上规则的(留原位)
    std::vector<std::wstring> kept;          // 保留的系统入口
    const bool arrange = arrangeMode(dir);
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].keep) { kept.push_back(items[i].name); continue; }
        if (arrange && isShortcutName(items[i].name)) continue;   // 快捷方式 → 只排列
        std::wstring cat = arrange ? classifyByExtension(extensionOf(items[i].name))
                                   : items[i].cat;
        if (cat.empty()) { unclassified.push_back(items[i].name); continue; }
        todo.push_back(items[i].name);
        todoCat.push_back(cat);
    }

    // 排列清单:预览先建(归档后文件还在桌面,要剔掉);执行时归档完成后再建
    ArrangePlan plan;
    if (arrange && mode == MODE_PREVIEW) {
        if (!loadArrangePlan(items, plan)) {
            outLn(L"找不到桌面图标列表,无法排列(桌面被锁定或处于远程会话)。"_w);
            return 1;
        }
        std::vector<int> filtered;
        for (size_t k = 0; k < plan.order.size(); ++k) {
            const std::wstring& t = plan.icons[plan.order[k]].text;
            bool willArchive = false;
            for (size_t i = 0; i < todo.size(); ++i) {
                if (todo[i] == t || displayName(todo[i]) == t) { willArchive = true; break; }
            }
            if (!willArchive) filtered.push_back(plan.order[k]);
        }
        plan.order.swap(filtered);
    }

    outLn(L"目录: " + dir);
    if (arrange) {
        outLn(mode == MODE_APPLY ? L"模式: 混合整理(文件归档进文件夹 + 快捷方式分类平铺)"_w
                                 : L"模式: 预览,未改动任何文件/图标(加 --apply 执行)"_w);
    } else {
        outLn(mode == MODE_APPLY ? L"模式: 归档执行(--apply --folders)"_w
                                 : L"模式: 预览,未改动任何文件(加 --apply 执行)"_w);
    }
    outLn(L""_w);

    const size_t ncat = sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]);
    size_t total = 0;      // 归档模式:待整理数
    size_t totalArc = 0;   // 混合模式:将归档的文件数
    size_t totalPos = 0;   // 混合模式:将排列的图标数

    if (arrange) {
        // (A) 将归档的文件 —— 按目标文件夹分组(执行时细节由逐项打印给出)
        if (!todo.empty()) {
            outLn(L"[归档] 文件进文件夹:"_w);
            for (size_t c = 0; c < ncat; ++c) {
                std::wstring cat = kCategoryOrder[c];
                size_t cnt = 0;
                for (size_t i = 0; i < todoCat.size(); ++i) if (todoCat[i] == cat) ++cnt;
                if (cnt == 0) continue;
                outLn(L"  [" + cat + L"] " + toW(cnt) + L" 项"_w);
                if (mode == MODE_PREVIEW) {
                    for (size_t i = 0; i < todo.size(); ++i) {
                        if (todoCat[i] == cat) outLn(L"      " + todo[i]);
                    }
                }
                totalArc += cnt;
            }
            outLn(L""_w);
        }
        // (B) 将平铺排列的图标 —— 预览时清单已就绪;执行时归档完成后才建
        if (mode == MODE_PREVIEW && !plan.order.empty()) {
            printArrangeList(plan, false);
            totalPos = plan.order.size();
        }
    } else {
        // 按分类打印(归档模式)
        for (size_t c = 0; c < ncat; ++c) {
            std::wstring cat = kCategoryOrder[c];
            std::vector<std::wstring> group;
            for (size_t i = 0; i < todo.size(); ++i) {
                if (todoCat[i] == cat) group.push_back(todo[i]);
            }
            if (group.empty()) continue;
            outLn(L"[" + cat + L"] " + toW(group.size()) + L" 项"_w);
            for (size_t i = 0; i < group.size(); ++i) {
                outLn(L"    " + group[i] + (mode == MODE_APPLY ? L"  -> " + cat : L""));
            }
            outLn(L""_w);
            total += group.size();
        }
        if (!unclassified.empty()) {
            outLn(L"[未分类,保持原位] " + toW(unclassified.size()) + L" 项"_w);
            for (size_t i = 0; i < unclassified.size(); ++i) outLn(L"    " + unclassified[i]);
            outLn(L""_w);
        }
        if (!kept.empty()) {
            outLn(L"[系统入口,保留] " + toW(kept.size()) + L" 项"_w);
            for (size_t i = 0; i < kept.size(); ++i) outLn(L"    " + kept[i]);
            outLn(L""_w);
        }
    }

    if (mode == MODE_PREVIEW) {
        if (arrange) {
            outLn(L"合计:归档 " + toW(totalArc) + L" 项,排列 " + toW(totalPos) + L" 项。"_w);
            if (totalArc + totalPos > 0) outLn(L"确认无误后执行: desk_sentry --apply"_w);
        } else {
            outLn(L"合计 " + toW(total) + L" 项待整理。"_w);
            if (total > 0) outLn(L"确认无误后执行: desk_sentry --apply"_w);
        }
        return 0;
    }

    // ---- 执行:混合整理(桌面) —— 先归档文件,再平铺排列 ----
    if (arrange) {
        // 0) 归档前记下文件图标的原坐标(撤销时文件还原后可精确复位)
        std::vector<LvIcon> lvNow;
        HWND lv0 = findDesktopListView();
        if (lv0 && readLvIcons(lv0, lvNow)) {
            std::vector<PosRecord> filePos;
            for (size_t i = 0; i < lvNow.size(); ++i) {
                const std::wstring& t = lvNow[i].text;
                bool isShortcutIcon = false;   // 与快捷方式重名的图标归快捷方式,不重复记
                for (size_t s = 0; s < items.size(); ++s) {
                    if (!isShortcutName(items[s].name)) continue;
                    if (items[s].name == t || displayName(items[s].name) == t) {
                        isShortcutIcon = true;
                        break;
                    }
                }
                if (isShortcutIcon) continue;
                for (size_t j = 0; j < todo.size(); ++j) {
                    if (todo[j] == t || displayName(todo[j]) == t) {
                        PosRecord p;
                        p.text = t;
                        p.x = lvNow[i].x;
                        p.y = lvNow[i].y;
                        filePos.push_back(p);
                        break;
                    }
                }
            }
            if (!filePos.empty() && !appendPosLog(filePos)) {
                outLn(L"警告:日志写入失败,--undo 可能不可用。"_w);
            }
        }

        // 1) 文件归档进分类文件夹
        std::vector<MoveRecord> records;
        int okArc = 0;
        int failArc = archiveFiles(dir, todo, todoCat, records, okArc);
        if (!records.empty() && !appendLog(records)) {
            outLn(L"警告:日志写入失败,--undo 可能不可用。"_w);
        }
        outLn(L"归档完成:移动 " + toW(okArc) + L" 项,失败 " + toW(failArc) + L" 项。"_w);
        outLn(L""_w);

        // 2) 等 shell 把归档变动同步进图标列表,再重建排列清单
        HWND lv1 = findDesktopListView();
        std::vector<std::wstring> wantFolders;
        for (size_t i = 0; i < todoCat.size(); ++i) {
            bool dup = false;
            for (size_t j = 0; j < wantFolders.size(); ++j) {
                if (wantFolders[j] == todoCat[i]) { dup = true; break; }
            }
            if (!dup) wantFolders.push_back(todoCat[i]);
        }
        waitForLvSettle(lv1, todo, wantFolders);
        if (!loadArrangePlan(items, plan)) {
            outLn(L"找不到桌面图标列表,排列未执行(文件已归档,可稍后 --apply 补排)。"_w);
            return failArc > 0 ? 1 : 0;
        }
        printArrangeList(plan, true);
        outLn(L""_w);

        // 3) 平铺排列 + 复核
        std::vector<PosRecord> snap;
        int miss = applyArrange(plan, snap);
        bool autoArr = ((GetWindowLongPtrW(plan.lv, GWL_STYLE) & LVS_AUTOARRANGE) != 0);
        if (!snap.empty() && !appendPosLog(snap)) {
            outLn(L"警告:日志写入失败,--undo 可能不可用。"_w);
        }
        outLn(L""_w);
        long cnt = (long)plan.order.size();
        if (miss < 0) outLn(L"完成:已排列,但复核未读回坐标(以桌面实际显示为准)。"_w);
        else if (miss > 0) outLn(L"完成:已排列 " + toW(cnt - miss) + L" 项,复核 " + toW(miss) + L" 项与目标有出入。"_w);
        else outLn(L"完成:已按分类顺序排列 " + toW(cnt) + L" 项。"_w);
        if (autoArr) outLn(L"(桌面开着自动排列图标,系统可能会再按名称重排 —— 在桌面右键 → 查看 里关掉即可保持)"_w);
        else outLn(L"旧坐标已记录,如需回滚: desk_sentry --undo"_w);
        return (miss > 0 || failArc > 0) ? 1 : 0;
    }

    // ---- 执行:归档进文件夹(--folders / 非桌面目录) ----
    std::vector<MoveRecord> records;
    int okArc = 0;
    int failArc = archiveFiles(dir, todo, todoCat, records, okArc);
    if (!records.empty() && !appendLog(records)) {
        outLn(L"警告:日志写入失败,--undo 可能不可用。"_w);
    }
    outLn(L""_w);
    outLn(L"完成:移动 " + toW(okArc) + L" 项,失败 " + toW(failArc) + L" 项。"_w);
    if (!records.empty()) {
        outLn(L"日志: " + logPath());
        outLn(L"如需回滚,运行: desk_sentry --undo"_w);
    }
    return failArc == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- 值守模式
static volatile bool gStopWatch = false;

// Ctrl+C / Ctrl+Break / 关闭窗口:让值守循环自然结束,而不是直接杀进程
static BOOL WINAPI onCtrlEvent(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        gStopWatch = true;
        return TRUE;                 // 我们自己处理,不走默认的"终止进程"
    }
    return FALSE;
}

// 桌面顶层名字快照(含目录)—— 用来发现"有新东西出现/消失"
static void watchSignature(const std::wstring& dir, std::vector<std::wstring>& out)
{
    out.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
        if (containsNoCase(n, L"desk_sentry"_w) || containsNoCase(n, L"desktop_sorter"_w)) continue;
        out.push_back(((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"[D]" : L"") + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 反复扫描目录:桌面 → 新东西出现就重新平铺排列;归档模式 → 新图标立刻收进分类文件夹
static int watch(const std::wstring& dir, int intervalSec)
{
    if (!dirExists(dir)) {
        outLn(L"目录不存在: " + dir);
        return 1;
    }
    gStopWatch = false;
    SetConsoleCtrlHandler(onCtrlEvent, TRUE);

    std::vector<Rule> rules;
    buildRules(rules);

    const bool arrange = arrangeMode(dir);
    outLn(L"自动值守已启动,每 " + toW(intervalSec) + L" 秒扫描一次。");
    if (arrange) outLn(L"桌面再冒出新图标会自动归位到所属分类,按 Ctrl+C 结束。");
    else outLn(L"桌面再冒出新图标会自动归入对应文件夹,按 Ctrl+C 结束。");
    outLn(L"");

    int moved = 0;
    std::vector<std::wstring> lastSig;
    if (arrange) {
        watchSignature(dir, lastSig);
        outLn(L"值守开始,先把桌面整理一次:");
        run(MODE_APPLY, dir);
    }
    for (;;) {
        if (arrange) {
            std::vector<std::wstring> sig;
            watchSignature(dir, sig);
            if (sig != lastSig) {
                lastSig.swap(sig);
                outLn(L"桌面有变化,按分类重新排列:");
                run(MODE_APPLY, dir);
                ++moved;
            }
        } else {
            std::vector<Item> items;
            scanDir(dir, rules, items);
            for (size_t i = 0; i < items.size() && !gStopWatch; ++i) {
                if (items[i].keep || items[i].cat.empty()) continue;
                std::wstring target = joinPath(dir, items[i].cat);
                if (!ensureDir(target)) continue;
                std::wstring dst = uniquePath(target, items[i].name);
                if (dst.empty()) continue;
                std::wstring src = joinPath(dir, items[i].name);
                if (MoveFileW(src.c_str(), dst.c_str())) {
                    MoveRecord rec;
                    rec.src = src;
                    rec.dst = dst;
                    std::vector<MoveRecord> one;
                    one.push_back(rec);
                    appendLog(one);          // 记录日志,事后仍可用 --undo 批量回滚
                    ++moved;
                    outLn(L"  发现新图标 -> [" + items[i].cat + L"] " + items[i].name);
                }
            }
        }
        // 分片睡眠,便于及时响应 Ctrl+C
        for (int k = 0; k < intervalSec * 5 && !gStopWatch; ++k) Sleep(200);
        if (gStopWatch) break;
    }
    SetConsoleCtrlHandler(onCtrlEvent, FALSE);
    outLn(L""_w);
    if (arrange) outLn(L"值守结束:本次共自动重新排列 " + toW(moved) + L" 次。");
    else outLn(L"值守结束:本次共自动归类 " + toW(moved) + L" 项。");
    return 0;
}

// ---------------------------------------------------------------- GUI 图形界面
// 双击 exe 默认进入图形界面:程序内起一个只监听 127.0.0.1 的迷你 HTTP 服务器,
// 界面是同目录的 desk_sentry_gui.html,再用 Edge --app 模式拉起无边框应用窗口。
// winsock 用 LoadLibrary 动态加载 —— 不链接 -lws2_32,小熊猫 F9 单文件编译也能过。

typedef int    (WSAAPI *PFN_WSAStartup)(WORD, LPWSADATA);
typedef int    (WSAAPI *PFN_WSACleanup)(void);
typedef SOCKET (WSAAPI *PFN_socket)(int, int, int);
typedef int    (WSAAPI *PFN_bind)(SOCKET, const sockaddr*, int);
typedef int    (WSAAPI *PFN_listen)(SOCKET, int);
typedef SOCKET (WSAAPI *PFN_accept)(SOCKET, sockaddr*, int*);
typedef int    (WSAAPI *PFN_connect)(SOCKET, const sockaddr*, int);
typedef int    (WSAAPI *PFN_recv)(SOCKET, char*, int, int);
typedef int    (WSAAPI *PFN_send)(SOCKET, const char*, int, int);
typedef int    (WSAAPI *PFN_closesocket)(SOCKET);
typedef int    (WSAAPI *PFN_setsockopt)(SOCKET, int, int, const char*, int);
typedef u_short(WSAAPI *PFN_htons)(u_short);
typedef unsigned long (WSAAPI *PFN_inet_addr)(const char*);

static PFN_WSAStartup   pWSAStartup  = NULL;
static PFN_WSACleanup   pWSACleanup  = NULL;
static PFN_socket       psocket      = NULL;
static PFN_bind         pbind        = NULL;
static PFN_listen       plisten      = NULL;
static PFN_accept       paccept      = NULL;
static PFN_connect      pconnect     = NULL;
static PFN_recv         precv        = NULL;
static PFN_send         psend        = NULL;
static PFN_closesocket  pclosesocket = NULL;
static PFN_setsockopt   psetsockopt  = NULL;
static PFN_htons        phtons       = NULL;
static PFN_inet_addr    pinet_addr   = NULL;
static HMODULE          g_wsaDll     = NULL;

static bool loadWinsock()
{
    if (g_wsaDll) return true;
    g_wsaDll = LoadLibraryW(L"ws2_32.dll");
    if (!g_wsaDll) return false;
    pWSAStartup  = (PFN_WSAStartup)GetProcAddress(g_wsaDll, "WSAStartup");
    pWSACleanup  = (PFN_WSACleanup)GetProcAddress(g_wsaDll, "WSACleanup");
    psocket      = (PFN_socket)GetProcAddress(g_wsaDll, "socket");
    pbind        = (PFN_bind)GetProcAddress(g_wsaDll, "bind");
    plisten      = (PFN_listen)GetProcAddress(g_wsaDll, "listen");
    paccept      = (PFN_accept)GetProcAddress(g_wsaDll, "accept");
    pconnect     = (PFN_connect)GetProcAddress(g_wsaDll, "connect");
    precv        = (PFN_recv)GetProcAddress(g_wsaDll, "recv");
    psend        = (PFN_send)GetProcAddress(g_wsaDll, "send");
    pclosesocket = (PFN_closesocket)GetProcAddress(g_wsaDll, "closesocket");
    psetsockopt  = (PFN_setsockopt)GetProcAddress(g_wsaDll, "setsockopt");
    phtons       = (PFN_htons)GetProcAddress(g_wsaDll, "htons");
    pinet_addr   = (PFN_inet_addr)GetProcAddress(g_wsaDll, "inet_addr");
    return pWSAStartup && pWSACleanup && psocket && pbind && plisten && paccept
        && pconnect && precv && psend && pclosesocket && psetsockopt && phtons && pinet_addr;
}

static std::wstring g_guiDir;            // 界面控制的目录(默认桌面)
static std::wstring g_exeDir;
static std::string  g_token;             // 随机令牌,防其他网页乱调本地接口
static int          g_guiPort = 0;
static int          g_listenSock = -1;
static int          g_guiWatchSec = 3;
static volatile LONG g_watching = 0;

static int interactiveMenu();            // 前置声明(GUI 降级时要用)

static std::string jsonEscU8(const std::string& u8)
{
    std::string r;
    for (size_t i = 0; i < u8.size(); ++i) {
        char c = u8[i];
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if ((unsigned char)c < 0x20) { char b[8]; sprintf(b, "\\u%04x", (int)(unsigned char)c); r += b; }
        else r += c;
    }
    return r;
}

static std::string jsonEsc(const std::wstring& s)
{
    return jsonEscU8(toUtf8(s));
}

static std::string readWholeFile(const std::wstring& path)
{
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 8 * 1024 * 1024) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (!ReadFile(h, &out[0], (DWORD)out.size(), &rd, NULL)) out.clear();
        else out.resize(rd);
    }
    CloseHandle(h);
    return out;
}

static std::wstring exeDirOf()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf, (n && n < MAX_PATH) ? n : 0);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

static std::wstring stateFilePath()
{
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring base = (n && n < MAX_PATH) ? std::wstring(tmp) : L".\\";
    return base + L"desk_sentry_gui.state";
}

// ---------------------------------------------------- HTTP 收发小件
static void sendAll(SOCKET s, const std::string& data)
{
    int off = 0;
    while (off < (int)data.size()) {
        int n = psend(s, data.data() + off, (int)data.size() - off, 0);
        if (n <= 0) return;
        off += n;
    }
}

static void httpSend(SOCKET s, int code, const char* status, const char* ctype, const std::string& body)
{
    char head[384];
    int hl = sprintf(head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n",
        code, status, ctype, (unsigned)body.size());
    std::string h(head, hl);
    sendAll(s, h + body);
}

static std::string hdrVal(const std::string& lower, const std::string& all, const char* name)
{
    std::string key = std::string("\r\n") + name + ":";
    size_t p = lower.find(key);
    if (p == std::string::npos) return "";
    size_t vs = p + key.size();
    size_t e = lower.find("\r\n", vs);
    std::string v = all.substr(vs, (e == std::string::npos ? all.size() : e) - vs);
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
    while (!v.empty() && (v[v.size() - 1] == ' ' || v[v.size() - 1] == '\t')) v.erase(v.size() - 1);
    return v;
}

// ---------------------------------------------------- 值守线程控制
static DWORD WINAPI watchProc(LPVOID)
{
    watch(g_guiDir, g_guiWatchSec);
    InterlockedExchange(&g_watching, 0);
    return 0;
}

static void watchStart(int sec)
{
    if (sec < 1 || sec > 3600) sec = 3;
    if (g_watching) {
        if (sec == g_guiWatchSec) return;
        gStopWatch = true;                       // 换间隔:先停旧线程
        for (int i = 0; i < 30 && g_watching; ++i) Sleep(100);
    }
    g_guiWatchSec = sec;
    InterlockedExchange(&g_watching, 1);
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, watchProc, NULL, 0, &tid);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_watching, 0);
}

static void watchStop()
{
    if (g_watching) gStopWatch = true;
}

// ---------------------------------------------------- 接口实现
static std::string buildStateJson(const std::wstring& dir)
{
    std::vector<Rule> rules;
    buildRules(rules);
    std::vector<Item> items;
    scanDir(dir, rules, items);

    const size_t ncat = sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]);
    std::vector<int> cnt(ncat, 0);
    std::vector<std::vector<std::wstring> > files(ncat);
    std::vector<int> hasDir(ncat, 0);
    int todo = 0, kept = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].keep) { ++kept; continue; }
        if (items[i].cat.empty()) continue;
        ++todo;
        for (size_t c = 0; c < ncat; ++c) {
            if (items[i].cat == kCategoryOrder[c]) {
                ++cnt[c];
                if (files[c].size() < 80) files[c].push_back(items[i].name);
                break;
            }
        }
    }
    int folders = 0;
    for (size_t c = 0; c < ncat; ++c) {
        if (dirExists(joinPath(dir, kCategoryOrder[c]))) { hasDir[c] = 1; ++folders; }
    }

    std::vector<MoveRecord> recs;
    bool canUndo = readLog(recs);

    std::ostringstream o;
    o << "{\"dir\":\"" << jsonEsc(dir) << "\""
      << ",\"watching\":" << (g_watching ? "true" : "false")
      << ",\"watchSec\":" << g_guiWatchSec
      << ",\"total\":" << (int)items.size()
      << ",\"todo\":" << todo
      << ",\"kept\":" << kept
      << ",\"folders\":" << folders
      << ",\"canUndo\":" << (canUndo ? "true" : "false")
      << ",\"groups\":[";
    bool first = true;
    for (size_t c = 0; c < ncat; ++c) {
        if (cnt[c] == 0 && !hasDir[c]) continue;
        if (!first) o << ",";
        first = false;
        o << "{\"cat\":\"" << jsonEsc(kCategoryOrder[c]) << "\",\"n\":" << cnt[c] << ",\"files\":[";
        for (size_t f = 0; f < files[c].size(); ++f) {
            if (f) o << ",";
            o << "\"" << jsonEsc(files[c][f]) << "\"";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

static std::string buildActivityJson(const std::string& query)
{
    long since = -1;
    size_t sp = query.find("since=");
    if (sp != std::string::npos) since = atol(query.c_str() + sp + 6);

    ensureCapCs();
    EnterCriticalSection(&g_capCs);
    unsigned long total = g_capBase + (unsigned long)g_capBuf.size();
    std::string text;
    if (since >= 0 && (unsigned long)since >= g_capBase && (unsigned long)since <= total) {
        text = toUtf8(g_capBuf.substr((size_t)((unsigned long)since - g_capBase)));
    } else {
        std::wstring tail = g_capBuf.size() > 4000 ? g_capBuf.substr(g_capBuf.size() - 4000) : g_capBuf;
        text = toUtf8(tail);
    }
    LeaveCriticalSection(&g_capCs);

    std::ostringstream o;
    o << "{\"total\":" << total << ",\"text\":\"" << jsonEscU8(text) << "\"}";
    return o.str();
}

// ---------------------------------------------------- 请求处理
static bool tokenOk(const std::string& query, const std::string& htok)
{
    std::string t = htok;
    if (t.empty()) {
        size_t p = query.find("t=");
        if (p != std::string::npos) {
            t = query.substr(p + 2);
            size_t e = t.find('&');
            if (e != std::string::npos) t = t.substr(0, e);
        }
    }
    return !g_token.empty() && t == g_token;
}

static int parseSecFromBody(const std::string& body, int defSec)
{
    size_t p = body.find("sec");
    if (p == std::string::npos) return defSec;
    size_t d = body.find_first_of("0123456789", p);
    if (d == std::string::npos) return defSec;
    int v = atoi(body.c_str() + d);
    return (v >= 1 && v <= 3600) ? v : defSec;
}

static void handleConn(SOCKET s)
{
    // 读请求头
    std::string buf;
    char tmp[2048];
    for (;;) {
        int n = precv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) { pclosesocket(s); return; }
        buf.append(tmp, n);
        if (buf.find("\r\n\r\n") != std::string::npos) break;
        if (buf.size() > 65536) { pclosesocket(s); return; }
    }

    std::string lower = buf;
    for (size_t i = 0; i < lower.size(); ++i)
        if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] = (char)(lower[i] - 'A' + 'a');

    // 防 DNS rebinding:只认本机 Host
    std::string host = hdrVal(lower, buf, "host");
    if (host.find("127.0.0.1") == std::string::npos && host.find("localhost") == std::string::npos) {
        httpSend(s, 403, "Forbidden", "text/plain; charset=utf-8", "forbidden");
        pclosesocket(s);
        return;
    }

    std::string method, target;
    {
        size_t e = buf.find("\r\n");
        std::istringstream iss(buf.substr(0, e == std::string::npos ? buf.size() : e));
        iss >> method >> target;
    }
    std::string path = target, query;
    size_t qpos = target.find('?');
    if (qpos != std::string::npos) { path = target.substr(0, qpos); query = target.substr(qpos + 1); }

    // POST 体
    std::string body;
    size_t hdrEnd = buf.find("\r\n\r\n");
    if (hdrEnd != std::string::npos) {
        body = buf.substr(hdrEnd + 4);
        int clen = atoi(hdrVal(lower, buf, "content-length").c_str());
        if (clen > 0 && clen < 65536) {
            while ((int)body.size() < clen) {
                int n = precv(s, tmp, sizeof(tmp), 0);
                if (n <= 0) break;
                body.append(tmp, n);
            }
        }
    }

    std::string xtok = hdrVal(lower, buf, "x-token");

    // ---- 路由 ----
    if (path == "/") {
        if (!tokenOk(query, xtok)) {
            httpSend(s, 403, "Forbidden", "text/plain; charset=utf-8",
                     "forbidden - 请通过桌面哨兵程序打开本页面");
        } else {
            std::string html = readWholeFile(g_exeDir + L"\\desk_sentry_gui.html");
            if (html.empty()) {
                httpSend(s, 404, "Not Found", "text/plain; charset=utf-8",
                         "desk_sentry_gui.html not found");
            } else {
                for (size_t p = html.find("__DS_TOKEN__"); p != std::string::npos; p = html.find("__DS_TOKEN__", p + g_token.size()))
                    html.replace(p, 12, g_token);
                httpSend(s, 200, "OK", "text/html; charset=utf-8", html);
            }
        }
    }
    else if (path == "/icon") {
        std::string ico = readWholeFile(g_exeDir + L"\\desk_sentry.ico");
        const char* ct = "image/x-icon";
        if (ico.empty()) { ico = readWholeFile(g_exeDir + L"\\assets-icon.png"); ct = "image/png"; }
        if (ico.empty()) httpSend(s, 404, "Not Found", "text/plain", "no icon");
        else httpSend(s, 200, "OK", ct, ico);
    }
    else if (!tokenOk(query, xtok)) {
        httpSend(s, 403, "Forbidden", "application/json", "{\"ok\":false,\"err\":\"bad token\"}");
    }
    else if (path == "/api/ping") {
        httpSend(s, 200, "OK", "application/json", "{\"ok\":true,\"app\":\"desk_sentry\"}");
    }
    else if (path == "/api/state") {
        httpSend(s, 200, "OK", "application/json; charset=utf-8", buildStateJson(g_guiDir));
    }
    else if (path == "/api/activity") {
        httpSend(s, 200, "OK", "application/json; charset=utf-8", buildActivityJson(query));
    }
    else if (path == "/api/preview") {
        run(MODE_PREVIEW, g_guiDir);
        httpSend(s, 200, "OK", "application/json", "{\"ok\":true}");
    }
    else if (path == "/api/apply") {
        run(MODE_APPLY, g_guiDir);
        httpSend(s, 200, "OK", "application/json", "{\"ok\":true}");
    }
    else if (path == "/api/undo") {
        doUndo();
        httpSend(s, 200, "OK", "application/json", "{\"ok\":true}");
    }
    else if (path == "/api/watch/start") {
        watchStart(parseSecFromBody(body, 3));
        httpSend(s, 200, "OK", "application/json", "{\"ok\":true}");
    }
    else if (path == "/api/watch/stop") {
        watchStop();
        httpSend(s, 200, "OK", "application/json", "{\"ok\":true}");
    }
    else if (path == "/api/quit") {
        httpSend(s, 200, "OK", "application/json", "{\"ok\":true}");
        watchStop();
        Sleep(200);
        DeleteFileW(stateFilePath().c_str());
        Sleep(200);
        ExitProcess(0);
    }
    else {
        httpSend(s, 404, "Not Found", "text/plain", "not found");
    }
    pclosesocket(s);
}

static DWORD WINAPI connProc(LPVOID arg)
{
    handleConn((SOCKET)arg);
    return 0;
}

static int serverLoop()
{
    for (;;) {
        SOCKET c = paccept((SOCKET)g_listenSock, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        DWORD tid = 0;
        HANDLE h = CreateThread(NULL, 0, connProc, (LPVOID)c, 0, &tid);
        if (h) CloseHandle(h);
        else pclosesocket(c);
    }
    return 0;
}

// ---------------------------------------------------- 浏览器与状态文件
static std::wstring makeUrl(int port, const std::string& tok)
{
    return L"http://127.0.0.1:" + toW(port) + L"/?t=" + std::wstring(tok.begin(), tok.end());
}

static void launchBrowser(const std::wstring& url)
{
    const wchar_t* cands[2] = {
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe"
    };
    for (int i = 0; i < 2; ++i) {
        if (!fileExists(cands[i])) continue;
        std::wstring cmd = L"\"" + std::wstring(cands[i]) + L"\" --app=\"" + url + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));
        if (CreateProcessW(NULL, &buf[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
    ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static void saveState()
{
    std::ostringstream o;
    o << g_guiPort << "\n" << g_token;
    std::string s = o.str();
    HANDLE h = CreateFileW(stateFilePath().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, s.data(), (DWORD)s.size(), &wr, NULL);
        CloseHandle(h);
    }
}

static bool readState(int& port, std::string& tok)
{
    std::string c = readWholeFile(stateFilePath());
    if (c.empty()) return false;
    size_t nl = c.find('\n');
    if (nl == std::string::npos) return false;
    port = atoi(c.c_str());
    tok = c.substr(nl + 1);
    while (!tok.empty() && (tok[tok.size() - 1] == '\r' || tok[tok.size() - 1] == '\n' || tok[tok.size() - 1] == ' '))
        tok.erase(tok.size() - 1);
    return port > 0 && port < 65536 && tok.size() == 16;
}

static bool pingServer(int port, const std::string& tok)
{
    SOCKET s = psocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD tv = 800;
    psetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    psetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    sockaddr_in sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = phtons((u_short)port);
    sa.sin_addr.s_addr = pinet_addr("127.0.0.1");
    bool ok = false;
    if (pconnect(s, (sockaddr*)&sa, sizeof(sa)) == 0) {
        std::string req = "GET /api/ping?t=" + tok + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (psend(s, req.data(), (int)req.size(), 0) > 0) {
            char buf[1024];
            int n = precv(s, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                if (strstr(buf, "\"ok\"")) ok = true;
            }
        }
    }
    pclosesocket(s);
    return ok;
}

static int fallbackToMenu(const std::wstring& why)
{
    if (AllocConsole()) SetConsoleOutputCP(CP_UTF8);
    outLn(why);
    outLn(L"已退回命令行菜单。");
    return interactiveMenu();
}

// ---------------------------------------------------- 图形界面入口
static int startGui(const std::wstring& dir)
{
    FreeConsole();                       // 双击运行不留黑窗
    g_exeDir = exeDirOf();
    g_guiDir = dir;

    if (!fileExists(g_exeDir + L"\\desk_sentry_gui.html"))
        return fallbackToMenu(L"未找到界面文件 desk_sentry_gui.html,请与 exe 放在同一目录。");
    if (!loadWinsock())
        return fallbackToMenu(L"网络组件加载失败(ws2_32.dll)。");

    WSADATA wsd;
    if (pWSAStartup(MAKEWORD(2, 2), &wsd) != 0)
        return fallbackToMenu(L"网络组件初始化失败。");

    // 已有服务器在跑?那就只拉一个窗口,不重复启动
    int oldPort = 0;
    std::string oldTok;
    if (readState(oldPort, oldTok) && pingServer(oldPort, oldTok)) {
        launchBrowser(makeUrl(oldPort, oldTok));
        return 0;
    }

    // 生成随机令牌
    srand((unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId());
    {
        char tok[17];
        for (int i = 0; i < 16; ++i) tok[i] = "0123456789abcdef"[rand() % 16];
        tok[16] = 0;
        g_token = tok;
    }

    // 8787 起挑一个能绑定 127.0.0.1 的端口
    SOCKET ls = INVALID_SOCKET;
    for (int port = 8787; port <= 8806; ++port) {
        SOCKET s = psocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) break;
        int one = 1;
        psetsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in sa;
        ZeroMemory(&sa, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = phtons((u_short)port);
        sa.sin_addr.s_addr = pinet_addr("127.0.0.1");
        if (pbind(s, (sockaddr*)&sa, sizeof(sa)) == 0 && plisten(s, 8) == 0) {
            ls = s;
            g_guiPort = port;
            break;
        }
        pclosesocket(s);
    }
    if (ls == INVALID_SOCKET)
        return fallbackToMenu(L"本地端口 8787-8806 全被占用,无法启动界面服务。");

    g_listenSock = (int)ls;
    saveState();
    launchBrowser(makeUrl(g_guiPort, g_token));
    outLn(L"图形界面已启动: http://127.0.0.1:" + toW(g_guiPort) + L"  (Ctrl+C 或网页里点退出可关闭)");
    return serverLoop();
}

// ---------------------------------------------------------------- 交互菜单
// 控制台用 ReadConsoleW(中文路径不乱码),重定向输入退回宽字符流
static bool readLine(std::wstring& line)
{
    line.clear();
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h != NULL && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        wchar_t buf[1024];
        DWORD got = 0;
        if (!ReadConsoleW(h, buf, 1023, &got, NULL)) return false;
        while (got > 0 && (buf[got - 1] == L'\r' || buf[got - 1] == L'\n')) --got;
        line.assign(buf, got);
        return true;
    }
    std::wstring s;
    if (!std::getline(std::wcin, s)) return false;
    line = s;
    return true;
}

static void showMenu(const std::wstring& dir)
{
    outLn(L"");
    outLn(L"====================================================");
    outLn(L"  桌面哨兵  DeskSentry  v1.5");
    outLn(L"====================================================");
    outLn(L"  当前目录: " + dir);
    outLn(L"");
    outLn(L"  [1] 预览(只看不动)");
    outLn(L"  [2] 执行整理(文件进文件夹,快捷方式平铺)");
    outLn(L"  [3] 撤销上一次整理");
    outLn(L"  [4] 查看分类规则");
    outLn(L"  [5] 自动值守(新图标冒出来就自动归类)");
    outLn(L"  [6] 更换目录");
    outLn(L"  [0] 退出");
    outLn(L"----------------------------------------------------");
    out(L"请选择: ");
}

static int interactiveMenu()
{
    std::wstring dir = desktopDir();
    std::wstring line;
    for (;;) {
        showMenu(dir);
        if (!readLine(line)) { outLn(L""); return 0; }      // 没有输入流(被重定向为空)
        if (line.empty()) continue;
        wchar_t c = line[0];
        if (c == L'0') { outLn(L"再见。"); return 0; }
        else if (c == L'1') run(MODE_PREVIEW, dir);
        else if (c == L'2') run(MODE_APPLY, dir);
        else if (c == L'3') doUndo();
        else if (c == L'4') showRules();
        else if (c == L'5') watch(dir, 3);
        else if (c == L'6') {
            out(L"请输入目录路径: ");
            std::wstring p;
            if (readLine(p) && !p.empty()) {
                if (dirExists(p)) { dir = p; outLn(L"已切换到: " + dir); }
                else outLn(L"目录不存在: " + p);
            }
        }
        else outLn(L"无效的选择,请输入 0-6。");
    }
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    SetConsoleOutputCP(CP_UTF8);

    // 用宽字符命令行解析,--dir 后面跟中文路径也不会出错
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

    Mode mode = MODE_PREVIEW;
    std::wstring dir = desktopDir();
    bool hasAction = false;
    bool wantGui = false, wantCli = false;
    int watchSec = 0;

    for (int i = 1; i < wargc; ++i) {
        std::wstring a = lowerAscii(wargv[i]);
        if (a == L"--apply") { mode = MODE_APPLY; hasAction = true; }
        else if (a == L"--undo") { mode = MODE_UNDO; hasAction = true; }
        else if (a == L"--folders") { g_forceFolders = true; }   // 模式开关,需与 --apply/--watch 组合
        else if (a == L"--rules") { mode = MODE_RULES; hasAction = true; }
        else if (a == L"--gui") { wantGui = true; hasAction = true; }
        else if (a == L"--cli") { wantCli = true; hasAction = true; }
        else if (a == L"--watch") {
            hasAction = true;
            watchSec = 3;
            if (i + 1 < wargc) {
                wchar_t* end = 0;
                long v = wcstol(wargv[i + 1], &end, 10);
                if (end && *end == 0 && v > 0) { watchSec = (int)v; ++i; }
            }
        }
        else if (a == L"--help" || a == L"-h" || a == L"/?") {
            if (wargv) LocalFree(wargv);
            printHelp();
            return 0;
        }
        else if (a == L"--dir") {
            if (i + 1 >= wargc) {
                if (wargv) LocalFree(wargv);
                outLn(L"--dir 后面需要跟目录路径");
                return 1;
            }
            dir = wargv[++i];
            hasAction = true;      // 指定了目录就视为"要干活",不再进菜单
        }
        else {
            outLn(L"未知参数: " + std::wstring(wargv[i]));
            printHelp();
            if (wargv) LocalFree(wargv);
            return 1;
        }
    }
    if (wargv) LocalFree(wargv);

    // 没带参数(双击 exe)→ 图形界面;--cli → 命令行菜单
    if (!hasAction) {
        if (g_forceFolders) {
            outLn(L"--folders 是模式开关,需与 --apply 或 --watch 组合,例如: desk_sentry --apply --folders"_w);
            return 0;
        }
        return startGui(dir);
    }

    if (watchSec > 0) return watch(dir, watchSec);
    if (wantGui) return startGui(dir);
    if (wantCli) return interactiveMenu();
    if (mode == MODE_RULES) { showRules(); return 0; }
    if (mode == MODE_UNDO) return doUndo();
    return run(mode, dir);
}
