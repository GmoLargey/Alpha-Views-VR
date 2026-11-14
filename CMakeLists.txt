cmake_minimum_required(VERSION 3.20)
project(AlphaViewsVR VERSION 1.1.1 LANGUAGES CXX)

# Windows (64-bit) and Linux (x86-64). Everything is drawn on the CPU and handed to SteamVR as Vulkan
# images; only a thin platform layer in main.cpp differs. The SDK files below are downloaded once
# into the build folder.
set(DEPS ${CMAKE_BINARY_DIR}/deps)
function(fetch url dest)
    if(NOT EXISTS ${dest})
        file(DOWNLOAD ${url} ${dest} STATUS st TLS_VERIFY ON)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE ${dest})
            message(FATAL_ERROR "Couldn't download ${url}: ${st}")
        endif()
    endif()
endfunction()

# OpenVR SDK (BSD-3-Clause): the header, and the import library / DLL or the shared library.
set(OPENVR_TAG v2.15.6)
set(OPENVR_DIR ${CMAKE_BINARY_DIR}/openvr)
set(OPENVR_RAW https://raw.githubusercontent.com/ValveSoftware/openvr/${OPENVR_TAG})
if(WIN32)
    set(OPENVR_FILES headers/openvr.h lib/win64/openvr_api.lib bin/win64/openvr_api.dll)
    set(OPENVR_RUNTIME ${OPENVR_DIR}/bin/win64/openvr_api.dll)
else()
    set(OPENVR_FILES headers/openvr.h bin/linux64/libopenvr_api.so)
    set(OPENVR_RUNTIME ${OPENVR_DIR}/bin/linux64/libopenvr_api.so)
endif()
foreach(f ${OPENVR_FILES} LICENSE)
    fetch(${OPENVR_RAW}/${f} ${OPENVR_DIR}/${f})
endforeach()

# Vulkan headers (Apache-2.0 / MIT); the loader itself is opened at run time.
set(VULKAN_TAG vulkan-sdk-1.4.357.0)
set(VULKAN_DIR ${DEPS}/Vulkan-Headers-${VULKAN_TAG})
if(NOT EXISTS ${VULKAN_DIR}/include/vulkan/vulkan.h)
    fetch(https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${VULKAN_TAG}.tar.gz ${DEPS}/vulkan-headers.tar.gz)
    file(ARCHIVE_EXTRACT INPUT ${DEPS}/vulkan-headers.tar.gz DESTINATION ${DEPS})
endif()

# stb_truetype and stb_image_write (public domain / MIT), pinned to one commit.
set(STB_COMMIT 9600522de53973336fa033a289858a4354cbb04c)
foreach(f stb_truetype.h stb_image_write.h LICENSE)
    fetch(https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/${f} ${DEPS}/stb/${f})
endforeach()

add_executable(AlphaViewsVR WIN32 main.cpp)
set_target_properties(AlphaViewsVR PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(AlphaViewsVR PRIVATE ${OPENVR_DIR}/headers ${VULKAN_DIR}/include ${DEPS}/stb)
target_compile_definitions(AlphaViewsVR PRIVATE AVVR_VERSION="${PROJECT_VERSION}")

if(WIN32)
    # A Windows (not console) program: SteamVR starts it, and it lives in the dashboard's bottom bar.
    target_compile_options(AlphaViewsVR PRIVATE /utf-8)   # the source is UTF-8 (the · in the panels)
    target_compile_definitions(AlphaViewsVR PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(AlphaViewsVR PRIVATE ${OPENVR_DIR}/lib/win64/openvr_api.lib shell32 ole32)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif()
    # DirectXMath (MIT) is header-only and works with GCC / Clang; it wants a sal.h (linux/sal.h: empty
    # annotations).
    set(DXMATH_TAG jun2026)
    set(DXMATH_DIR ${DEPS}/DirectXMath-${DXMATH_TAG})
    if(NOT EXISTS ${DXMATH_DIR}/Inc/DirectXMath.h)
        fetch(https://github.com/microsoft/DirectXMath/archive/refs/tags/${DXMATH_TAG}.tar.gz ${DEPS}/directxmath.tar.gz)
        file(ARCHIVE_EXTRACT INPUT ${DEPS}/directxmath.tar.gz DESTINATION ${DEPS})
    endif()
    target_include_directories(AlphaViewsVR PRIVATE ${DXMATH_DIR}/Inc ${CMAKE_SOURCE_DIR}/linux)
    # libopenvr_api.so has no soname: link it by name, and find it next to the program.
    target_link_directories(AlphaViewsVR PRIVATE ${OPENVR_DIR}/bin/linux64)
    target_link_libraries(AlphaViewsVR PRIVATE openvr_api dl pthread)
    # The C++ runtime is built in, so the program runs on other distributions and in the Steam runtime.
    target_link_options(AlphaViewsVR PRIVATE -static-libstdc++ -static-libgcc)
    set_target_properties(AlphaViewsVR PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()

# The OpenVR library and the SteamVR app manifest go next to the program.
add_custom_command(TARGET AlphaViewsVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENVR_RUNTIME} $<TARGET_FILE_DIR:AlphaViewsVR>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/alphaviewsvr.vrmanifest $<TARGET_FILE_DIR:AlphaViewsVR>)
