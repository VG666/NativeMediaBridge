/**
 * @file nmb_util.cpp
 * @brief 全局对象定义与通用工具：错误文本、模块路径/DLL 加载、UTF-8 转换、ffmpeg 错误/源地址规范化、请求串切分、定时器精度。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 */
#include "core/nmb_internal.h"

namespace nmb {
// ── §3 全局对象与工具函数 ─────────────────────────────────────────────────
// g_browsers：进程内全部浏览器（NMB_Shutdown 时遍历销毁）；g_error 保存最近
// 一次对外错误文本。下面依次是错误记录、模块路径/DLL 加载、UTF-8/UTF-16
// 转换、ffmpeg 错误文本、mbQuery 请求串（制表符分隔）的切分/取值工具。
std::mutex g_mutex;
std::vector<Browser*> g_browsers;
std::wstring g_error;
HMODULE g_self = nullptr;  // 定义：DllMain 里赋本 DLL 模块句柄

// 宿主钩子（NMB_SetHostHooks 注册）：默认全空，桥行为不变。
HostHooks& hostHooks() {
    static HostHooks hooks;
    return hooks;
}


void setError(const std::wstring& value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_error = value;
}

void clearError() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_error.clear();
}

std::wstring windowsError(DWORD code) {
    wchar_t* text = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"未知错误";
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    return result;
}

std::wstring moduleDirectory() {
    wchar_t path[32768]{};
    DWORD length = GetModuleFileNameW(g_self, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    std::wstring result(path, length);
    size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

bool isAbsolutePath(const std::wstring& path) {
    return path.size() > 2 && ((path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
                              (path[0] == L'\\' && path[1] == L'\\'));
}

// 诊断开关：设 NMB_NO_NODEJS=1 就不给内核开 node。
//
// 为什么需要它：miniblink49 的 nodeblink 把宿主进程的**整个命令行**喂给
// node::CreateEnvironment（node/nodeblink.cpp），而 node 的 bootstrap 以
// process.argv[1] 为入口脚本（node/lib/internal/bootstrap_node.js）。宿主命令行里的
// --nwapp=...、-u、xxx.py 全会被当成脚本 require，找不到就当场致命退出——窗口一闪就没。
// 内核本来给 node 挂了 process._getPreloadScript，argv[1] 为空时才走"跑内嵌
// RenderInit.js"的分支（bootstrap_node.js 里 weolar 加的那段），但那条路被宿主参数挡住了。
// 根治要改 nodeblink.cpp 只保留 argv[0]；这里先用开关把 node 关掉，让桥能正常跑。
bool nodeJsDisabled() {
    static const bool disabled = [] {
        char buffer[8]{};
        return GetEnvironmentVariableA("NMB_NO_NODEJS", buffer, sizeof(buffer)) > 0;
    }();
    return disabled;
}

HMODULE loadDependency(const wchar_t* requested, const wchar_t* fallbackName, std::wstring& attempted) {
    std::vector<std::wstring> candidates;
    if (requested && *requested) candidates.emplace_back(requested);
    std::wstring base = moduleDirectory();
    if (requested && *requested && !isAbsolutePath(requested)) candidates.push_back(base + L"\\" + requested);
    candidates.push_back(base + L"\\" + fallbackName);
    candidates.push_back(base + L"\\..\\..\\tests\\" + fallbackName);
    candidates.push_back(base + L"\\..\\..\\tests\\dll\\" + fallbackName);
    DWORD lastError = ERROR_MOD_NOT_FOUND;
    for (const auto& candidate : candidates) {
        attempted += (attempted.empty() ? L"" : L"; ") + candidate;
        HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module) return module;
        lastError = GetLastError();
    }
    SetLastError(lastError);
    return nullptr;
}

HMODULE loadSiblingDependency(const wchar_t* fileName, std::wstring& attempted) {
    // fileName 可能是"同目录的裸文件名"，也可能是调用方（initializeKernel）已经拼好的
    // 绝对路径——后者不能再去拼 moduleDirectory()，否则会出现
    // "bin\F:\...\bin\xxx.dll" 这种两段绝对路径，LoadLibrary 一律报 126。
    // 裸文件名仍然按"与宿主 DLL 同级"解析（LOAD_WITH_ALTERED_SEARCH_PATH 保证依赖也找同目录）。
    attempted = (fileName && isAbsolutePath(fileName))
        ? fileName : moduleDirectory() + L"\\" + (fileName ? fileName : L"");
    return LoadLibraryExW(attempted.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring ffmpegError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return utf8ToWide(buffer);
}

std::string ffmpegSource(const std::wstring& source) {
    if (source.rfind(L"file://", 0) == 0) {
        std::wstring path = source.substr(7);
        if (path.size() >= 3 && path[0] == L'/' && path[2] == L':') path.erase(path.begin());
        std::replace(path.begin(), path.end(), L'/', L'\\');
        // file URL 的路径按 UTF-8 百分号编码；未解码时 FFmpeg 会寻找字面量 "%E7..." 文件。
        const std::string encoded = wideToUtf8(path);
        std::string decoded;
        decoded.reserve(encoded.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '%' && i + 2 < encoded.size() &&
                hex(encoded[i + 1]) >= 0 && hex(encoded[i + 2]) >= 0) {
                decoded.push_back(static_cast<char>((hex(encoded[i + 1]) << 4) | hex(encoded[i + 2])));
                i += 2;
            } else {
                decoded.push_back(encoded[i]);
            }
        }
        return decoded;
    }
    return wideToUtf8(source);
}
// 页面侧把一次请求的所有字段用制表符拼成一行发过来（见 kInjection 里的 send），
// 这里按制表符切开。制表符不会出现在 URL、数值或本模块自己的 op/id 里，所以不需要转义。
std::vector<std::wstring> splitFields(const std::wstring& request) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (;;) {
        size_t cut = request.find(L'\t', start);
        if (cut == std::wstring::npos) { fields.push_back(request.substr(start)); break; }
        fields.push_back(request.substr(start, cut - start));
        start = cut + 1;
    }
    return fields;
}

std::wstring field(const std::vector<std::wstring>& fields, size_t index) {
    return index < fields.size() ? fields[index] : std::wstring();
}

double number(const std::wstring& text, double fallback) {
    try { return std::stod(text); } catch (...) { return fallback; }
}
// Windows 默认的系统定时器精度是 15.6ms：std::this_thread::sleep_until 每帧都会多睡十几毫秒，
// 30fps 的视频算下来一帧变成 46ms 上下，播放速率直接掉到 0.7 倍（实测 0.69）。
// 解码/音频线程起来时把精度提到 1ms。定时器精度是进程级的，进程退出时系统自动复位。
void ensureTimerResolution() {
    static const bool raised = [] {
        timeBeginPeriod(1);
        return true;
    }();
    static_cast<void>(raised);
}

const wchar_t* lastErrorText() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_error.c_str();
}

std::vector<uint8_t> base64Decode(const std::string& in) {
    static const int8_t t[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    std::vector<uint8_t> out; out.reserve(in.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == '\t' || c == ' ') continue;
        int v = t[c]; if (v < 0) continue;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF)); }
    }
    return out;
}

} // namespace nmb
