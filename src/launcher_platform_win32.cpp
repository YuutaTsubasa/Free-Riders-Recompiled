#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>

#include "imgui.h"
#include "launcher_platform.h"

namespace sfr::launcher {
namespace fs = std::filesystem;
namespace {
HWND owner_window = nullptr;

std::wstring quoted(const std::wstring& argument) {
    std::wstring text = L"\"" + argument;
    if (!argument.empty() && argument.back() == L'\\') text += L'\\';  // keep the closing quote
    return text + L"\"";
}

class Win32Game final : public GameProcess {
public:
    explicit Win32Game(HANDLE process) : process_(process) {}
    ~Win32Game() override { CloseHandle(process_); }
    std::optional<uint32_t> exit_code() override {
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return std::nullopt;
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return uint32_t(code);
    }
    HANDLE handle() const { return process_; }
private:
    HANDLE process_;
};
}

void set_owner_window(void* window) { owner_window = static_cast<HWND>(window); }

void* wait_handle(GameProcess& game) { return static_cast<Win32Game&>(game).handle(); }

fs::path launcher_directory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (length < path.size()) { path.resize(length); break; }
        path.resize(path.size() * 2);
    }
    return fs::path(path).parent_path();
}

bool can_pick_folders() { return true; }

bool copy_picked_file(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
    return !error;
}

std::string source_name(const fs::path& source) {
    const auto text = source.u8string();
    return std::string(text.begin(), text.end());
}

std::optional<fs::path> pick_path(const fs::path& start, bool folders) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folders) {
        const COMDLG_FILTERSPEC types[] = {{L"Disc image (*.iso)", L"*.iso;*.xiso"}, {L"*.*", L"*.*"}};
        dialog->SetFileTypes(2, types);
    }
    IShellItem* folder = nullptr;
    std::error_code error;
    const fs::path begin = fs::is_directory(start, error) ? start : start.parent_path();
    if (!begin.empty() && fs::is_directory(begin, error) &&
        SUCCEEDED(SHCreateItemFromParsingName(begin.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
    }
    std::optional<fs::path> chosen;
    IShellItem* item = nullptr;
    struct ReleaseKeys {
        // The dialog takes the key-up of the Enter or A that opened it.
        ~ReleaseKeys() { ImGui::GetIO().ClearInputKeys(); }
    } release_keys;
    if (SUCCEEDED(dialog->Show(owner_window)) && SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            chosen = fs::path(name);
            CoTaskMemFree(name);
        }
        item->Release();
    }
    dialog->Release();
    return chosen;
}

fs::path game_program(const fs::path& directory) { return directory / L"sfr_cpu_diagnostic.exe"; }

bool quit_with_game() { return true; }

std::unique_ptr<GameProcess> start_game(const LauncherSettings& settings, const fs::path& directory, const fs::path& log) {
    for (const auto& [name, value] : game_environment(settings))
        SetEnvironmentVariableA(name.c_str(), value.empty() ? nullptr : value.c_str());
    // Saves beside the launcher, whatever the game's working directory.
    SetEnvironmentVariableW(L"SFR_SAVE_DIRECTORY", (directory / L"save").c_str());
    const fs::path root = find_runtime_root(directory);
    // Without the checkout's shader tools, the pack beside the launcher.
    std::error_code missing;
    if (root.empty() && fs::is_regular_file(directory / L"shaders.pack", missing))
        SetEnvironmentVariableW(L"SFR_SHADER_PACK", (directory / L"shaders.pack").c_str());
    // A release translates the shaders the pack lacks with its own tools.
    if (root.empty())
        for (const auto& [name, value] : shader_tool_environment(directory))
            SetEnvironmentVariableW(std::wstring(name.begin(), name.end()).c_str(), value.c_str());
    const fs::path program = game_program(directory);
    std::wstring command = quoted(program.wstring());
    for (const auto& argument : game_arguments(settings)) command += L" " + quoted(argument);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION process{};
    // In the checkout when there is one: the game translates its shaders
    // with tools found relative to its working directory.
    const fs::path working = root.empty() ? directory : root;
    const BOOL started = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, working.c_str(), &startup, &process);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!started) return nullptr;
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
    return std::make_unique<Win32Game>(process.hProcess);
}

bool can_open_log() { return true; }

void open_log(const fs::path& log) { ShellExecuteW(owner_window, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

FontFiles font_files() {
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const fs::path fonts = fs::path(windows) / L"Fonts";
    return {{fonts / L"segoeui.ttf"},
            {fonts / L"seguisb.ttf"},
            {fonts / L"seguibli.ttf", fonts / L"segoeuib.ttf"},  // black italic, like a race logo
            {fonts / L"msjh.ttc"},
            {fonts / L"msjhbd.ttc"}};
}

int ui_language() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? 1 : 0; }
}
