cmake_minimum_required(VERSION 3.22)

# vcpkg supplies the native dependencies from vcpkg.json. The preset passes the toolchain.
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

project(app VERSION 0.1 LANGUAGES CXX)

# The product name lives here. MAIN_MODULE_URI is the internal QML module name.

set(APP_DISPLAY_NAME "AoWave" CACHE STRING "User-facing application name")
set(MAIN_MODULE_URI "App")

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "${APP_DISPLAY_NAME} builds with MSVC on Windows only (x64).")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Whole-program optimisation in Release: smaller, faster code for a slower link.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UPPER)

find_package(Qt6 REQUIRED COMPONENTS
    Quick QuickControls2 Qml Concurrent Sql Network Svg ShaderTools
    WebEngineCore WebEngineQuick WebEngineWidgets Widgets Test)
find_package(LibXml2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)

qt_standard_project_setup()
qt_policy(SET QTP0004 NEW)
qt_policy(SET QTP0008 NEW)   # deploy script content goes through as written

file(GLOB_RECURSE SOURCES   CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} src/*.cpp src/*.h)
list(REMOVE_ITEM SOURCES src/main.cpp src/launcher.cpp)   # the app DLL's entry and the launcher
file(GLOB_RECURSE QML_FILES CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} src/ui/qml/*.qml)
file(GLOB_RECURSE RESOURCES CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} resources/*)
file(GLOB SHADERS           CONFIGURE_DEPENDS RELATIVE ${PROJECT_SOURCE_DIR} src/ui/shaders/*.frag)

# Not in the repo; scripts/download-tools.ps1 populates them.
file(GLOB LIBS ${PROJECT_SOURCE_DIR}/third-parties/lib/*)
file(GLOB BINS ${PROJECT_SOURCE_DIR}/third-parties/bin/*)

configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

# Build settings for everything compiled from src/.
add_library(app_common INTERFACE)
target_compile_definitions(app_common INTERFACE
    APP_VERSION="${PROJECT_VERSION}"
    APP_NAME="${APP_DISPLAY_NAME}"
    MAIN_MODULE_URI="${MAIN_MODULE_URI}"
    NOMINMAX            # <windows.h>'s min/max macros break std::numeric_limits<T>::max()
    WIN32_LEAN_AND_MEAN)

# Sources carry UTF-8 BOMs and CJK literals.
target_compile_options(app_common INTERFACE
    /utf-8 /permissive- /Zc:__cplusplus /bigobj /W4
    /wd4100   # unreferenced formal parameter - Qt overrides are full of them
    /wd4127
    /wd4702
    /we4458   # a local that hides a data member reads the wrong variable
)

# qmltyperegistrar emits bare <basename.h>, so every source dir is an include dir.
list(TRANSFORM SOURCES REPLACE "/[^/]+$" "" OUTPUT_VARIABLE source_dirs)
list(REMOVE_DUPLICATES source_dirs)
list(TRANSFORM source_dirs PREPEND "${PROJECT_SOURCE_DIR}/")
target_include_directories(app_common INTERFACE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third-parties/includes ${source_dirs})
target_include_directories(app_common SYSTEM INTERFACE ${LIBXML2_INCLUDE_DIR})
# clangd cannot find this Visual Studio on its own, so the compile database names the INCLUDE
# dirs from vcvars. cl.exe already searches them; as external dirs their warnings stay quiet.
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    file(TO_CMAKE_PATH "$ENV{INCLUDE}" msvc_include_dirs)
    target_include_directories(app_common SYSTEM INTERFACE ${msvc_include_dirs})
endif()

target_link_libraries(app_common INTERFACE
    Qt6::Quick Qt6::QuickControls2 Qt6::Qml Qt6::Concurrent
    Qt6::Sql Qt6::Network Qt6::Svg Qt6::WebEngineCore Qt6::WebEngineQuick
    Qt6::Widgets Qt6::WebEngineWidgets   # the window a site's human check is shown in
    LibXml2::LibXml2 ZLIB::ZLIB unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator
    ${LIBS}
    bcrypt crypt32 dbghelp psapi ole32 uuid runtimeobject dwmapi)   # CNG, DPAPI, minidumps + module list, taskbar, WinRT, window bounds

set_source_files_properties(src/ui/qml/Globals.qml src/ui/qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# qmlcachegen runs only in Release, and is the only place QML syntax errors surface.
set(qml_extra_args "")
if(BUILD_TYPE_UPPER STREQUAL "DEBUG")
    list(APPEND qml_extra_args NO_CACHEGEN)
endif()

# Everything but main(): the App QML module with its C++ types, pages, icons and shaders. The
# executable and the tests link it, so each source compiles once and tests can render the UI.
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
    # The C++ items' base types (QQuickPaintedItem, QQuickFramebufferObject), for qmllint.
    DEPENDENCIES QtQuick
    ${qml_extra_args})

qt_add_shaders(app_module "app_shaders" PREFIX "/${MAIN_MODULE_URI}" FILES ${SHADERS})
target_precompile_headers(app_module PRIVATE src/pch.h)

# The app is a DLL, and AoWave.exe a small launcher that loads it from app\bin, so a release
# folder shows just AoWave.exe, app\ and the data\ the app keeps its state in.
# The launcher is the `app` target, the name the executable always had, so an IDE's run
# settings still point at it.
qt_add_library(app_core SHARED src/main.cpp)
set_target_properties(app_core PROPERTIES OUTPUT_NAME "${APP_DISPLAY_NAME}Core")
target_link_libraries(app_core PRIVATE app_module app_moduleplugin)

add_executable(${PROJECT_NAME} WIN32 src/launcher.cpp "${CMAKE_CURRENT_BINARY_DIR}/app.rc" app.manifest)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${APP_DISPLAY_NAME}")
# QT_BIN_DIR: the Qt a build folder's copy runs against, since none sits beside it there.
target_compile_definitions(${PROJECT_NAME} PRIVATE CORE_DLL=L"${APP_DISPLAY_NAME}Core.dll" APP_NAME=L"${APP_DISPLAY_NAME}"
    QT_BIN_DIR=L"$<TARGET_FILE_DIR:Qt6::Core>")
# Its own C runtime, linked in: the shared one it would need sits in app\bin, which it has not
# found yet on a PC without the Visual C++ redistributable.
set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(${PROJECT_NAME} app_core)

# Tests stay on the developer's machine, out of the repo, and out of release builds.
if(EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt" AND BUILD_TYPE_UPPER STREQUAL "DEBUG")
    add_subdirectory(tests)
endif()

# Every build, not only a relink: tools refreshed by download-tools.ps1 would otherwise stay stale.
add_custom_target(copy_runtime_bins ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BINS} "$<TARGET_FILE_DIR:app_core>"
    COMMENT "Copying runtime DLLs")
add_dependencies(copy_runtime_bins app_core)

# The mpv config a first run copies into data\mpv. copy_directory only adds, so a dropped
# shader would live on forever.
add_custom_target(copy_mpv_config ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:app_core>/mpv/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/third-parties/mpv-config" "$<TARGET_FILE_DIR:app_core>/mpv"
    COMMENT "Syncing mpv config")
add_dependencies(copy_mpv_config app_core)

# Deploy, Release only: cmake --install <build dir> fills <build dir>\deploy. AoWave.exe goes at
# the top, everything else into app\, laid out as a Qt prefix (bin, plugins, qml, resources,
# translations) that Qt finds from Qt6Core.dll's place, so no qt.conf is needed. Qt's QML app
# deploy script only takes executables, hence windeployqt on the DLL, scanning the QML sources
# for imports. Other build types install nothing.
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
        DEPLOY_TOOL_OPTIONS --qmldir \"${PROJECT_SOURCE_DIR}/src/ui/qml\")
    ")
    install(SCRIPT ${deploy_script})
    # The Visual C++ runtime beside the app, rather than an installer for the user to run: the
    # newest redist beside the compiler's own VC folder. Not InstallRequiredSystemLibraries, which
    # guesses the folder's toolset name (Microsoft.VC143.CRT) and misses Visual Studio 18's VC145.
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
    # The deploy script ships what the app never loads: every Controls style (it runs Universal,
    # on Basic), Qt5Compat's effects and the shader compiler only they import, a software OpenGL
    # (mpv on it is too slow to watch), WebEngine's DevTools and ~50 UI locales, Qt's own
    # translations (the app has its own), and plugins for image formats and geolocation nothing
    # here asks for.
    install(CODE [=[
        set(app "${CMAKE_INSTALL_PREFIX}/app")
        set(unshipped
            bin/opengl32sw.dll bin/Qt6ShaderTools.dll bin/Qt6QuickNativeStyle.dll
            qml/Qt5Compat qml/QtQuick/NativeStyle plugins/qmltooling plugins/position
            resources/qtwebengine_devtools_resources.pak)
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
        # Chromium's own strings, for the app's five languages.
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

    # vcpkg's DLL copy is a post-build step that install does not repeat and the Qt deploy
    # script does not cover. install(DIRECTORY) takes the file list at install time.

    if(DEFINED VCPKG_INSTALLED_DIR)
        set(vcpkg_root "${VCPKG_INSTALLED_DIR}")
    elseif(EXISTS "${CMAKE_BINARY_DIR}/vcpkg_installed")
        set(vcpkg_root "${CMAKE_BINARY_DIR}/vcpkg_installed")   # manifest mode's default
    else()
        set(vcpkg_root "$ENV{VCPKG_ROOT}/installed")            # classic mode
    endif()
    set(vcpkg_bin "${vcpkg_root}/${VCPKG_TARGET_TRIPLET}/bin")
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
