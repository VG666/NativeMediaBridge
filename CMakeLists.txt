# CMakeLists.txt —— MSVC/CMake 的备用构建入口（日常首选 build-zig.bat）。
# 注意：默认 FFMPEG_ROOT 指向 ffmpeg-sdk 下的 LGPL shared 版 SDK；该目录体积
# 较大且与 ffmpeg-static 二选一，未随注解版拷贝，使用本构建方式前需自行放回。
# 仅支持 Windows x64；中文路径下 CMake 的 VS 生成器可能配置失败，可用 Ninja。
cmake_minimum_required(VERSION 3.20)
project(NativeMediaBridge LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NativeMediaBridge requires Windows x64")
endif()

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.toolchain/ffmpeg-sdk/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0" CACHE PATH "FFmpeg SDK root")

add_library(NativeMediaBridge SHARED
    api/native_media_bridge.cpp
    kernel/nmb_kernel.cpp
    core/nmb_util.cpp
    media/nmb_audio.cpp
    media/nmb_video.cpp
    media/nmb_media.cpp
    page/nmb_inject.cpp
    page/nmb_protocol.cpp
    render/nmb_composite.cpp
    platform/nmb_window.cpp
    api/native_media_bridge.def
)

# 注入脚本真源是 hook/js/*.js：有 python 时构建前自动重新生成 hook/inc/*.inc；
# 没找到 python 就直接编译随仓的 .inc（改过 js 又没 python 时记得手动生成）。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_EXECUTABLE)
    add_custom_target(NmbHooksInc ALL
        COMMAND "${Python3_EXECUTABLE}" utility/_nmb_gen_hooks_inc.py
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS
            hook/js/injection.js hook/js/media_api_shim.js
            hook/js/compat_shim.js hook/js/crypto_shim.js
            utility/_nmb_gen_hooks_inc.py
        COMMENT "从 hook/js/*.js 生成 hook/inc/*.inc")
    add_dependencies(NativeMediaBridge NmbHooksInc)
endif()

target_compile_features(NativeMediaBridge PRIVATE cxx_std_17)
target_compile_definitions(NativeMediaBridge PRIVATE NMB_EXPORTS WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(NativeMediaBridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utility
    ${FFMPEG_ROOT}/include
)
target_link_directories(NativeMediaBridge PRIVATE ${FFMPEG_ROOT}/lib)
target_link_libraries(NativeMediaBridge PRIVATE avcodec avformat avutil swscale swresample user32 gdi32 winmm)

if(MSVC)
    target_compile_options(NativeMediaBridge PRIVATE /W4 /EHsc /utf-8)
else()
    target_compile_options(NativeMediaBridge PRIVATE -Wall -Wextra)
endif()

set_target_properties(NativeMediaBridge PROPERTIES
    OUTPUT_NAME "NativeMediaBridge"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)
