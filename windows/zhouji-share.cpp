#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <gdiplus.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Gdiplus;
using std::string;
using std::wstring;

enum class Category { Study, Course, Fitness, Work, Life };

struct Task {
    wstring title;
    Category category = Category::Study;
    int duration = 45;
    int day = 0;
    int startMinutes = 540;
    bool done = false;
};

static ULONG_PTR g_gdiplus = 0;
static HWND g_window = nullptr;
static HWND g_dialog = nullptr;
static HFONT g_uiFont = nullptr;
static std::vector<Task> g_tasks;
static std::vector<RECT> g_taskRects;
static RECT g_addRect{}, g_aiRect{}, g_settingsRect{};
static int g_scrollY = 0;
static int g_maxScroll = 0;
static float g_zoom = 1.0f;
static wstring g_dataDir;
static wstring g_endpoint;
static wstring g_model;
static wstring g_apiKey;
static wstring g_dayLabels[7];
static wstring g_weekRange;
static std::mutex g_aiMutex;
static std::vector<Task> g_aiTasks;
static wstring g_aiError;
static constexpr UINT WM_AI_RESULT = WM_APP + 21;

enum class DialogKind { Add, Settings, AI };
static DialogKind g_dialogKind = DialogKind::Add;

enum ControlId {
    ID_TITLE = 1001, ID_DAY, ID_TIME, ID_DURATION, ID_CATEGORY, ID_SAVE_ADD,
    ID_ENDPOINT = 1101, ID_MODEL, ID_KEY, ID_SAVE_SETTINGS,
    ID_GOALS = 1201, ID_GENERATE, ID_AI_STATUS, ID_CANCEL = 1999
};

static HWND hTitle{}, hDay{}, hTime{}, hDuration{}, hCategory{};
static HWND hEndpoint{}, hModel{}, hKey{};
static HWND hGoals{}, hGenerate{}, hAiStatus{};

static Color C(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Color(a, r, g, b); }

static string Utf8(const wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), size, nullptr, nullptr);
    return result;
}

static wstring Wide(const string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
    wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), size);
    return result;
}

static wstring Trim(wstring value) {
    const wchar_t* spaces = L" \t\r\n";
    size_t first = value.find_first_not_of(spaces);
    if (first == wstring::npos) return L"";
    size_t last = value.find_last_not_of(spaces);
    return value.substr(first, last - first + 1);
}

static wstring FolderPath(int csidl) {
    wchar_t value[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, value))) return L"";
    return value;
}

static wstring EnvironmentOverride(const wchar_t* name, const wstring& fallback) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return fallback;
    wstring value(needed, L'\0');
    GetEnvironmentVariableW(name, value.data(), needed);
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value.empty() ? fallback : value;
}

static bool HasArgument(const wchar_t* wanted) {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool found = false;
    if (args) {
        for (int i = 1; i < count; ++i) if (_wcsicmp(args[i], wanted) == 0) { found = true; break; }
        LocalFree(args);
    }
    return found;
}

static wstring ArgumentAfter(const wchar_t* wanted) {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    wstring value;
    if (args) {
        for (int i = 1; i + 1 < count; ++i) if (_wcsicmp(args[i], wanted) == 0) { value = args[i + 1]; break; }
        LocalFree(args);
    }
    return value;
}

static void RoundedPath(GraphicsPath& path, const RectF& r, REAL radius) {
    REAL d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

static void FillRounded(Graphics& g, const RectF& r, REAL radius, const Color& color) {
    GraphicsPath path; RoundedPath(path, r, radius);
    SolidBrush brush(color); g.FillPath(&brush, &path);
}

static void StrokeRounded(Graphics& g, const RectF& r, REAL radius, const Color& color, REAL width = 1.0f) {
    GraphicsPath path; RoundedPath(path, r, radius);
    Pen pen(color, width); g.DrawPath(&pen, &path);
}

static Font MakeFont(REAL size, INT style = FontStyleRegular) {
    FontFamily family(L"Microsoft YaHei UI");
    return Font(&family, size, style, UnitPixel);
}

static void Text(Graphics& g, const wchar_t* value, const RectF& box, REAL size, const Color& color,
                 INT style = FontStyleRegular, StringAlignment align = StringAlignmentNear,
                 StringAlignment lineAlign = StringAlignmentNear, bool wrap = false) {
    Font font = MakeFont(size, style); SolidBrush brush(color); StringFormat format;
    format.SetAlignment(align); format.SetLineAlignment(lineAlign); format.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) format.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(value, -1, &font, box, &format, &brush);
}

static Color Accent(Category category) {
    switch (category) {
        case Category::Study: return C(92, 126, 188);
        case Category::Course: return C(116, 129, 149);
        case Category::Fitness: return C(93, 151, 116);
        case Category::Work: return C(177, 132, 74);
        default: return C(139, 116, 157);
    }
}

static Color Card(Category category) {
    switch (category) {
        case Category::Study: return C(239, 244, 252);
        case Category::Course: return C(241, 243, 246);
        case Category::Fitness: return C(238, 246, 240);
        case Category::Work: return C(250, 244, 233);
        default: return C(247, 241, 248);
    }
}

static const wchar_t* CategoryText(Category category) {
    switch (category) {
        case Category::Study: return L"学习";
        case Category::Course: return L"课程";
        case Category::Fitness: return L"训练";
        case Category::Work: return L"工作";
        default: return L"生活";
    }
}

static string CleanField(const wstring& value) {
    wstring clean = value;
    for (wchar_t& ch : clean) if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
    return Utf8(clean);
}

static void SavePlan() {
    std::ofstream file(std::filesystem::path(g_dataDir) / L"plan.tsv", std::ios::binary | std::ios::trunc);
    for (const Task& task : g_tasks) {
        file << task.day << '\t' << task.startMinutes << '\t' << task.duration << '\t'
             << (int)task.category << '\t' << (task.done ? 1 : 0) << '\t' << CleanField(task.title) << "\n";
    }
}

static void LoadPlan() {
    g_tasks.clear();
    std::ifstream file(std::filesystem::path(g_dataDir) / L"plan.tsv", std::ios::binary);
    string line;
    while (std::getline(file, line)) {
        std::vector<string> fields; size_t start = 0;
        for (int i = 0; i < 5; ++i) {
            size_t tab = line.find('\t', start); if (tab == string::npos) break;
            fields.push_back(line.substr(start, tab - start)); start = tab + 1;
        }
        fields.push_back(line.substr(start));
        if (fields.size() != 6) continue;
        try {
            Task task; task.day = std::stoi(fields[0]); task.startMinutes = std::stoi(fields[1]);
            task.duration = std::stoi(fields[2]); task.category = (Category)std::stoi(fields[3]);
            task.done = std::stoi(fields[4]) != 0; task.title = Wide(fields[5]);
            if (task.day >= 0 && task.day < 7 && task.startMinutes >= 420 && task.startMinutes < 1440 && task.duration > 0 && !task.title.empty()) g_tasks.push_back(task);
        } catch (...) {}
    }
}

static void SaveText(const wchar_t* name, const wstring& value) {
    std::ofstream file(std::filesystem::path(g_dataDir) / name, std::ios::binary | std::ios::trunc);
    string data = Utf8(value); file.write(data.data(), data.size());
}

static wstring LoadText(const wchar_t* name) {
    std::ifstream file(std::filesystem::path(g_dataDir) / name, std::ios::binary);
    return Wide(string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
}

static void SaveSecret(const wstring& secret) {
    string plain = Utf8(secret);
    DATA_BLOB input{(DWORD)plain.size(), reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB output{};
    if (CryptProtectData(&input, L"Zhouji AI Key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        std::ofstream file(std::filesystem::path(g_dataDir) / L"key.dat", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
    }
}

static wstring LoadSecret() {
    std::ifstream file(std::filesystem::path(g_dataDir) / L"key.dat", std::ios::binary);
    string encrypted((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (encrypted.empty()) return L"";
    DATA_BLOB input{(DWORD)encrypted.size(), reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return L"";
    string plain(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return Wide(plain);
}

static void SaveSettings() { SaveText(L"endpoint.txt", g_endpoint); SaveText(L"model.txt", g_model); SaveSecret(g_apiKey); }
static void LoadSettings() { g_endpoint = Trim(LoadText(L"endpoint.txt")); g_model = Trim(LoadText(L"model.txt")); g_apiKey = LoadSecret(); }

static void ComputeWeek() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    FILETIME ft{}; SystemTimeToFileTime(&now, &ft);
    ULARGE_INTEGER value{}; value.LowPart = ft.dwLowDateTime; value.HighPart = ft.dwHighDateTime;
    int back = (now.wDayOfWeek + 6) % 7;
    value.QuadPart -= (ULONGLONG)back * 24 * 60 * 60 * 10000000ULL;
    SYSTEMTIME days[7]{};
    for (int i = 0; i < 7; ++i) {
        ULARGE_INTEGER day = value; day.QuadPart += (ULONGLONG)i * 24 * 60 * 60 * 10000000ULL;
        FILETIME dayFt{day.LowPart, day.HighPart}; FileTimeToSystemTime(&dayFt, &days[i]);
        wchar_t label[16]{}; swprintf(label, ARRAYSIZE(label), L"%02d.%02d", days[i].wMonth, days[i].wDay); g_dayLabels[i] = label;
    }
    wchar_t range[64]{};
    swprintf(range, ARRAYSIZE(range), L"%04d-%02d-%02d — %04d-%02d-%02d", days[0].wYear, days[0].wMonth, days[0].wDay, days[6].wYear, days[6].wMonth, days[6].wDay);
    g_weekRange = range;
}

static wstring TimeText(int minutes) {
    wchar_t buffer[16]{}; swprintf(buffer, ARRAYSIZE(buffer), L"%02d:%02d", minutes / 60, minutes % 60); return buffer;
}

static int DoneCount() { int count = 0; for (const Task& task : g_tasks) if (task.done) ++count; return count; }

static void RenderButton(Graphics& g, RECT& out, REAL x, REAL y, REAL width, const wchar_t* label, bool primary = false) {
    out = {(LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + 34)};
    FillRounded(g, RectF(x, y, width, 34), 9, primary ? C(76, 101, 142) : C(242, 241, 237));
    StrokeRounded(g, RectF(x, y, width, 34), 9, primary ? C(76, 101, 142) : C(218, 216, 210));
    Text(g, label, RectF(x, y, width, 34), 11.5f, primary ? C(255, 255, 255) : C(65, 78, 97), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
}

static void RenderDashboard(Graphics& g, int width, int height) {
    g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit); g.Clear(C(246, 244, 239));
    const int navW = 68, contentX = 94, timeW = 54, gridX = contentX + timeW, gridRight = width - 24;
    const int topH = 68, introTop = 88, infoTop = 174, dayTop = 244, dayH = 54, gridTop = dayTop + dayH, footerH = 42;
    const int viewportBottom = height - footerH, hourH = 60, contentHeight = 17 * hourH;
    const int viewportH = std::max(1, viewportBottom - gridTop);
    g_maxScroll = std::max(0, contentHeight - viewportH); g_scrollY = std::clamp(g_scrollY, 0, g_maxScroll);
    REAL dayW = (gridRight - gridX) / 7.0f;

    SolidBrush nav(C(57, 72, 95)); g.FillRectangle(&nav, 0, 0, navW, height);
    FillRounded(g, RectF(18, 18, 32, 32), 8, C(246, 244, 239));
    Text(g, L"周", RectF(18, 18, 32, 32), 17, C(57, 72, 95), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    SolidBrush dot(C(128, 157, 207)); g.FillEllipse(&dot, 30, 82, 8, 8);
    Pen navLine(C(112, 130, 157), 1); g.DrawLine(&navLine, 34, 106, 34, height - 38);

    SolidBrush top(C(251, 250, 247)); g.FillRectangle(&top, navW, 0, width - navW, topH);
    Pen hair(C(225, 222, 215), 1); g.DrawLine(&hair, navW, topH - 1, width, topH - 1);
    Text(g, L"周迹", RectF(contentX, 0, 90, topH), 19, C(41, 55, 76), FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
    Text(g, g_weekRange.c_str(), RectF(contentX + 86, 0, 330, topH), 12, C(118, 130, 148), FontStyleRegular, StringAlignmentNear, StringAlignmentCenter);
    REAL right = width - 24.0f;
    RenderButton(g, g_settingsRect, right - 94, 17, 94, L"AI 设置");
    RenderButton(g, g_aiRect, right - 202, 17, 98, L"AI 规划", true);
    RenderButton(g, g_addRect, right - 310, 17, 98, L"＋ 添加任务");

    Text(g, L"把这一周，落到每一天。", RectF(contentX, introTop, 500, 40), 27, C(40, 52, 70), FontStyleBold);
    Text(g, L"计划仅保存在本机。单击任务即可标记完成。", RectF(contentX, introTop + 44, 560, 24), 13, C(113, 124, 140));
    wchar_t summary[32]{}; swprintf(summary, ARRAYSIZE(summary), L"%d / %d", DoneCount(), (int)g_tasks.size());
    Text(g, summary, RectF(width - 210.0f, introTop - 1, 178, 38), 28, C(45, 61, 83), FontStyleRegular, StringAlignmentFar);
    Text(g, L"本周完成", RectF(width - 210.0f, introTop + 39, 178, 22), 12, C(119, 130, 146), FontStyleRegular, StringAlignmentFar);

    FillRounded(g, RectF(contentX, infoTop, width - contentX - 24.0f, 52), 10, C(235, 240, 247));
    if (g_tasks.empty()) {
        Text(g, L"还没有本周计划", RectF(contentX + 16, infoTop, 150, 52), 13, C(64, 86, 122), FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
        Text(g, L"点击“AI 规划”根据目标自动安排，或手动添加第一项任务。", RectF(contentX + 170, infoTop, width - contentX - 210.0f, 52), 12, C(91, 108, 135), FontStyleRegular, StringAlignmentNear, StringAlignmentCenter);
    } else {
        int study = 0, fitness = 0; for (const auto& t : g_tasks) { if (t.category == Category::Study || t.category == Category::Course) ++study; if (t.category == Category::Fitness) ++fitness; }
        wchar_t info[128]{}; swprintf(info, ARRAYSIZE(info), L"本周共 %d 项 · 学习/课程 %d 项 · 训练 %d 项", (int)g_tasks.size(), study, fitness);
        Text(g, L"本周计划", RectF(contentX + 16, infoTop, 100, 52), 13, C(64, 86, 122), FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
        Text(g, info, RectF(contentX + 116, infoTop, 460, 52), 12, C(91, 108, 135), FontStyleRegular, StringAlignmentNear, StringAlignmentCenter);
    }

    static const wchar_t* names[] = {L"周一",L"周二",L"周三",L"周四",L"周五",L"周六",L"周日"};
    Text(g, L"时间", RectF(contentX, dayTop, timeW - 8.0f, dayH), 11, C(124, 134, 148), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
    SYSTEMTIME now{}; GetLocalTime(&now); int today = (now.wDayOfWeek + 6) % 7;
    for (int day = 0; day < 7; ++day) {
        REAL x = gridX + day * dayW;
        if (day == today) FillRounded(g, RectF(x + 4, dayTop + 5.0f, dayW - 8, dayH - 10.0f), 9, C(232, 238, 247));
        Text(g, names[day], RectF(x, dayTop + 6.0f, dayW, 22), 13, day == today ? C(66, 91, 132) : C(49, 62, 81), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        Text(g, g_dayLabels[day].c_str(), RectF(x, dayTop + 27.0f, dayW, 18), 10, C(131, 141, 155), FontStyleRegular, StringAlignmentCenter, StringAlignmentCenter);
    }

    GraphicsState clipped = g.Save(); g.SetClip(Rect(contentX, gridTop, width - contentX - 20, viewportH));
    SolidBrush gridBg(C(251, 250, 247)); g.FillRectangle(&gridBg, contentX, gridTop, width - contentX - 24, viewportH);
    for (int day = 0; day < 7; ++day) {
        REAL x = gridX + day * dayW;
        if (day == today) { SolidBrush brush(C(245, 248, 252)); g.FillRectangle(&brush, x, (REAL)gridTop, dayW, (REAL)viewportH); }
        Pen v(C(225, 226, 225), 1); g.DrawLine(&v, x, (REAL)gridTop, x, (REAL)viewportBottom);
    }
    Pen lastV(C(225, 226, 225), 1); g.DrawLine(&lastV, gridRight, gridTop, gridRight, viewportBottom);
    for (int hour = 7; hour <= 24; ++hour) {
        REAL y = gridTop + (hour - 7) * hourH - g_scrollY; Pen line(C(227, 226, 222), 1); g.DrawLine(&line, (REAL)contentX, y, (REAL)gridRight, y);
        if (hour < 24) { wchar_t label[8]{}; swprintf(label, ARRAYSIZE(label), L"%02d:00", hour); Text(g, label, RectF(contentX, y + 4, timeW - 9.0f, 18), 10, C(137, 145, 156), FontStyleRegular, StringAlignmentFar); }
    }

    g_taskRects.assign(g_tasks.size(), RECT{});
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& task = g_tasks[i]; REAL x = gridX + task.day * dayW + 5;
        REAL y = gridTop + (task.startMinutes - 420) * (hourH / 60.0f) - g_scrollY + 2;
        REAL cardH = std::max(19.0f, task.duration * (hourH / 60.0f) - 4); REAL cardW = dayW - 10;
        g_taskRects[i] = {(LONG)x, (LONG)y, (LONG)(x + cardW), (LONG)(y + cardH)};
        if (y + cardH < gridTop || y > viewportBottom) continue;
        Color fill = Card(task.category), accent = Accent(task.category);
        if (task.done) { fill = C(245, 244, 241); accent = C(142, 151, 162); }
        FillRounded(g, RectF(x, y, cardW, cardH), 7, fill); SolidBrush accentBrush(accent);
        g.FillRectangle(&accentBrush, x, y + 5, 3.2f, std::max(8.0f, cardH - 10)); StrokeRounded(g, RectF(x, y, cardW, cardH), 7, C(222, 225, 228));
        REAL left = x + 10, rightPad = task.done ? 23.0f : 8.0f;
        if (cardH >= 42) {
            Text(g, task.title.c_str(), RectF(left, y + 7, cardW - 10 - rightPad, cardH - 27), 11.1f, task.done ? C(123, 132, 144) : C(49, 62, 81), FontStyleBold, StringAlignmentNear, StringAlignmentNear, true);
            wstring meta = TimeText(task.startMinutes) + L" · " + std::to_wstring(task.duration) + L" min";
            Text(g, meta.c_str(), RectF(left, y + cardH - 21, cardW - 18, 16), 9.2f, C(123, 133, 148));
        } else Text(g, task.title.c_str(), RectF(left, y, cardW - 10 - rightPad, cardH), 10.2f, task.done ? C(123, 132, 144) : C(49, 62, 81), FontStyleBold, StringAlignmentNear, StringAlignmentCenter);
        if (task.done) { SolidBrush checked(C(93, 151, 116)); g.FillEllipse(&checked, x + cardW - 20, y + 7, 13.0f, 13.0f); Text(g, L"✓", RectF(x + cardW - 21, y + 5, 15, 15), 9, C(255,255,255), FontStyleBold, StringAlignmentCenter, StringAlignmentCenter); }
    }
    g.Restore(clipped);

    SolidBrush footer(C(251, 250, 247)); g.FillRectangle(&footer, navW, height - footerH, width - navW, footerH); g.DrawLine(&hair, navW, height - footerH, width, height - footerH);
    REAL lx = contentX;
    for (Category cat : {Category::Study, Category::Course, Category::Fitness, Category::Work, Category::Life}) {
        Text(g, CategoryText(cat), RectF(lx, height - footerH, 52, footerH), 11, Accent(cat), FontStyleBold, StringAlignmentNear, StringAlignmentCenter); lx += 56;
    }
    Text(g, L"Ctrl + 滚轮缩放 · 数据仅保存在本机", RectF(width - 360.0f, height - footerH, 330, footerH), 11, C(121, 131, 145), FontStyleRegular, StringAlignmentFar, StringAlignmentCenter);
}

static HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, HWND parent, int id, DWORD ex = 0) {
    HWND control = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, (WPARAM)g_uiFont, TRUE); return control;
}

static wstring ControlText(HWND control) {
    int length = GetWindowTextLengthW(control); if (!length) return L""; wstring value(length + 1, L'\0'); GetWindowTextW(control, value.data(), length + 1); value.resize(length); return value;
}

static void Label(HWND parent, const wchar_t* text, int x, int y, int w, int h) { MakeControl(L"STATIC", text, 0, x, y, w, h, parent, 0); }

static void CloseDialogWindow(HWND dialog) { EnableWindow(g_window, TRUE); SetForegroundWindow(g_window); DestroyWindow(dialog); g_dialog = nullptr; }

static void PopulateCombo(HWND combo, const std::vector<wstring>& values) {
    for (const auto& value : values) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)value.c_str()); SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

static int ParseTime(const wstring& value) {
    int h = -1, m = -1; if (swscanf(value.c_str(), L"%d:%d", &h, &m) != 2) return -1;
    if (h < 7 || h > 23 || m < 0 || m > 59) return -1; return h * 60 + m;
}

static void CenterDialog(HWND window, int width, int height) {
    RECT owner{}; GetWindowRect(g_window, &owner); int x = owner.left + (owner.right - owner.left - width) / 2; int y = owner.top + (owner.bottom - owner.top - height) / 2;
    SetWindowPos(window, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
}

static string JsonEscape(const string& value) {
    string out; out.reserve(value.size() + 32);
    for (unsigned char ch : value) {
        if (ch == '"') out += "\\\""; else if (ch == '\\') out += "\\\\"; else if (ch == '\n') out += "\\n"; else if (ch == '\r') out += "\\r"; else if (ch == '\t') out += "\\t"; else if (ch < 0x20) out += ' '; else out += (char)ch;
    }
    return out;
}

static void AppendUtf8Codepoint(string& out, unsigned value) {
    if (value <= 0x7F) out.push_back((char)value);
    else if (value <= 0x7FF) { out.push_back((char)(0xC0 | value >> 6)); out.push_back((char)(0x80 | value & 0x3F)); }
    else { out.push_back((char)(0xE0 | value >> 12)); out.push_back((char)(0x80 | (value >> 6) & 0x3F)); out.push_back((char)(0x80 | value & 0x3F)); }
}

static bool ExtractContent(const string& json, wstring& content) {
    size_t key = json.find("\"content\""); if (key == string::npos) return false;
    size_t colon = json.find(':', key + 9); if (colon == string::npos) return false;
    size_t quote = json.find('"', colon + 1); if (quote == string::npos) return false;
    string out;
    for (size_t i = quote + 1; i < json.size(); ++i) {
        char ch = json[i]; if (ch == '"') { content = Wide(out); return true; }
        if (ch != '\\') { out.push_back(ch); continue; }
        if (++i >= json.size()) break; char e = json[i];
        if (e == 'n') out.push_back('\n'); else if (e == 'r') out.push_back('\r'); else if (e == 't') out.push_back('\t'); else if (e == '"') out.push_back('"'); else if (e == '\\') out.push_back('\\');
        else if (e == 'u' && i + 4 < json.size()) { unsigned value = 0; for (int k = 0; k < 4; ++k) { char h = json[++i]; value = value * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h - 'A' + 10); } AppendUtf8Codepoint(out, value); }
        else out.push_back(e);
    }
    return false;
}

static bool HttpAiRequest(const wstring& endpoint, const wstring& model, const wstring& key, const wstring& goals, wstring& response, wstring& error) {
    URL_COMPONENTS parts{sizeof(parts)}; parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(endpoint.c_str(), 0, 0, &parts)) { error = L"API 地址格式不正确。"; return false; }
    wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET session = WinHttpOpen(L"ZhoujiPlanner/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = L"无法初始化网络连接。"; return false; }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
    if (!request) { if (connect) WinHttpCloseHandle(connect); WinHttpCloseHandle(session); error = L"无法连接到 AI 服务。"; return false; }

    const wstring system = L"你是周计划助手。只输出 TSV 文本，每行五列：星期数字(1-7)\\t开始时间(HH:MM)\\t时长分钟\\t类别\\t任务。类别只能是学习、课程、训练、工作、生活。安排 07:00-23:30，不超过35项，不要代码块，不要解释，不要推测用户身份或添加个人信息。";
    string body = "{\"model\":\"" + JsonEscape(Utf8(model)) + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + JsonEscape(Utf8(system)) + "\"},{\"role\":\"user\",\"content\":\"" + JsonEscape(Utf8(goals)) + "\"}],\"temperature\":0.3}";
    wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!key.empty()) headers += L"Authorization: Bearer " + key + L"\r\n";
    BOOL sent = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L, body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, statusSize = sizeof(status); WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &statusSize, nullptr);
    string json; if (sent) { DWORD available = 0; while (WinHttpQueryDataAvailable(request, &available) && available) { size_t old = json.size(); json.resize(old + available); DWORD read = 0; if (!WinHttpReadData(request, json.data() + old, available, &read)) break; json.resize(old + read); } }
    WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    if (!sent) { error = L"AI 请求失败，请检查网络和接口地址。"; return false; }
    if (status < 200 || status >= 300) { error = L"AI 服务返回错误，HTTP " + std::to_wstring(status) + L"。"; return false; }
    if (!ExtractContent(json, response)) { error = L"无法读取 AI 返回内容，请确认接口兼容 Chat Completions。"; return false; }
    return true;
}

static Category ParseCategory(const wstring& text) {
    if (text.find(L"课程") != wstring::npos || text.find(L"course") != wstring::npos) return Category::Course;
    if (text.find(L"训练") != wstring::npos || text.find(L"健身") != wstring::npos || text.find(L"fitness") != wstring::npos) return Category::Fitness;
    if (text.find(L"工作") != wstring::npos || text.find(L"work") != wstring::npos) return Category::Work;
    if (text.find(L"生活") != wstring::npos || text.find(L"life") != wstring::npos) return Category::Life;
    return Category::Study;
}

static bool ParseAiPlan(const wstring& text, std::vector<Task>& tasks, wstring& error) {
    std::wstringstream stream(text); wstring line; tasks.clear();
    while (std::getline(stream, line) && tasks.size() < 50) {
        line = Trim(line); if (line.empty() || line.rfind(L"```", 0) == 0) continue;
        wchar_t delimiter = line.find(L'\t') != wstring::npos ? L'\t' : L'|';
        std::vector<wstring> fields; size_t start = 0;
        for (int i = 0; i < 4; ++i) { size_t pos = line.find(delimiter, start); if (pos == wstring::npos) break; fields.push_back(Trim(line.substr(start, pos - start))); start = pos + 1; }
        fields.push_back(Trim(line.substr(start))); if (fields.size() != 5) continue;
        int day = 0; try { day = std::stoi(fields[0]); } catch (...) { static const wchar_t* names[] = {L"周一",L"周二",L"周三",L"周四",L"周五",L"周六",L"周日"}; for (int i=0;i<7;++i) if (fields[0].find(names[i]) != wstring::npos) day=i+1; }
        int time = ParseTime(fields[1]), duration = 0; try { duration = std::stoi(fields[2]); } catch (...) {}
        wstring title = fields[4]; for (wchar_t& ch : title) if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
        if (day < 1 || day > 7 || time < 0 || duration < 10 || duration > 600 || title.empty()) continue;
        if (title.size() > 80) title.resize(80);
        tasks.push_back({title, ParseCategory(fields[3]), duration, day - 1, time, false});
    }
    std::sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b){ return a.day == b.day ? a.startMinutes < b.startMinutes : a.day < b.day; });
    if (tasks.empty()) { error = L"AI 没有返回可识别的计划，请调整目标后重试。"; return false; }
    return true;
}

static void BeginAiGeneration(const wstring& goals) {
    wstring endpoint = g_endpoint, model = g_model, key = g_apiKey;
    std::thread([endpoint, model, key, goals]() {
        wstring content, error; std::vector<Task> tasks;
        bool ok = HttpAiRequest(endpoint, model, key, goals, content, error) && ParseAiPlan(content, tasks, error);
        { std::lock_guard<std::mutex> lock(g_aiMutex); g_aiTasks = std::move(tasks); g_aiError = error; }
        if (IsWindow(g_window)) PostMessageW(g_window, WM_AI_RESULT, ok ? 1 : 0, 0);
    }).detach();
}

static LRESULT CALLBACK DialogProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CREATE) {
        g_dialogKind = (DialogKind)(INT_PTR)((CREATESTRUCTW*)lp)->lpCreateParams;
        if (g_dialogKind == DialogKind::Add) {
            Label(window,L"任务名称",28,28,110,24); hTitle=MakeControl(L"EDIT",L"",ES_AUTOHSCROLL,28,54,440,32,window,ID_TITLE,WS_EX_CLIENTEDGE);
            Label(window,L"星期",28,105,80,24); hDay=MakeControl(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,28,131,130,180,window,ID_DAY); PopulateCombo(hDay,{L"周一",L"周二",L"周三",L"周四",L"周五",L"周六",L"周日"});
            Label(window,L"开始时间",180,105,100,24); hTime=MakeControl(L"EDIT",L"09:00",ES_AUTOHSCROLL,180,131,120,32,window,ID_TIME,WS_EX_CLIENTEDGE);
            Label(window,L"时长（分钟）",322,105,120,24); hDuration=MakeControl(L"EDIT",L"45",ES_NUMBER,322,131,146,32,window,ID_DURATION,WS_EX_CLIENTEDGE);
            Label(window,L"类别",28,185,80,24); hCategory=MakeControl(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,28,211,200,180,window,ID_CATEGORY); PopulateCombo(hCategory,{L"学习",L"课程",L"训练",L"工作",L"生活"});
            MakeControl(L"BUTTON",L"取消",BS_PUSHBUTTON,270,292,90,34,window,ID_CANCEL); MakeControl(L"BUTTON",L"添加",BS_DEFPUSHBUTTON,378,292,90,34,window,ID_SAVE_ADD);
            CenterDialog(window,520,390); SetFocus(hTitle);
        } else if (g_dialogKind == DialogKind::Settings) {
            Label(window,L"AI 接口地址",28,24,160,24); hEndpoint=MakeControl(L"EDIT",g_endpoint.c_str(),ES_AUTOHSCROLL,28,50,544,32,window,ID_ENDPOINT,WS_EX_CLIENTEDGE);
            Label(window,L"示例：https://api.example.com/v1/chat/completions（需兼容 Chat Completions）",28,86,544,22);
            Label(window,L"模型名称",28,124,160,24); hModel=MakeControl(L"EDIT",g_model.c_str(),ES_AUTOHSCROLL,28,150,544,32,window,ID_MODEL,WS_EX_CLIENTEDGE);
            Label(window,L"API Key（仅在本机加密保存）",28,202,280,24); hKey=MakeControl(L"EDIT",g_apiKey.c_str(),ES_PASSWORD|ES_AUTOHSCROLL,28,228,544,32,window,ID_KEY,WS_EX_CLIENTEDGE);
            Label(window,L"适用于提供 OpenAI 兼容接口的 AI 服务；密钥不会写入安装包。",28,270,544,22);
            MakeControl(L"BUTTON",L"取消",BS_PUSHBUTTON,374,312,90,34,window,ID_CANCEL); MakeControl(L"BUTTON",L"保存",BS_DEFPUSHBUTTON,482,312,90,34,window,ID_SAVE_SETTINGS);
            CenterDialog(window,620,410);
        } else {
            Label(window,L"写下这周的目标、固定时间和偏好",28,24,430,24);
            hGoals=MakeControl(L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL,28,52,554,220,window,ID_GOALS,WS_EX_CLIENTEDGE);
            hAiStatus=MakeControl(L"STATIC",L"AI 将生成并替换当前周计划；不会读取电脑上的其他内容。",0,28,284,554,28,window,ID_AI_STATUS);
            MakeControl(L"BUTTON",L"取消",BS_PUSHBUTTON,380,330,94,34,window,ID_CANCEL); hGenerate=MakeControl(L"BUTTON",L"生成计划",BS_DEFPUSHBUTTON,488,330,94,34,window,ID_GENERATE);
            CenterDialog(window,630,430); SetFocus(hGoals);
        }
        return 0;
    }
    if (message == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == ID_CANCEL) { CloseDialogWindow(window); return 0; }
        if (id == ID_SAVE_ADD) {
            wstring title=Trim(ControlText(hTitle)); int day=(int)SendMessageW(hDay,CB_GETCURSEL,0,0), time=ParseTime(Trim(ControlText(hTime))), duration=0; try{duration=std::stoi(ControlText(hDuration));}catch(...){}
            int category=(int)SendMessageW(hCategory,CB_GETCURSEL,0,0);
            if(title.empty()||day<0||time<0||duration<10||duration>600){MessageBoxW(window,L"请填写有效的任务、时间和时长。",L"周迹",MB_OK|MB_ICONWARNING);return 0;}
            g_tasks.push_back({title,(Category)category,duration,day,time,false}); std::sort(g_tasks.begin(),g_tasks.end(),[](const Task&a,const Task&b){return a.day==b.day?a.startMinutes<b.startMinutes:a.day<b.day;}); SavePlan(); CloseDialogWindow(window); InvalidateRect(g_window,nullptr,FALSE); return 0;
        }
        if (id == ID_SAVE_SETTINGS) {
            g_endpoint=Trim(ControlText(hEndpoint)); g_model=Trim(ControlText(hModel)); g_apiKey=ControlText(hKey);
            if(!g_endpoint.empty() && g_endpoint.rfind(L"https://",0)!=0 && g_endpoint.rfind(L"http://",0)!=0){MessageBoxW(window,L"接口地址需要以 https:// 或 http:// 开头。",L"周迹",MB_OK|MB_ICONWARNING);return 0;}
            SaveSettings(); CloseDialogWindow(window); return 0;
        }
        if (id == ID_GENERATE) {
            wstring goals=Trim(ControlText(hGoals));
            if(g_endpoint.empty()||g_model.empty()){MessageBoxW(window,L"请先完成 AI 设置。",L"周迹",MB_OK|MB_ICONINFORMATION);return 0;}
            if(goals.empty()){MessageBoxW(window,L"请写下本周目标。",L"周迹",MB_OK|MB_ICONINFORMATION);return 0;}
            EnableWindow(hGenerate,FALSE); SetWindowTextW(hAiStatus,L"正在生成计划，请稍候……"); BeginAiGeneration(goals); return 0;
        }
    }
    if (message == WM_CLOSE) { CloseDialogWindow(window); return 0; }
    return DefWindowProcW(window,message,wp,lp);
}

static void OpenDialog(DialogKind kind) {
    if (g_dialog) { SetForegroundWindow(g_dialog); return; }
    EnableWindow(g_window,FALSE);
    const wchar_t* title = kind==DialogKind::Add?L"添加任务":kind==DialogKind::Settings?L"AI 设置":L"AI 规划本周";
    g_dialog = CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,L"ZhoujiShareDialog",title,WS_CAPTION|WS_SYSMENU|WS_POPUP,0,0,600,400,g_window,nullptr,GetModuleHandleW(nullptr),(void*)(INT_PTR)kind);
    ShowWindow(g_dialog,SW_SHOW); UpdateWindow(g_dialog);
}

static int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT count=0,size=0; GetImageEncodersSize(&count,&size); if(!size)return-1; std::vector<BYTE> buffer(size); ImageCodecInfo* info=(ImageCodecInfo*)buffer.data(); GetImageEncoders(count,size,info);
    for(UINT i=0;i<count;++i)if(wcscmp(info[i].MimeType,format)==0){*clsid=info[i].Clsid;return(int)i;}return-1;
}

static bool SavePreview(const wstring& path) { Bitmap bitmap(1440,900,PixelFormat32bppARGB); Graphics graphics(&bitmap); RenderDashboard(graphics,1440,900); CLSID png{}; return GetEncoderClsid(L"image/png",&png)>=0 && bitmap.Save(path.c_str(),&png,nullptr)==Ok; }

static bool CreateShortcut(const wstring& path,const wstring& target,const wstring& dir) {
    IShellLinkW* link=nullptr; HRESULT hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&link); if(FAILED(hr)||!link)return false;
    link->SetPath(target.c_str()); link->SetWorkingDirectory(dir.c_str()); link->SetDescription(L"周迹通用版 · AI 周计划"); link->SetIconLocation(target.c_str(),0);
    IPersistFile* persist=nullptr; hr=link->QueryInterface(IID_IPersistFile,(void**)&persist); bool ok=false; if(SUCCEEDED(hr)&&persist){ok=SUCCEEDED(persist->Save(path.c_str(),TRUE));persist->Release();} link->Release(); return ok;
}

static int InstallAndLaunch(bool verifyOnly) {
    wstring local=EnvironmentOverride(L"ZHOUJI_SHARE_TEST_LOCALAPPDATA",FolderPath(CSIDL_LOCAL_APPDATA)); wstring desktop=EnvironmentOverride(L"ZHOUJI_SHARE_TEST_DESKTOP",FolderPath(CSIDL_DESKTOPDIRECTORY)); if(local.empty()||desktop.empty())return 10;
    wstring dir=local+L"\\ZhoujiPlanner"; std::error_code ec; std::filesystem::create_directories(dir,ec); if(ec)return 11;
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr,self,ARRAYSIZE(self)); wstring installed=dir+L"\\周迹通用版.exe";
    HWND existing=FindWindowW(L"ZhoujiShareWindowV1",nullptr); if(existing){PostMessageW(existing,WM_CLOSE,0,0);for(int i=0;i<30&&IsWindow(existing);++i)Sleep(100);}
    if(!CopyFileW(self,installed.c_str(),FALSE))return 12;
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED); bool shortcut=CreateShortcut(desktop+L"\\周迹通用版.lnk",installed,dir); if(SUCCEEDED(com))CoUninitialize(); if(!shortcut)return 13;
    if(verifyOnly)return 0; HINSTANCE launched=ShellExecuteW(nullptr,L"open",installed.c_str(),nullptr,dir.c_str(),SW_SHOWNORMAL); return (INT_PTR)launched>32?0:14;
}

static bool InRect(const RECT& r, POINT p) { return PtInRect(&r,p)!=FALSE; }

static LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    switch(message){
        case WM_ERASEBKGND:return 1;
        case WM_GETMINMAXINFO:{auto*info=(MINMAXINFO*)lp;info->ptMinTrackSize.x=1040;info->ptMinTrackSize.y=680;return 0;}
        case WM_SIZE:InvalidateRect(window,nullptr,FALSE);return 0;
        case WM_MOUSEWHEEL:{int delta=GET_WHEEL_DELTA_WPARAM(wp);if(GetKeyState(VK_CONTROL)&0x8000)g_zoom=std::clamp(g_zoom+(delta>0?.08f:-.08f),.82f,1.6f);else g_scrollY=std::clamp(g_scrollY-delta/WHEEL_DELTA*90,0,g_maxScroll);InvalidateRect(window,nullptr,FALSE);return 0;}
        case WM_KEYDOWN:{if((GetKeyState(VK_CONTROL)&0x8000)&&(wp==VK_OEM_PLUS||wp==VK_ADD))g_zoom=std::min(1.6f,g_zoom+.08f);else if((GetKeyState(VK_CONTROL)&0x8000)&&(wp==VK_OEM_MINUS||wp==VK_SUBTRACT))g_zoom=std::max(.82f,g_zoom-.08f);else if((GetKeyState(VK_CONTROL)&0x8000)&&wp=='0')g_zoom=1;else if(wp==VK_HOME)g_scrollY=0;else if(wp==VK_END)g_scrollY=g_maxScroll;else break;InvalidateRect(window,nullptr,FALSE);return 0;}
        case WM_LBUTTONDOWN:{POINT p{(LONG)(GET_X_LPARAM(lp)/g_zoom),(LONG)(GET_Y_LPARAM(lp)/g_zoom)};if(InRect(g_addRect,p)){OpenDialog(DialogKind::Add);return 0;}if(InRect(g_aiRect,p)){OpenDialog(DialogKind::AI);return 0;}if(InRect(g_settingsRect,p)){OpenDialog(DialogKind::Settings);return 0;}
            RECT client{};GetClientRect(window,&client);if(p.y<298||p.y>(int)(client.bottom/g_zoom)-42)return 0;for(size_t i=0;i<g_taskRects.size();++i)if(InRect(g_taskRects[i],p)){g_tasks[i].done=!g_tasks[i].done;SavePlan();InvalidateRect(window,nullptr,FALSE);break;}return 0;}
        case WM_AI_RESULT:{bool ok=wp!=0;std::vector<Task> tasks;wstring error;{std::lock_guard<std::mutex>lock(g_aiMutex);tasks=g_aiTasks;error=g_aiError;}if(ok){g_tasks=std::move(tasks);SavePlan();if(g_dialog)CloseDialogWindow(g_dialog);InvalidateRect(window,nullptr,FALSE);}else if(g_dialog&&hAiStatus){SetWindowTextW(hAiStatus,error.c_str());EnableWindow(hGenerate,TRUE);}return 0;}
        case WM_PAINT:{PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);RECT client{};GetClientRect(window,&client);HDC memory=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);HGDIOBJ old=SelectObject(memory,bitmap);Graphics graphics(memory);graphics.ScaleTransform(g_zoom,g_zoom);RenderDashboard(graphics,(int)(client.right/g_zoom),(int)(client.bottom/g_zoom));BitBlt(dc,0,0,client.right,client.bottom,memory,0,0,SRCCOPY);SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);EndPaint(window,&paint);return 0;}
        case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(window,message,wp,lp);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
    SetProcessDPIAware();GdiplusStartupInput startup;if(GdiplusStartup(&g_gdiplus,&startup,nullptr)!=Ok)return 20;ComputeWeek();
    if(HasArgument(L"--self-test")){std::vector<Task>tasks;wstring error;bool ok=ParseAiPlan(L"1\t09:00\t60\t学习\t阅读\n4\t18:30\t45\t训练\t力量训练",tasks,error)&&tasks.size()==2&&tasks[0].day==0&&tasks[1].category==Category::Fitness;GdiplusShutdown(g_gdiplus);return ok?0:30;}
    wstring preview=ArgumentAfter(L"--render-preview");if(!preview.empty()){g_dataDir=L"";bool ok=SavePreview(preview);GdiplusShutdown(g_gdiplus);return ok?0:21;}
    bool verify=HasArgument(L"--verify");wchar_t current[32768]{};GetModuleFileNameW(nullptr,current,ARRAYSIZE(current));wstring local=EnvironmentOverride(L"ZHOUJI_SHARE_TEST_LOCALAPPDATA",FolderPath(CSIDL_LOCAL_APPDATA));wstring expected=local+L"\\ZhoujiPlanner\\周迹通用版.exe";
    if(verify||_wcsicmp(current,expected.c_str())!=0){int result=InstallAndLaunch(verify);GdiplusShutdown(g_gdiplus);if(result&&!verify)MessageBoxW(nullptr,L"安装通用版时发生错误。",L"周迹",MB_OK|MB_ICONERROR);return result;}
    g_dataDir=local+L"\\ZhoujiPlanner";std::filesystem::create_directories(g_dataDir);LoadPlan();LoadSettings();
    g_uiFont=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));wc.hIconSm=wc.hIcon;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"ZhoujiShareWindowV1";RegisterClassExW(&wc);
    WNDCLASSEXW dc{sizeof(dc)};dc.lpfnWndProc=DialogProc;dc.hInstance=instance;dc.hIcon=wc.hIcon;dc.hCursor=wc.hCursor;dc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);dc.lpszClassName=L"ZhoujiShareDialog";RegisterClassExW(&dc);
    RECT area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);int width=std::min(1500,(int)(area.right-area.left-60)),height=std::min(940,(int)(area.bottom-area.top-50));int x=area.left+(area.right-area.left-width)/2,y=area.top+(area.bottom-area.top-height)/2;
    g_window=CreateWindowExW(0,wc.lpszClassName,L"周迹 · 通用 AI 周计划",WS_OVERLAPPEDWINDOW,x,y,width,height,nullptr,nullptr,instance,nullptr);if(!g_window){GdiplusShutdown(g_gdiplus);return 22;}
    ShowWindow(g_window,show);UpdateWindow(g_window);MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!g_dialog||!IsDialogMessageW(g_dialog,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(g_uiFont)DeleteObject(g_uiFont);GdiplusShutdown(g_gdiplus);return 0;
}
