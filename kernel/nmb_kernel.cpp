/**
 * @file nmb_kernel.cpp
 * @brief 内核封装：MbApi 全局实例定义、宽字符加载包装、mb108/mb132/miniblink DLL 的动态加载与全部函数指针绑定。
 *
 * 由 native_media_bridge.cpp 按职责拆分（P1-P5 重构）；逻辑未改。
 * 内部实现一律在 namespace nmb，跨模块接口集中声明于 nmb_internal.h。
 *
 * NMB_Initialize 直接加载真实的 miniblink 内核 DLL，不再经过任何兼容层。
 */
#include "core/nmb_internal.h"
#include <cwchar>

namespace {
// 支持显式指定 miniblink_x64.dll，也保留 mb108/mb132 作为手动回退内核。
bool isSupportedKernelTarget(const wchar_t* path) {
    if (!path || !*path) return true;
    const wchar_t* name = wcsrchr(path, L'\\');
    if (!name) name = wcsrchr(path, L'/');
    name = name ? name + 1 : path;
    if (_wcsicmp(name, L"mb108_x64.dll") == 0 ||
        _wcsicmp(name, L"mb132_x64.dll") == 0 ||
        _wcsicmp(name, L"miniblink_x64.dll") == 0) return true;
    const std::wstring fileName(name);
    return fileName.size() > 13 &&
           _wcsnicmp(fileName.c_str(), L"miniblink_", 10) == 0 &&
           fileName.compare(fileName.size() - 4, 4, L".dll") == 0;
}
}

namespace nmb {
MbApi g_kernel;

// 内核只提供 UTF-8 版本的加载接口，而本模块对外的接口一直是宽字符：
// 这两个包装负责转换，调用点（NMB_LoadURL / NMB_LoadHTML）完全不用动。
void __cdecl nmbLoadURLW(WebView view, const wchar_t* url) {
    if (!view || !g_kernel.mbLoadURL) return;
    const std::string utf8 = wideToUtf8(url ? url : L"");
    g_kernel.mbLoadURL(view, utf8.c_str());
}

void __cdecl nmbLoadHTMLW(WebView view, const wchar_t* html) {
    if (!view || !g_kernel.mbLoadHtmlWithBaseUrl) return;
    const std::string utf8 = wideToUtf8(html ? html : L"");
    // baseUrl 传空：页面里的相对地址就按默认空文档处理，和旧内核 wkeLoadHTMLW 的行为一致。
    g_kernel.mbLoadHtmlWithBaseUrl(view, utf8.c_str(), nullptr);
}

// 卸载内核（NMB_Shutdown 调用）。
void unloadKernel() {
    if (g_kernel.module) {
        FreeLibrary(g_kernel.module);
        g_kernel = {};
    }
}

// NMB_Initialize 必须最先调用：直接加载并绑定真实内核，
// 绑定 MbApi 全部函数指针，再创建全局窗口类/原子表等一次性资源。
bool initializeKernel(const wchar_t* miniblinkDll) {
    clearError();
    if (g_kernel.module) return true;
    // 默认内核固定为 miniblink_x64.dll；其他内核仅通过显式路径使用。
    if (!isSupportedKernelTarget(miniblinkDll)) {
        setError(L"不支持的内核（仅 mb108_x64 / mb132_x64 / miniblink_x64）");
        return false;
    }
    const std::wstring requested = miniblinkDll && *miniblinkDll ? miniblinkDll : L"miniblink_x64.dll";
    const std::wstring kernelPath = isAbsolutePath(requested)
        ? requested : moduleDirectory() + L"\\" + requested;
    const wchar_t* kernel = kernelPath.c_str();
    std::wstring attempted;
    g_kernel.module = loadSiblingDependency(kernel, attempted);
    if (!g_kernel.module) {
        DWORD code = GetLastError();
        setError(L"无法加载 miniblink_x64.dll，错误 " + std::to_wstring(code) + L": " + windowsError(code) + L"；尝试路径: " + attempted);
        return false;
    }
    // 必需接口：缺任何一个都跑不起来，直接失败（错误信息里带函数名，便于对照内核版本）。
#define MB(field,name) if(!loadProc(g_kernel.module,name,g_kernel.field)) return 0
    MB(createInitSettings,"mbCreateInitSettings");
    loadProc(g_kernel.module, "mbSetInitSettings", g_kernel.setInitSettings);
    MB(init,"mbInit");
    MB(setDebugConfig,"mbSetDebugConfig");
    loadProc(g_kernel.module, "mbSetNodeJsEnable", g_kernel.setNodeJsEnable);
    MB(createWebView,"mbCreateWebView");
    MB(destroyWebView,"mbDestroyWebView");
    MB(resize,"mbResize");
    MB(showWindow,"mbShowWindow");
    MB(setHandle,"mbSetHandle");
    MB(getViewDC,"mbGetLockedViewDC");
    MB(unlockViewDC,"mbUnlockViewDC");
    MB(onDocumentReady2,"mbOnDocumentReady");
    MB(wake,"mbWake");
    MB(runJsByFrame,"mbRunJs");
    MB(onJsQuery,"mbOnJsQuery");
    MB(responseQuery,"mbResponseQuery");
#undef MB
    // 可选接口：加载不到就保持空指针，调用点本来就逐处判空。
    loadProc(g_kernel.module, "mbOnLoadUrlBegin", g_kernel.onLoadUrlBegin);
    loadProc(g_kernel.module, "mbNetSetData", g_kernel.netSetData);
    loadProc(g_kernel.module, "mbCreateWebWindow", g_kernel.createWebWindow);
    loadProc(g_kernel.module, "mbSetHeadlessEnabled", g_kernel.setHeadlessEnabled);
    loadProc(g_kernel.module, "mbSetHandleOffset", g_kernel.setHandleOffset);
    // mb132 才有的"关掉内核自己上屏"开关：mb108 没有这个导出，而它这一版的 mbCreateWebView
    // 是纯离屏视图、本来就不往窗口上画，所以取不到时留空指针、调用点判空跳过即可。
    loadProc(g_kernel.module, "mbSetAutoDrawToHwnd", g_kernel.setAutoDrawToHwnd);
    loadProc(g_kernel.module, "mbSetTransparent", g_kernel.setTransparent);
    loadProc(g_kernel.module, "mbSetUserAgent", g_kernel.setUserAgent);
    loadProc(g_kernel.module, "mbOnPaintUpdated", g_kernel.onPaintUpdated);
    loadProc(g_kernel.module, "mbOnDocumentReadyInBlinkThread", g_kernel.onDocumentReadyInBlinkThread);
    loadProc(g_kernel.module, "mbOnDidCreateScriptContext", g_kernel.didCreateScriptContext);
    loadProc(g_kernel.module, "mbFireMouseEvent", g_kernel.fireMouseEvent);
    loadProc(g_kernel.module, "mbFireMouseWheelEvent", g_kernel.fireMouseWheelEvent);
    loadProc(g_kernel.module, "mbFireKeyDownEvent", g_kernel.fireKeyDownEvent);
    loadProc(g_kernel.module, "mbFireKeyUpEvent", g_kernel.fireKeyUpEvent);
    loadProc(g_kernel.module, "mbFireKeyPressEvent", g_kernel.fireKeyPressEvent);
    loadProc(g_kernel.module, "mbFireWindowsMessage", g_kernel.fireWindowsMessage);
    loadProc(g_kernel.module, "mbWebFrameGetMainFrame", g_kernel.getMainFrame);
    loadProc(g_kernel.module, "mbGetTitle", g_kernel.getTitle);
    loadProc(g_kernel.module, "mbSetFocus", g_kernel.setFocus);
    loadProc(g_kernel.module, "mbKillFocus", g_kernel.killFocus);
    loadProc(g_kernel.module, "mbIsMainFrame", g_kernel.isMainFrame);
    loadProc(g_kernel.module, "mbSetNavigationToNewWindowEnable", g_kernel.setNavigationToNewWindowEnable);
    loadProc(g_kernel.module, "mbSetCspCheckEnable", g_kernel.setCspCheckEnable);
    loadProc(g_kernel.module, "mbLoadURL", g_kernel.mbLoadURL);
    loadProc(g_kernel.module, "mbLoadHtmlWithBaseUrl", g_kernel.mbLoadHtmlWithBaseUrl);
    // DevTools 调试器（用户拍板：内核统一 miniblink_x64.dll 后顺带实装）。mb108 没有这个导出。
    loadProc(g_kernel.module, "wkeShowDevtools", g_kernel.showDevtools);
    // 对外仍然是宽字符接口，这里挂上转换包装。
    g_kernel.loadURLW = &nmbLoadURLW;
    g_kernel.loadHTMLW = &nmbLoadHTMLW;
    // mbInit 前必须通过初始化设置开启 Node；旧内核没有该 setter 时继续使用默认设置。
    void* settings = g_kernel.createInitSettings();
    // 设了 NMB_NO_NODEJS=1 就不给内核开 node（见 core/nmb_util.cpp::nodeJsDisabled 注释：
    // 内核的 nodeblink 会把宿主命令行当入口脚本，非脚本参数导致窗口一闪退出）。
    if (settings && g_kernel.setInitSettings)
        g_kernel.setInitSettings(settings, "enableNodejs", nodeJsDisabled() ? "0" : "1");
    g_kernel.init(settings);
    return true;
}

} // namespace nmb
