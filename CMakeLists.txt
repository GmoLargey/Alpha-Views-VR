cmake_minimum_required(VERSION 3.20)
project(AlphaViewsVR VERSION 1.0.0 LANGUAGES CXX)

# OpenVR SDK: only the header, import library and DLL are needed, fetched from Valve's repository.
set(OPENVR_TAG v2.15.6)
set(OPENVR_DIR ${CMAKE_BINARY_DIR}/openvr)
set(OPENVR_RAW https://raw.githubusercontent.com/ValveSoftware/openvr/${OPENVR_TAG})
foreach(f headers/openvr.h lib/win64/openvr_api.lib bin/win64/openvr_api.dll)
    if(NOT EXISTS ${OPENVR_DIR}/${f})
        file(DOWNLOAD ${OPENVR_RAW}/${f} ${OPENVR_DIR}/${f} STATUS st TLS_VERIFY ON)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Couldn't download OpenVR ${f}: ${st}")
        endif()
    endif()
endforeach()

# A Windows (not console) program: SteamVR starts it, and it lives in the dashboard's bottom bar.
add_executable(AlphaViewsVR WIN32 main.cpp)
set_target_properties(AlphaViewsVR PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(AlphaViewsVR PRIVATE /utf-8)   # the source is UTF-8 (the · in the panels)
target_include_directories(AlphaViewsVR PRIVATE ${OPENVR_DIR}/headers)
target_link_libraries(AlphaViewsVR PRIVATE ${OPENVR_DIR}/lib/win64/openvr_api.lib d3d11 d3dcompiler gdi32 shell32 ole32 windowscodecs)
target_compile_definitions(AlphaViewsVR PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    AVVR_VERSION="${PROJECT_VERSION}")

# The DLL and the SteamVR app manifest go next to the exe.
add_custom_command(TARGET AlphaViewsVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENVR_DIR}/bin/win64/openvr_api.dll $<TARGET_FILE_DIR:AlphaViewsVR>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/alphaviewsvr.vrmanifest $<TARGET_FILE_DIR:AlphaViewsVR>)
