cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    foreach(candidate "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}"
                      "$ENV{USERPROFILE}/vcpkg" "C:/vcpkg" "C:/dev/vcpkg" "C:/src/vcpkg")
        if(candidate AND EXISTS "${candidate}/scripts/buildsystems/vcpkg.cmake")
            set(CMAKE_TOOLCHAIN_FILE "${candidate}/scripts/buildsystems/vcpkg.cmake"
                CACHE FILEPATH "vcpkg toolchain" FORCE)
            break()
        endif()
    endforeach()
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "vcpkg not found. Set VCPKG_ROOT, or configure with\n"
        "  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
        "`cmake --preset release` does this for you.")
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "")

project(app VERSION 1.0 LANGUAGES CXX)

set(APP_DISPLAY_NAME "AoWave" CACHE STRING "User-facing application name")
set(MAIN_MODULE_URI "App")

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "${APP_DISPLAY_NAME} builds with MSVC on Windows only (x64).")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UPPER)

set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 REQUIRED COMPONENTS
    Quick QuickControls2 Qml Concurrent Sql Network Svg ShaderTools GuiPrivate
    WebEngineCore WebEngineQuick WebEngineWidgets Widgets)
find_package(LibXml2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)

qt_standard_project_setup()
qt_policy(SET QTP0004 NEW)
qt_policy(SET QTP0008 NEW)

file(GLOB_RECURSE SOURCES   CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} src/*.cpp src/*.h)
file(GLOB_RECURSE QML_FILES CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} src/ui/*.qml)
file(GLOB_RECURSE RESOURCES CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} resources/*)
file(GLOB SHADERS           CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} src/ui/shaders/*.frag src/ui/shaders/*.vert)

# The glob compiles whatever is under src, tracked or not: warn about files git does not know yet.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c core.quotePath=false ls-files -- src
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE tracked_files
                    RESULT_VARIABLE git_result OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(git_result EQUAL 0 AND tracked_files)
        string(REPLACE "\n" ";" tracked_files "${tracked_files}")
        set(untracked_sources ${SOURCES})
        list(REMOVE_ITEM untracked_sources ${tracked_files})
        if(untracked_sources)
            list(JOIN untracked_sources "\n  " untracked_list)
            message(WARNING "Compiled but not tracked by git (git add, or delete):\n  ${untracked_list}")
        endif()
    endif()
endif()

file(GLOB LIBS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/third-parties/lib/*)
file(GLOB BINS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/third-parties/bin/*)
find_file(D3DCOMPILER_REDIST d3dcompiler_47.dll NO_DEFAULT_PATH
          PATHS "$ENV{WindowsSdkDir}Redist/D3D/x64" "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64")
if(D3DCOMPILER_REDIST)
    list(APPEND BINS "${D3DCOMPILER_REDIST}")
endif()

configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

add_library(app_common INTERFACE)
target_compile_definitions(app_common INTERFACE
    APP_VERSION="${PROJECT_VERSION}"
    APP_NAME="${APP_DISPLAY_NAME}"
    MAIN_MODULE_URI="${MAIN_MODULE_URI}"
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

target_compile_options(app_common INTERFACE
    /utf-8 /permissive- /Zc:__cplusplus /bigobj /W4 /external:W0
    /wd4702
    /we4458
)

# qmltyperegistrar emits bare <basename.h>, so every source dir is an include dir.
list(TRANSFORM SOURCES REPLACE "/[^/]+$" "" OUTPUT_VARIABLE source_dirs)
list(REMOVE_DUPLICATES source_dirs)
list(TRANSFORM source_dirs PREPEND "${PROJECT_SOURCE_DIR}/")
target_include_directories(app_common INTERFACE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third-parties/includes ${source_dirs})
target_include_directories(app_common SYSTEM INTERFACE ${LIBXML2_INCLUDE_DIR})
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    file(TO_CMAKE_PATH "$ENV{INCLUDE}" msvc_include_dirs)
    target_include_directories(app_common SYSTEM INTERFACE ${msvc_include_dirs})
endif()

target_link_libraries(app_common INTERFACE
    Qt6::Quick Qt6::QuickControls2 Qt6::Qml Qt6::Concurrent
    Qt6::Sql Qt6::Network Qt6::Svg Qt6::WebEngineCore Qt6::WebEngineQuick
    Qt6::Widgets Qt6::WebEngineWidgets
    Qt6::GuiPrivate
    LibXml2::LibXml2 ZLIB::ZLIB unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator
    ${LIBS}
    bcrypt crypt32 dbghelp psapi ole32 uuid runtimeobject dwmapi
    dxgi)

set_source_files_properties(src/ui/Ui.qml src/ui/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

set(qml_extra_args "")
if(BUILD_TYPE_UPPER STREQUAL "DEBUG")
    list(APPEND qml_extra_args NO_CACHEGEN)
endif()

qt_add_library(app_module STATIC)
target_link_libraries(app_module PUBLIC app_common)
qt_add_qml_module(app_module
    URI ${MAIN_MODULE_URI}
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${MAIN_MODULE_URI}"
    VERSION 1.0
    RESOURCE_PREFIX "/"
    QML_FILES ${QML_FILES}
    SOURCES   ${SOURCES}
    RESOURCES ${RESOURCES}
    DEPENDENCIES QtQuick
    ${qml_extra_args})

qt_add_shaders(app_module "app_shaders" PREFIX "/${MAIN_MODULE_URI}" BATCHABLE FILES ${SHADERS})
target_precompile_headers(app_module PRIVATE src/pch.h)

qt_add_library(app_core SHARED entry/main.cpp)
set_target_properties(app_core PROPERTIES OUTPUT_NAME "${APP_DISPLAY_NAME}Core")
target_link_libraries(app_core PRIVATE app_module app_moduleplugin)

add_executable(${PROJECT_NAME} WIN32 entry/launcher.cpp "${CMAKE_CURRENT_BINARY_DIR}/app.rc" app.manifest)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${APP_DISPLAY_NAME}")
target_compile_definitions(${PROJECT_NAME} PRIVATE CORE_DLL=L"${APP_DISPLAY_NAME}Core.dll" APP_NAME=L"${APP_DISPLAY_NAME}"
    QT_BIN_DIR=L"$<TARGET_FILE_DIR:Qt6::Core>")
# Static C runtime: the shared one is in app\bin, which the launcher has not found yet.
set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(${PROJECT_NAME} app_core)

if(EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt" AND BUILD_TYPE_UPPER STREQUAL "DEBUG")
    add_subdirectory(tests)
endif()

add_custom_target(copy_runtime_bins ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BINS} "$<TARGET_FILE_DIR:app_core>"
    COMMENT "Copying runtime DLLs")
add_dependencies(copy_runtime_bins app_core)

add_custom_target(copy_mpv_config ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:app_core>/mpv"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/third-parties/mpv-config" "$<TARGET_FILE_DIR:app_core>/mpv"
    COMMENT "Syncing mpv config")
add_dependencies(copy_mpv_config app_core)

set(shader_seed_default ON)
if(DEFINED ENV{CI} OR BUILD_TYPE_UPPER STREQUAL "DEBUG")   # CI runners have no GPU; the Debug app warms up on its own
    set(shader_seed_default OFF)
endif()
option(AOWAVE_SHADER_SEED "Precompile the upscalers' shaders at build time" ${shader_seed_default})
if(AOWAVE_SHADER_SEED)
    file(GLOB SEED_INPUTS CONFIGURE_DEPENDS
         "${PROJECT_SOURCE_DIR}/third-parties/mpv-config/shaders/*.glsl"
         "${PROJECT_SOURCE_DIR}/third-parties/mpv-config/mpv.conf"
         "${PROJECT_SOURCE_DIR}/third-parties/bin/libmpv-2.dll")
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/shader-cache.stamp"
        COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:${PROJECT_NAME}> -DOUT=${CMAKE_BINARY_DIR}/shader-cache
                -DCONFIG=${CMAKE_BINARY_DIR}/mpv -DSTAMP=${CMAKE_BINARY_DIR}/shader-cache.stamp
                -P "${PROJECT_SOURCE_DIR}/scripts/seed-shaders.cmake"
        DEPENDS ${SEED_INPUTS} ${D3DCOMPILER_REDIST} "${PROJECT_SOURCE_DIR}/scripts/seed-shaders.cmake"
        COMMENT "Precompiling the upscalers' shaders (a few minutes, only when libmpv or the shaders change)"
        USES_TERMINAL)
    add_custom_target(shader_seed ALL DEPENDS "${CMAKE_BINARY_DIR}/shader-cache.stamp")
    add_dependencies(shader_seed ${PROJECT_NAME} app_core copy_runtime_bins copy_mpv_config)
endif()

if(BUILD_TYPE_UPPER STREQUAL "RELEASE")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/deploy" CACHE PATH "" FORCE)
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION .)
    install(TARGETS app_core RUNTIME DESTINATION app/bin)
    qt_generate_deploy_script(TARGET app_core OUTPUT_SCRIPT deploy_script CONTENT "
    set(QT_DEPLOY_PREFIX \"\${QT_DEPLOY_PREFIX}/app\")
    qt_deploy_runtime_dependencies(
        EXECUTABLE \"$<TARGET_FILE:app_core>\"
        BIN_DIR bin
        NO_COMPILER_RUNTIME
        DEPLOY_TOOL_OPTIONS --qmldir \"${PROJECT_SOURCE_DIR}/src/ui\")
    ")
    install(SCRIPT ${deploy_script})
    get_filename_component(vc_dir "${CMAKE_CXX_COMPILER}/../../../../../../.." ABSOLUTE)
    file(GLOB crt_dirs "${vc_dir}/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
    if(crt_dirs)
        list(SORT crt_dirs COMPARE NATURAL ORDER DESCENDING)
        list(GET crt_dirs 0 crt_dir)
        file(GLOB crt_dlls "${crt_dir}/*.dll")
        install(FILES ${crt_dlls} DESTINATION app/bin)
    else()
        message(WARNING "No Visual C++ redistributable beside ${CMAKE_CXX_COMPILER}; the package "
                        "will need the user to have it installed")
    endif()
    install(CODE [=[
        set(app "${CMAKE_INSTALL_PREFIX}/app")
        set(unshipped
            bin/opengl32sw.dll bin/Qt6ShaderTools.dll bin/Qt6QuickNativeStyle.dll
            qml/Qt5Compat qml/QtQuick/NativeStyle plugins/qmltooling plugins/position
            resources/qtwebengine_devtools_resources.pak
            bin/Qt6VirtualKeyboard.dll qml/QtQuick/VirtualKeyboard plugins/platforminputcontexts/qtvirtualkeyboardplugin.dll
            bin/Qt6Lottie.dll bin/Qt6LottieVectorImageGenerator.dll)
        foreach(style FluentWinUI3 Fusion Imagine Material Windows)
            list(APPEND unshipped qml/QtQuick/Controls/${style}
                 bin/Qt6QuickControls2${style}.dll bin/Qt6QuickControls2${style}StyleImpl.dll)
        endforeach()
        foreach(format qicns qtga qtiff qwbmp)
            list(APPEND unshipped plugins/imageformats/${format}.dll)
        endforeach()
        foreach(path IN LISTS unshipped)
            file(REMOVE_RECURSE "${app}/${path}")
        endforeach()
        file(GLOB qt_catalogs "${app}/translations/qt*.qm")
        file(GLOB locales "${app}/translations/qtwebengine_locales/*.pak")
        foreach(file IN LISTS qt_catalogs locales)
            if(NOT file MATCHES "/(en-US|zh-CN|zh-TW|ja|ko)\\.pak$")
                file(REMOVE "${file}")
            endif()
        endforeach()
    ]=])
    install(FILES ${BINS} DESTINATION app/bin)
    install(CODE "file(REMOVE_RECURSE \"\${CMAKE_INSTALL_PREFIX}/app/bin/mpv/shaders\")")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/third-parties/mpv-config/" DESTINATION app/bin/mpv)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/shader-cache/" DESTINATION app/bin/shader-cache OPTIONAL)

    set(vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    if(NOT EXISTS "${vcpkg_bin}")
        message(FATAL_ERROR "No vcpkg runtime DLLs at ${vcpkg_bin}. Deploying without them produces a "
                            "build that cannot start; set VCPKG_INSTALLED_DIR if they live elsewhere.")
    endif()
    install(DIRECTORY "${vcpkg_bin}/" DESTINATION app/bin
            FILES_MATCHING PATTERN "*.dll")
endif()

message(STATUS "** Build type : ${CMAKE_BUILD_TYPE}")
if(BUILD_TYPE_UPPER STREQUAL "RELEASE")
    message(STATUS "** Deploy dir : ${CMAKE_INSTALL_PREFIX}")
endif()
