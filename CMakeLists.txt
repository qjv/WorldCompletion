cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
project(WorldCompletion LANGUAGES CXX)

set(GWTOOLBOX_ROOT "" CACHE PATH "Existing GWToolbox checkout with built SDK libraries")
set(GWTOOLBOX_BUILD "${GWTOOLBOX_ROOT}/build-wine" CACHE PATH "Existing GWToolbox build directory")
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "WorldCompletion requires a Windows x86 MSVC toolchain.")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "Use Release or RelWithDebInfo to match the prebuilt SDK runtime.")
endif()

set(SDK_LIBRARIES
    "${GWTOOLBOX_BUILD}/imgui.lib"
    "${GWTOOLBOX_BUILD}/GWToolboxdll/GWToolboxdll.lib"
    "${GWTOOLBOX_ROOT}/Dependencies/GWCA/lib/gwca.lib"
    "${GWTOOLBOX_BUILD}/vcpkg_installed/x86-windows-mixed/lib/minhook.x32.lib")
foreach(LIBRARY IN LISTS SDK_LIBRARIES)
    if(NOT EXISTS "${LIBRARY}")
        message(FATAL_ERROR "Missing prebuilt SDK library: ${LIBRARY}. This project never builds GWToolbox.")
    endif()
endforeach()

add_library(WorldCompletion SHARED
    plugins/WorldCompletion/WorldCompletion.cpp
    "${GWTOOLBOX_ROOT}/plugins/Base/dllmain.cpp"
    "${GWTOOLBOX_ROOT}/plugins/Base/ToolboxPlugin.cpp"
    "${GWTOOLBOX_ROOT}/plugins/Base/PluginUtils.cpp"
    "${GWTOOLBOX_ROOT}/plugins/Base/ToolboxUIPlugin.cpp"
    "${GWTOOLBOX_ROOT}/GWToolboxdll/ToolboxIni.cpp"
    "${GWTOOLBOX_ROOT}/GWToolboxdll/Utils/SettingsDoc.cpp")
file(GLOB PLUGIN_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/WorldCompletion/*.h")
# Wine's compiler wrapper does not emit usable header dependencies for Ninja.
set_property(SOURCE plugins/WorldCompletion/WorldCompletion.cpp APPEND PROPERTY OBJECT_DEPENDS "${PLUGIN_HEADERS}")
target_compile_features(WorldCompletion PRIVATE cxx_std_23)
set_target_properties(WorldCompletion PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
target_include_directories(WorldCompletion PRIVATE
    plugins/WorldCompletion
    "${GWTOOLBOX_ROOT}/plugins/Base"
    "${GWTOOLBOX_ROOT}/GWToolboxdll"
    "${GWTOOLBOX_ROOT}/Dependencies"
    "${GWTOOLBOX_BUILD}/_deps/imgui-src"
    "${GWTOOLBOX_BUILD}/_deps/imgui-src/backends"
    "${GWTOOLBOX_BUILD}/_deps/iconfontcppheaders-src"
    "${GWTOOLBOX_BUILD}/GWToolboxdll")
target_include_directories(WorldCompletion SYSTEM PRIVATE
    "${GWTOOLBOX_BUILD}/vcpkg_installed/x86-windows-mixed/include"
    "${GWTOOLBOX_ROOT}/Dependencies/GWCA/include")
target_compile_definitions(WorldCompletion PRIVATE BUILD_DLL NOMINMAX WIN32_LEAN_AND_MEAN ZMIJ_USE_SIMD=0
    IMGUI_USER_CONFIG="${GWTOOLBOX_ROOT}/GWToolboxdll/imconfig.h")
target_compile_options(WorldCompletion PRIVATE /W4 /WX /wd4201 /wd4505 /permissive- /Zc:preprocessor /Zc:lambda
    /constexpr:depth2048 /constexpr:steps10000000 /bigobj /Gy)
target_link_options(WorldCompletion PRIVATE /LTCG /OPT:REF /OPT:ICF /SAFESEH:NO /INCREMENTAL:NO)
target_link_libraries(WorldCompletion PRIVATE ${SDK_LIBRARIES})
