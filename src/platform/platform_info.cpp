/**
 * @file platform_info.cpp
 * @brief Реализация сбора информации о платформе
 */
#include "platform_info.hpp"
#include <chrono>
#include <sstream>

// Платформо-специфичные заголовки
#if defined(__linux__)
#  include <sys/utsname.h>
#  include <unistd.h>
#elif defined(_WIN32)
#  include <windows.h>
#endif

namespace tb::platform {

PlatformInfo get_platform_info() {
    // Время запуска — инициализируем сразу, т.к. Timestamp не default-constructible
    auto now = std::chrono::system_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    PlatformInfo info{
        .os_name           = {},
        .hostname          = {},
        .build_type        = {},
        .compiler_version  = {},
        .app_version       = {},
        .start_time        = Timestamp{ns}
    };

    // Версия приложения (из CMake через define или константа)
#ifdef TB_VERSION
    info.app_version = TB_VERSION;
#else
    info.app_version = "0.1.0";
#endif

    // Тип сборки
#ifdef NDEBUG
    info.build_type = "Release";
#else
    info.build_type = "Debug";
#endif

    // Версия компилятора
#if defined(__clang__)
    info.compiler_version = "Clang " + std::to_string(__clang_major__) + "." +
                             std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    info.compiler_version = "GCC " + std::to_string(__GNUC__) + "." +
                            std::to_string(__GNUC_MINOR__);
#else
    info.compiler_version = "Unknown compiler";
#endif

    // ОС и хост
#if defined(__linux__)
    struct utsname uts{};
    if (uname(&uts) == 0) {
        info.os_name  = std::string(uts.sysname) + " " + uts.release;
        info.hostname = uts.nodename;
    } else {
        info.os_name  = "Linux";
        info.hostname = "unknown";
    }
#elif defined(_WIN32)
    info.os_name  = "Windows";
    char hostname[256] = {};
    DWORD size = sizeof(hostname);
    if (GetComputerNameA(hostname, &size)) {
        info.hostname = hostname;
    } else {
        info.hostname = "unknown";
    }
#else
    info.os_name  = "Unknown OS";
    info.hostname = "unknown";
#endif

    return info;
}

std::string format_startup_banner(const PlatformInfo& info) {
    std::ostringstream oss;
    oss << "\n";
    oss << "╔══════════════════════════════════════════════════════════╗\n";
    oss << "║         Tomorrow Bot — Adaptive Trading System           ║\n";
    oss << "╠══════════════════════════════════════════════════════════╣\n";
    oss << "║  Версия:    " << info.app_version
        << std::string(44 - info.app_version.size(), ' ') << "║\n";
    oss << "║  Сборка:    " << info.build_type
        << std::string(44 - info.build_type.size(), ' ') << "║\n";
    oss << "║  Компилятор: " << info.compiler_version
        << std::string(43 - info.compiler_version.size(), ' ') << "║\n";
    oss << "║  ОС:        " << info.os_name.substr(0, 43)
        << std::string(44 - std::min<size_t>(info.os_name.size(), 43), ' ') << "║\n";
    oss << "║  Хост:      " << info.hostname.substr(0, 43)
        << std::string(44 - std::min<size_t>(info.hostname.size(), 43), ' ') << "║\n";
    oss << "╚══════════════════════════════════════════════════════════╝\n";
    return oss.str();
}

} // namespace tb::platform
