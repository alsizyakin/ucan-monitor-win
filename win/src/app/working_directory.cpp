// Upstream main.cpp loads its fonts from ../assets and writes log.txt and
// imgui.ini relative to the working directory, so the program works only when
// started from its bin/; otherwise ImGui falls back to a font without Cyrillic
// and shows question marks. This moves the working directory to the
// executable's own before main() runs, wherever the program was started from:
// a shortcut, a terminal, the build tree.

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


namespace {


std::filesystem::path executable_dir() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size == path.size()) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path).parent_path();
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path.parent_path();
#endif
}


[[maybe_unused]] const bool working_directory_set = [] {
    const auto dir = executable_dir();
    std::error_code error;
    if (!dir.empty()) {
        std::filesystem::current_path(dir, error);
    }
    return !dir.empty() && !error;
}();


} // namespace
