// ============================================================================
//  桌面哨兵 DeskSentry (desk_sentry.cpp)
//  —— 桌面图标自动分类整理工具(Windows / 小熊猫C++ 可直接编译)
//
//  用途:把桌面上散落的快捷方式按类别自动归入文件夹,保持桌面整洁;
//        值守模式下,新冒出来的图标会在几秒内被自动收走。
//  分类:学习类 / 游戏类 / 影音类 / 通讯类 / 工具类
//
//  用法:
//      desk_sentry                  打开交互菜单(双击 exe 即可使用)
//      desk_sentry --apply          执行移动,并写入日志
//      desk_sentry --undo           撤销上一次 --apply 的全部移动
//      desk_sentry --rules          查看分类规则表
//      desk_sentry --watch [秒]     值守模式:新图标出现即自动归类
//      desk_sentry --dir "路径"     整理指定目录(默认是桌面)
//
//  设计要点:
//    1. 全程宽字符(W 系列 API),中文路径、中文文件名不乱码;
//    2. 所有中文数据表用宽字面量 L"..." 存放,直接编译成 UTF-16 码点,
//       不受源文件编码和 -fexec-charset 影响(GBK/UTF-8 编译结果一致);
//    3. 规则表有优先级,先匹配先生效;"此电脑/回收站/Edge" 等系统入口强制保留;
//    4. 目标重名自动加 " (1)";每次移动都写日志,支持一键撤销;
//    5. 默认只读预览,加 --apply 或菜单选 2 才动手 —— 误操作零风险。
// ============================================================================

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <sstream>
#include <cstring>
#include <cstdlib>
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

static void out(const std::wstring& s)
{
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

// 报告时用的分类顺序
static const wchar_t* const kCategoryOrder[] = {
    L"学习类", L"游戏类", L"影音类", L"通讯类", L"工具类"
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

// 参与整理的扩展名
static const wchar_t* const kExtensions[] = {
    L".lnk", L".url", L".exe", L".java", L".c", L".cpp", L".h", L".hpp", L".py", L".bat", L".cmd"
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

static bool isSortableObject(const std::wstring& name)
{
    std::wstring ext = extensionOf(name);
    const size_t n = sizeof(kExtensions) / sizeof(kExtensions[0]);
    for (size_t i = 0; i < n; ++i) {
        if (ext == kExtensions[i]) return true;
    }
    return false;
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

static std::wstring classify(const std::wstring& name, const std::wstring& fullPath,
                             const std::vector<Rule>& rules)
{
    for (size_t i = 0; i < rules.size(); ++i) {
        if (containsNoCase(name, rules[i].key)) return rules[i].cat;
    }
    if (extensionOf(name) == L".url" && isSteamUrl(fullPath)) return L"游戏类";
    return L"";
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

static bool readLog(std::vector<MoveRecord>& records)
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
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        MoveRecord rec;
        rec.src = line.substr(0, tab);
        rec.dst = line.substr(tab + 1);
        records.push_back(rec);
    }
    return !records.empty();
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

// ---------------------------------------------------------------- 模式
enum Mode { MODE_PREVIEW, MODE_APPLY, MODE_UNDO, MODE_RULES };

static void printHelp()
{
    outLn(L"桌面哨兵 DeskSentry —— 桌面图标自动分类整理工具"_w);
    outLn(L""_w);
    outLn(L"  不带任何参数运行:打开交互菜单(双击 exe 即可使用)"_w);
    outLn(L""_w);
    outLn(L"  --apply            执行移动,并写入日志"_w);
    outLn(L"  --undo             撤销上一次整理"_w);
    outLn(L"  --rules            查看分类规则表"_w);
    outLn(L"  --watch [秒]       值守模式:新图标出现即自动归类(默认 3 秒一查)"_w);
    outLn(L"  --dir 路径         整理指定目录(默认是桌面)"_w);
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
    outLn(L"  兜底:Steam 协议的 .url 快捷方式一律归入游戏类"_w);
    outLn(L"  保留:此电脑 / 回收站 / Microsoft Edge 不参与整理"_w);
}

static int doUndo()
{
    std::vector<MoveRecord> records;
    if (!readLog(records)) {
        outLn(L"没有找到可撤销的记录(日志: " + logPath() + L")"_w);
        return 1;
    }
    int ok = 0, fail = 0;
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
        } else {
            ++fail;
            outLn(L"  失败: " + dst);
        }
    }
    if (fail == 0) {
        DeleteFileW(logPath().c_str());
        // 顺手清掉撤销后变空的分类文件夹(记录里的 src 就是目录根)
        int removed = 0;
        size_t pos = records[0].src.find_last_of(L"\\/");
        std::wstring root = (pos == std::wstring::npos) ? L"." : records[0].src.substr(0, pos);
        size_t ncat = sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]);
        for (size_t c = 0; c < ncat; ++c) {
            std::wstring dir = root + L"\\" + kCategoryOrder[c];
            if (dirExists(dir) && isDirEmpty(dir) && RemoveDirectoryW(dir.c_str())) ++removed;
        }
        outLn(L"撤销完成:还原 " + toW(ok) + L" 项,日志已清除。"_w);
        if (removed > 0) outLn(L"清理了 " + toW(removed) + L" 个空文件夹。"_w);
    } else {
        outLn(L"撤销完成:还原 " + toW(ok) + L" 项,失败 " + toW(fail) + L" 项。"_w);
    }
    return fail == 0 ? 0 : 1;
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

    std::vector<std::wstring> todo;          // 归类成功的文件名
    std::vector<std::wstring> unclassified;  // 没匹配上规则的
    std::vector<std::wstring> kept;          // 保留的系统入口
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].keep) kept.push_back(items[i].name);
        else if (items[i].cat.empty()) unclassified.push_back(items[i].name);
        else todo.push_back(items[i].name);
    }

    outLn(L"目录: " + dir);
    outLn(mode == MODE_APPLY ? L"模式: 执行(--apply)"_w : L"模式: 预览,未改动任何文件(加 --apply 执行)"_w);
    outLn(L""_w);

    // 按分类打印
    const size_t ncat = sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]);
    size_t total = 0;
    for (size_t c = 0; c < ncat; ++c) {
        std::wstring cat = kCategoryOrder[c];
        std::vector<std::wstring> group;
        for (size_t i = 0; i < todo.size(); ++i) {
            if (classify(todo[i], joinPath(dir, todo[i]), rules) == cat) group.push_back(todo[i]);
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

    if (mode == MODE_PREVIEW) {
        outLn(L"合计 " + toW(total) + L" 项待整理。"_w);
        if (total > 0) outLn(L"确认无误后执行: desk_sentry --apply"_w);
        return 0;
    }

    // ---- 执行 ----
    int ok = 0, fail = 0;
    std::vector<MoveRecord> records;
    for (size_t c = 0; c < ncat; ++c) {
        std::wstring cat = kCategoryOrder[c];
        std::wstring target = joinPath(dir, cat);
        for (size_t i = 0; i < todo.size(); ++i) {
            if (classify(todo[i], joinPath(dir, todo[i]), rules) != cat) continue;
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

    if (!records.empty() && !appendLog(records)) {
        outLn(L"警告:日志写入失败,--undo 可能不可用。"_w);
    }
    outLn(L""_w);
    outLn(L"完成:移动 " + toW(ok) + L" 项,失败 " + toW(fail) + L" 项。"_w);
    if (!records.empty()) {
        outLn(L"日志: " + logPath());
        outLn(L"如需回滚,运行: desk_sentry --undo"_w);
    }
    return fail == 0 ? 0 : 1;
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

// 反复扫描目录,新出现的图标立刻归类 —— 专治抖音、WeGame 这类会自己重建快捷方式的程序
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

    outLn(L"自动值守已启动,每 " + toW(intervalSec) + L" 秒扫描一次。");
    outLn(L"桌面再冒出新图标会自动归入对应文件夹,按 Ctrl+C 结束。");
    outLn(L"");

    int moved = 0;
    for (;;) {
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
        // 分片睡眠,便于及时响应 Ctrl+C
        for (int k = 0; k < intervalSec * 5 && !gStopWatch; ++k) Sleep(200);
        if (gStopWatch) break;
    }
    SetConsoleCtrlHandler(onCtrlEvent, FALSE);
    outLn(L""_w);
    outLn(L"值守结束:本次共自动归类 " + toW(moved) + L" 项。");
    return 0;
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
    outLn(L"  桌面哨兵  DeskSentry  v1.1");
    outLn(L"====================================================");
    outLn(L"  当前目录: " + dir);
    outLn(L"");
    outLn(L"  [1] 预览(只看不动)");
    outLn(L"  [2] 执行整理");
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
    int watchSec = 0;

    for (int i = 1; i < wargc; ++i) {
        std::wstring a = lowerAscii(wargv[i]);
        if (a == L"--apply") { mode = MODE_APPLY; hasAction = true; }
        else if (a == L"--undo") { mode = MODE_UNDO; hasAction = true; }
        else if (a == L"--rules") { mode = MODE_RULES; hasAction = true; }
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

    // 没带参数(双击 exe 或在小熊猫里直接运行)→ 交互菜单,不再一闪而过
    if (!hasAction) return interactiveMenu();

    if (watchSec > 0) return watch(dir, watchSec);
    if (mode == MODE_RULES) { showRules(); return 0; }
    if (mode == MODE_UNDO) return doUndo();
    return run(mode, dir);
}
