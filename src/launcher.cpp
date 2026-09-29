// AoWave.exe. The app itself is AoWaveCore.dll in app\bin, beside Qt and the tools, so a release
// folder shows only this, app\ and data\. In a build folder the DLL sits beside this exe instead.
#include <windows.h>
#include <stdlib.h>
#include <iterator>
#include <string>

extern "C" {
// Prefer the discrete GPU on switchable graphics. Drivers read these off the exe only.
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// The narrow entry point: only it fills __argv, which QGuiApplication wants. Qt reads the real,
// Unicode arguments off the command line itself.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    wchar_t exe[32768];
    const DWORD length = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    std::wstring folder(exe, length);
    folder.resize(folder.find_last_of(L'\\') + 1);

    std::wstring dll;
    DWORD error = 0;
    for (const std::wstring &dir : {folder + L"app\\bin\\", folder}) {
        const std::wstring candidate = dir + CORE_DLL;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        dll = candidate;
        // A build folder runs against the Qt it was built with, which is not beside it. An IDE
        // only puts Qt on PATH for programs that link it, and this one does not.
        if (dir == folder) {
            std::wstring path(32767, L'\0');
            path.resize(GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size())));
            SetEnvironmentVariableW(L"PATH", (std::wstring(QT_BIN_DIR) + L';' + path).c_str());
        }
        // What the app loads later (Qt's plugins, D3D's compiler) looks here as well.
        SetDllDirectoryW(dir.c_str());
        using Main = int (*)(int, char **);
        const HMODULE core = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (const auto run = core ? reinterpret_cast<Main>(GetProcAddress(core, "aowaveMain")) : nullptr)
            return run(__argc, __argv);
        error = GetLastError();
        break;
    }

    std::wstring message;
    if (dll.empty()) {
        message = APP_NAME L" could not start: app\\bin\\" CORE_DLL L" is missing. Extract the download "
                  L"again into a fresh folder.";
    } else {
        wchar_t reason[512] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reason,
                       DWORD(std::size(reason)), nullptr);
        message = APP_NAME L" could not start: Windows could not load " + dll + L".\n\n" + reason
                + L"(error " + std::to_wstring(error) + L")";
        if (error == ERROR_MOD_NOT_FOUND)
            message += L"\n\nA DLL it needs is missing. Extract the download again into a fresh folder.";
    }
    MessageBoxW(nullptr, message.c_str(), APP_NAME, MB_ICONERROR);
    return 1;
}
