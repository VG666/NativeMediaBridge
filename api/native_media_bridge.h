/**
 * @file    native_media_bridge.h
 * @brief   NativeMediaBridge（NMB）对外 C 接口。
 *
 * NativeMediaBridge 是套在 mb108 离屏内核（mb108_x64.dll，Chromium 内核）
 * 与宿主程序之间的一层“原生媒体桥”DLL，主要解决两件事：
 *   1. 网页里的 <video>/<audio> 改由本机 ffmpeg 解码、原生播放（视频帧覆盖回
 *      离屏表面的对应矩形，声音走 WASAPI 音频线程），站点自身的播放器 UI
 *      不受遮挡、样式仍可被第三方修改；
 *   2. 向页面注入兼容垫片（Chrome UA / 加密 API 垫片 / Fullscreen polyfill /
 *      hover 事件合成等），让桌面端常见网页在离屏内核里表现与真实 Chrome 接近。
 *
 * 本头文件就是宿主唯一需要包含的接口：先 NMB_Initialize()，再
 * NMB_CreateBrowser() 拿到浏览器句柄，用 NMB_LoadURL()/NMB_LoadHTML() 加载
 * 页面，在宿主消息循环里窗口会自动合成绘制；退出时 NMB_DestroyBrowser() +
 * NMB_Shutdown()。
 *
 * 调用约定：
 *   - 全部导出函数为 extern "C" + __cdecl，可被 C / C++ / Python(ctypes) /
 *     nw.js 等任意语言加载；
 *   - NMB_HANDLE 是不透明指针，不要自行解引用；
 *   - 除特别说明外，API 在宿主主线程（UI 线程）调用；
 *   - 宽字符字符串均为 UTF-16（Windows WCHAR）。
 *
 * 配套文件（src\ 按功能域分子目录）：
 *   api\      —— 本头 + NMB_* 导出薄封装与 DllMain + 导出表（.def），宿主只需包含本头
 *   kernel\   —— mb108/mb132 动态加载与 mb* 函数指针绑定
 *   core\     —— nmb_internal.h/nmb_types.h + 全局对象与通用工具
 *   media\    —— ffmpeg 音视频解码、媒体生命周期与状态 JSON
 *   page\     —— JS 注入与 mbQuery 协议
 *   render\   —— 页面+原生视频帧离屏合成（挖洞/软洞抠像）
 *   platform\ —— Win32 宿主窗口过程、输入转发与浏览器收尾
 *   内部实现全部在 namespace nmb；bin/mb108_x64.dll 为 mb108 离屏内核（第三方，运行时必需），
 *   bin/NativeMediaBridge.dll 为本桥编译产物。
 */
#pragma once
#include <windows.h>

#ifdef __cplusplus
#define NMB_EXTERN_C extern "C"
#else
#define NMB_EXTERN_C extern
#endif

/* 编译本 DLL 时由构建脚本定义 NMB_EXPORTS => dllexport；
   宿主包含本头文件时未定义 => dllimport。 */
#ifdef NMB_EXPORTS
#define NMB_API NMB_EXTERN_C __declspec(dllexport)
#else
#define NMB_API NMB_EXTERN_C __declspec(dllimport)
#endif

#ifndef NMB_CALL
#define NMB_CALL __cdecl
#endif

/* 不透明浏览器句柄，内部对应 native_media_bridge.cpp 里的 Browser 结构。 */
typedef void* NMB_HANDLE;

/**
 * @brief 初始化桥：动态加载 mb108 内核并绑定全部内核 API 指针。
 * @param miniblink_dll 内核 DLL 路径（mb108_x64.dll）；传 NULL 时使用
 *                      本桥 DLL 同目录 bin 下的默认文件名。
 * @param bass_dll       保留参数（早期 BASS 音频库路径），当前可传 NULL。
 * @return 1=成功；0=失败，可用 NMB_GetLastError() 取原因。
 * @note  进程内只需调用一次，重复调用安全（已初始化时直接返回成功）。
 */
NMB_API int NMB_CALL NMB_Initialize(const wchar_t* miniblink_dll, const wchar_t* bass_dll);

/**
 * @brief 创建一个离屏浏览器实例（内核表面 + 宿主可见窗口 + 注入脚本）。
 * @param parent 父窗口句柄；传 NULL 时自建顶层窗口。
 * @param x,y,width,height 窗口初始位置与客户区尺寸（像素）。
 * @return 浏览器句柄；失败返回 NULL（见 NMB_GetLastError）。
 */
NMB_API NMB_HANDLE NMB_CALL NMB_CreateBrowser(HWND parent, int x, int y, int width, int height);

/**
 * @brief 取底层 mb108 WebView 指针。
 * 高级用法：宿主想直接调内核导出函数（mbRunJs / mbGetTitle 等）时使用，
 * 普通宿主不需要。
 */
NMB_API void* NMB_CALL NMB_GetWebView(NMB_HANDLE browser);

/** @brief 设置底层内核 debug 配置（例如 enableNodejs=1），必须在首次加载页面前调用。 */
NMB_API void NMB_CALL NMB_SetDebugConfig(NMB_HANDLE browser, const char* name, const char* value);

/** @brief 取承载页面合成画面的窗口句柄（嵌入场景即子窗口 HWND）。 */
NMB_API HWND NMB_CALL NMB_GetWindow(NMB_HANDLE browser);

/** @brief 加载 URL（http/https/file/blob 等内核支持的协议）。成功返回 1。 */
NMB_API int NMB_CALL NMB_LoadURL(NMB_HANDLE browser, const wchar_t* url);

/** @brief 直接加载一段 HTML 文本（数据页/测试页常用）。成功返回 1。 */
NMB_API int NMB_CALL NMB_LoadHTML(NMB_HANDLE browser, const wchar_t* html);

/** @brief 改变浏览器窗口与内核表面尺寸（通常在父窗口 WM_SIZE 里调用）。 */
NMB_API void NMB_CALL NMB_Resize(NMB_HANDLE browser, int width, int height);

/** @brief 显示/隐藏窗口，show 取 1(显示) / 0(隐藏)。 */
NMB_API void NMB_CALL NMB_Show(NMB_HANDLE browser, int show);

/** @brief 顶层窗口模式下最大化(1)/还原(0)；嵌入父窗口时无实际效果。 */
NMB_API int NMB_CALL NMB_SetMaximized(NMB_HANDLE browser, int maximized);

/**
 * @brief 打开内核 DevTools 调试器窗口（wkeShowDevtools）。
 * @param path DevTools 前端资源路径（miniblink 发行包 front_end 目录下 inspector.html 的
 *             全路径，UTF-16）；传 NULL 时用内核默认资源（有没有取决于内核版本）。
 * @note  仅 miniblink_x64.dll（2023 双名版）有此内核导出；mb108 上调用会置错误并返回 0。
 *        内核没有"关闭"语义，调试器窗口由用户自行关闭。
 */
NMB_API int NMB_CALL NMB_ShowDevTools(NMB_HANDLE browser, const wchar_t* path);

/** @brief 当前是否处于最大化状态（1=是）。 */
NMB_API int NMB_CALL NMB_IsMaximized(NMB_HANDLE browser);

/**
 * @brief 销毁浏览器：止声、释放全部 ffmpeg 媒体、销毁窗口与内核表面。
 * @warning 内核后台开流线程可能仍在引用资源，内部做了延迟安全释放，
 *          但仍应在进程退出前调用，而不是强杀进程。
 */
NMB_API void NMB_CALL NMB_DestroyBrowser(NMB_HANDLE browser);

/** @brief 销毁全部浏览器并卸载内核 DLL（进程收尾）。 */
NMB_API void NMB_CALL NMB_Shutdown(void);

/**
 * @brief 取最近一次错误的文本（UTF-16，内部静态缓冲）。
 * 返回指针在下一次 API 调用后可能失效，需要留存请自行拷贝。
 */
NMB_API const wchar_t* NMB_CALL NMB_GetLastError(void);

/* ── 宿主钩子（可选）：把被桥占用的内核回调槽"还"给宿主 ──────────────────
 *
 * 桥在 NMB_CreateBrowser 里占用了 4 个 per-view 单槽内核回调：
 *   mbOnDocumentReady / mbOnDocumentReadyInBlinkThread（注入时机）
 *   mbOnDidCreateScriptContext（注入时机）
 *   mbOnJsQuery（页面→native 协议）
 *   mbOnPaintUpdated（离屏合成）
 * nw 宿主这类"自己也要往页面注入脚本、自己也要收页面 RPC"的宿主，经这组钩子
 * 把自己的逻辑挂回同样的时机。桥在自己的处理完之后调用宿主钩子，两不耽误。
 *
 * 线程约定与内核回调一致：onDocumentReady/onScriptContext/onJsQuery 在 UI 线程
 * （Blink 主线程）上回调，宿主可以碰窗口与消息循环。
 *
 * 生命周期：SetHostHooks 之后创建的所有浏览器都带钩子；传 NULL 清空。
 * 钩子指针与 param 由宿主保证在 NMB_Shutdown 前有效。
 */

/* 文档就绪：与 mbOnDocumentReady 同签名。 */
typedef void (*NMB_HostDocumentReadyFn)(void* view, void* param, void* frame);

/* 脚本上下文建立：与 mbOnDidCreateScriptContext 同签名（worldId 非 0 是扩展世界，
 * 桥不会为它调用宿主钩子）。 */
typedef void (*NMB_HostScriptContextFn)(void* view, void* param, void* frame, void* context,
                                        int extensionGroup, int worldId);

/* 页面 RPC：只转发**宿主形态**的请求——首字符是 '{' 的（JSON）。
 * 桥自己的媒体协议是制表符分隔的位置字段（op 在第 0 格），不会以 '{' 开头，
 * 两边按首字符分流互不误伤。返回 1 表示宿主已用 mbResponseQuery 应答、
 * 桥不再回包；返回 0 时桥按未知请求回 {"ok":false}。 */
typedef int (*NMB_HostJsQueryFn)(void* view, void* param, void* execState, long long queryId,
                                 int customMsg, const char* request);

typedef struct NMB_HostHooks {
    void* param;                            /* 透传给每个钩子的上下文，宿主自己解释 */
    NMB_HostDocumentReadyFn onDocumentReady;  /* 可空 */
    NMB_HostScriptContextFn onScriptContext;  /* 可空 */
    NMB_HostJsQueryFn onJsQuery;              /* 可空 */
} NMB_HostHooks;

/** @brief 注册/清除宿主钩子（传 NULL 清空）。在第一个 NMB_CreateBrowser 之前调用。 */
NMB_API void NMB_CALL NMB_SetHostHooks(const NMB_HostHooks* hooks);

/** @brief 媒体下载拦截（内核 onLoadUrlBegin 的媒体部分）。
 *
 * 内核该回调是 per-view 单槽：宿主（nw）把它让给自己的同步 RPC 退路后，对"非 RPC
 * 请求"调用本函数链式转发，内核的媒体字节流照旧由 ffmpeg 接管。返回 TRUE 表示请求
 * 已被接管（宿主直接把这个返回值回给内核）；FALSE 表示与媒体无关，宿主继续自己的
 * 放行逻辑。签名与 miniblink 的 mbOnLoadUrlBegin 回调一致，可直接挂在回调类型上。
 */
NMB_API int NMB_CALL NMB_MediaLoadUrlBegin(void* view, void* param, const char* url, void* job);
