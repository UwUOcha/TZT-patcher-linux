#include "launcher.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <csignal>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ansi.hpp"
#include "process_memory.hpp"

using namespace ansi;

namespace {

// Перенаправить stdout/stderr дочернего процесса в /dev/null. Иначе шумная
// диалоговая обёртка xdg-open (kfmclient not found, "test: : integer expected"
// при пустой переменной версии KDE) лезет прямо в наш аккуратный TUI.
void silenceChildOutput() {
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0) return;
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) close(devnull);
}

// Сбросить root до пользователя, вызвавшего sudo, и запустить Steam-URI.
// Вызывается ТОЛЬКО в дочернем процессе и завершается exec/_exit.
// Steam отказывается работать от root, поэтому уходим под SUDO_UID/SUDO_GID и
// восстанавливаем пользовательское окружение (HOME, XDG_RUNTIME_DIR), чтобы Steam
// нашёл сокет уже запущенного инстанса.
void dropPrivAndLaunchSteam() {
    const char* sudo_uid  = getenv("SUDO_UID");
    const char* sudo_gid  = getenv("SUDO_GID");
    const char* sudo_user = getenv("SUDO_USER");

    if (sudo_uid && sudo_gid && sudo_user) {
        const uid_t uid = static_cast<uid_t>(std::atoi(sudo_uid));
        const gid_t gid = static_cast<gid_t>(std::atoi(sudo_gid));

        // Порядок важен: группы → gid → uid (после setuid вернуть привилегии нельзя).
        if (initgroups(sudo_user, gid) != 0) _exit(126);
        if (setgid(gid) != 0)                _exit(126);
        if (setuid(uid) != 0)                _exit(126);

        if (const struct passwd* pw = getpwuid(uid))
            setenv("HOME", pw->pw_dir, 1);
        char xdg[64];
        snprintf(xdg, sizeof(xdg), "/run/user/%u", static_cast<unsigned>(uid));
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        setenv("USER", sudo_user, 1);
        setenv("LOGNAME", sudo_user, 1);
    }

    silenceChildOutput();

    // Сначала пробуем steam напрямую: он сам обрабатывает steam:// и куда надёжнее
    // обёртки xdg-open (та ломается на детекте окружения и заваливает вывод
    // ошибками). xdg-open оставлен как запасной вариант на случай нестандартной
    // установки Steam (flatpak/snap), где бинаря `steam` в PATH нет.
    execlp("steam",    "steam",    "steam://run/570", static_cast<char*>(nullptr));
    execlp("xdg-open", "xdg-open", "steam://run/570", static_cast<char*>(nullptr));
    _exit(127);
}

} // namespace

namespace launcher {

pid_t launchAndWaitForDota() {
    std::cout << CYAN << "[*] Launching Dota 2 via Steam (as invoking user)..." << RESET << "\n";

    if (getenv("SUDO_USER") == nullptr) {
        std::cout << YELLOW << "[!] SUDO_USER not set — run the tool with sudo, "
                  << "otherwise Steam won't start as root." << RESET << "\n";
    }

    signal(SIGCHLD, SIG_IGN); // авто-reap: лаунчер-child не должен висеть зомби

    if (fork() == 0) {
        // child: уходим под обычного пользователя и отдаём URI Steam'у
        dropPrivAndLaunchSteam();
        _exit(1); // недостижимо
    }
    // Не ждём child — он завершится быстро, Steam подхватит URI сам

    std::cout << "[*] Waiting for dota2 process";
    std::cout.flush();

    constexpr int TIMEOUT_SEC = 120;
    for (int i = 0; i < TIMEOUT_SEC * 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto pids = findProcessesByName("dota2");
        if (!pids.empty()) {
            std::cout << "\n";
            return pids[0];
        }
        if (i % 4 == 0) { std::cout << "."; std::cout.flush(); }
    }
    std::cout << "\n" << RED << "[!] Timeout waiting for Dota 2." << RESET << "\n";
    return 0;
}

bool waitForClientLib(pid_t pid, int timeoutSec) {
    std::cout << "[*] Waiting for libclient.so";
    std::cout.flush();
    for (int i = 0; i < timeoutSec * 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
        std::ifstream f(mapsPath);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("libclient.so") != std::string::npos) {
                std::cout << "\n";
                return true;
            }
        }
        if (i % 4 == 0) { std::cout << "."; std::cout.flush(); }
    }
    std::cout << "\n" << RED << "[!] Timeout waiting for libclient.so." << RESET << "\n";
    return false;
}

} // namespace launcher
