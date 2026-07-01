#include "app.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "ansi.hpp"
#include "camera_manager.hpp"
#include "console.hpp"
#include "launcher.hpp"
#include "particles_manager.hpp"
#include "plus_manager.hpp"
#include "process_memory.hpp"
#include "river_manager.hpp"
#include "weather_manager.hpp"

using namespace ansi;

namespace {

// Одна строка статуса в шапке меню: цветной кружок, выровненная метка, значение.
void statusRow(const std::string& dotColor, const std::string& label, const std::string& value) {
    std::cout << "  " << dotColor << "●" << RESET << " " << label << GRAY << ":" << RESET
              << " " << value << "\n";
}

// Спросить float у пользователя. false при нечисловом вводе (буфер чистится).
bool readFloat(const std::string& prompt, float& out) {
    std::cout << prompt << CYAN;
    const bool ok = static_cast<bool>(std::cin >> out);
    std::cout << RESET;
    if (!ok) {
        console::flushLine();
        std::cout << RED << "Bad input." << RESET << "\n";
        return false;
    }
    return true;
}

std::string realHome() {
    if (const char* su = std::getenv("SUDO_USER"))
        if (struct passwd* pw = getpwnam(su)) return pw->pw_dir;
    if (const char* h = std::getenv("HOME")) return h;
    if (struct passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return {};
}

bool realIds(uid_t& uid, gid_t& gid) {
    if (const char* su = std::getenv("SUDO_USER"))
        if (struct passwd* pw = getpwnam(su)) {
            uid = pw->pw_uid;
            gid = pw->pw_gid;
            return true;
        }
    return false;
}

void chownToRealUser(const std::string& path) {
    uid_t uid;
    gid_t gid;
    if (realIds(uid, gid)) {
        if (::chown(path.c_str(), uid, gid) != 0) {
            // Non-fatal: root can still read/write the settings file.
        }
    }
}

std::string settingsPath() {
    const std::string home = realHome();
    return home.empty() ? ".tzt_patcher_settings" : home + "/.config/tzt_patcher/settings.conf";
}

void ensureParentDir(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return;
    const std::string dir = path.substr(0, slash);
    std::string cur;
    if (!dir.empty() && dir[0] == '/') cur = "/";
    size_t i = (cur == "/") ? 1 : 0;
    while (i <= dir.size()) {
        const size_t j = dir.find('/', i);
        const std::string part = dir.substr(0, j == std::string::npos ? dir.size() : j);
        if (!part.empty()) {
            ::mkdir(part.c_str(), 0755);
            chownToRealUser(part);
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
}

App::LaunchSelection loadLaunchSelection() {
    App::LaunchSelection sel;
    std::ifstream f(settingsPath());
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        const bool on = (val == "1" || val == "true" || val == "on");
        if (key == "dota_plus") sel.dotaPlus = on;
        else if (key == "particles") sel.particles = on;
    }
    return sel;
}

void saveLaunchSelection(const App::LaunchSelection& sel) {
    const std::string path = settingsPath();
    ensureParentDir(path);
    std::ofstream f(path);
    if (!f) return;
    f << "dota_plus=" << (sel.dotaPlus ? 1 : 0) << "\n";
    f << "particles=" << (sel.particles ? 1 : 0) << "\n";
    f.close();
    chownToRealUser(path);
}

char readKey() {
    std::cout.flush();
    if (!::isatty(STDIN_FILENO)) {
        char c = 0;
        std::cin.get(c);
        return c;
    }

    termios oldt{};
    if (::tcgetattr(STDIN_FILENO, &oldt) != 0) {
        char c = 0;
        std::cin.get(c);
        return c;
    }

    termios raw = oldt;
    raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char c = 0;
    const ssize_t n = ::read(STDIN_FILENO, &c, 1);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return n == 1 ? c : 0;
}

void pauseAfterHotkey(const std::string& msg = "\nPress Enter to return...") {
    std::cout << msg;
    std::cout.flush();
    std::cin.clear();
    std::cin.get();
}

} // namespace

void App::printWeatherList() {
    struct WeatherItem {
        std::string color;
        std::string icon; // глиф Nerd Font
        char key;
        std::string name;
    };

    // Иконки заданы кодпоинтами (\u / \U), чтобы не зависеть от вставки глифа.
    static const std::vector<WeatherItem> items = {
        { WHITE,  "",     '1', "Snow"       },
        { BLUE,   "",     '2', "Rain"       },
        { PURPLE, "",     '3', "Moonbeam"   },
        { GREEN,  "",     '4', "Pestilence" },
        { ORANGE, "\U000F0E69", '5', "Harvest"    },
        { YELLOW, "",     '6', "Sirocco"    },
        { PINK,   "\U000F09F2", '7', "Spring"     },
        { RED,    "",     '8', "Ash"        },
        { CYAN,   "\U000F078D", '9', "Aurora"     },
        { GRAY,   "•",          '0', "Default"    },
    };

    std::cout << "\n" << BOLD << "Weather:" << RESET << "\n";
    for (size_t i = 0; i < items.size(); i += 2) {
        for (size_t j = i; j < i + 2 && j < items.size(); ++j) {
            const auto& w = items[j];
            const std::string label = std::string("[") + w.key + "] " + w.name; // видимая ASCII-часть
            std::cout << "   " << w.color << w.icon << RESET << "  " << label;
            for (int pad = static_cast<int>(label.size()); pad < 16; ++pad)
                std::cout << ' ';
        }
        std::cout << "\n";
    }
}

void App::printRiverList() {
    struct RiverItem {
        std::string color;
        std::string icon;
        char key;
        std::string name;
    };

    static const std::vector<RiverItem> items = {
        { WHITE,  "\U000F11FD", '1', "Chrome"  },
        { YELLOW, "\uEE8E",     '2', "Dry"     },
        { GREEN,  "\uE275",     '3', "Slime"   },
        { PURPLE, "\U000F1053", '4', "Oil"     },
        { CYAN,   "\U000F140B", '5', "Electric"},
        { PINK,   "\U000F1308", '6', "Potion"  },
        { RED,    "\uE275",     '7', "Blood"   },
        { GRAY,   "•",          '0', "Default" },
    };

    std::cout << "\n" << BOLD << "River:" << RESET << "\n";
    for (size_t i = 0; i < items.size(); i += 2) {
        for (size_t j = i; j < i + 2 && j < items.size(); ++j) {
            const auto& r = items[j];
            const std::string label = std::string("[") + r.key + "] " + r.name;
            std::cout << "   " << r.color << r.icon << RESET << "  " << label;
            for (int pad = static_cast<int>(label.size()); pad < 16; ++pad)
                std::cout << ' ';
        }
        std::cout << "\n";
    }
}

void App::runExternalMode(pid_t dotaPid, const LaunchSelection& sel, bool launchedByUs) {
    ProcessMemory mem;
    if (!mem.attach(dotaPid)) {
        std::cerr << RED << "[!] Failed to open process memory (PID " << dotaPid << ")." << RESET << "\n";
        return;
    }

    std::cout << "[*] Parsing memory maps...\n";
    const auto regions = mem.memoryRegions();
    std::vector<MemoryRegion> clientRegions;
    std::vector<MemoryRegion> dataRegions;

    for (const auto& region : regions) {
        if (region.path.find("libclient.so") != std::string::npos ||
            region.path.find("client.dll") != std::string::npos) {
            if (region.perms.find('x') != std::string::npos) clientRegions.push_back(region);
            else if (region.perms.find("rw") != std::string::npos) dataRegions.push_back(region);
        }
    }
    std::cout << "[*] Found " << clientRegions.size() << " executable regions.\n";

    WeatherManager weatherMgr;
    RiverManager riverMgr;
    ParticlesManager particlesMgr;
    PlusManager plusMgr;
    CameraManager camera(mem, dataRegions, [&](uintptr_t addr) { weatherMgr.persistCamera(addr); });

    // Dota Plus читается один раз во время раннего GC/UI init. Если он выбран при
    // запуске, находим и ставим этот патч ПЕРЕД всеми тяжёлыми сигнатурными
    // сканами остальных модулей. Dota останавливается только на короткую запись.
    if (launchedByUs && sel.dotaPlus)
        plusMgr.scanAndEnableEarly(mem, clientRegions);
    else
        plusMgr.scan(mem, clientRegions);

    weatherMgr.scan(mem, clientRegions);
    particlesMgr.scan(mem, clientRegions);
    riverMgr.scan(mem, clientRegions);

    // Восстановление адреса камеры из файла состояния той же сессии Dota.
    if (const uintptr_t rc = weatherMgr.recoveredCameraAddr()) {
        float dist = 0.0f;
        if (mem.read(rc, dist) && dist > 100.0f && dist < 10000.0f) {
            camera.linkRecovered(rc, dist);
            std::cout << GREEN << "[+] Camera linked from previous session — distance "
                      << dist << "." << RESET << "\n";
        }
    }

    // Применяем выбранные на экране запуска моды сразу после старта игры.
    if (launchedByUs && sel.particles) {
        if (particlesMgr.isFound() && !particlesMgr.isEnabled(mem)) particlesMgr.toggle(mem);
        else if (!particlesMgr.isFound())
            std::cout << YELLOW << "[!] Particles selected, but gates not found." << RESET << "\n";
    }
    // Dota Plus (client-side) патчит ЧТЕНИЕ статуса подписки — это нужно поставить
    // ДО GC-логина, чтобы welcome/UI открылись уже как Plus.
    if (launchedByUs && sel.dotaPlus) {
        if (!plusMgr.isFound())
            std::cout << YELLOW << "[!] Dota Plus selected, but status-read sites not found "
                      << "(game updated a lot? refresh the mov[rax+0x2c] signatures)." << RESET << "\n";
        else if (!plusMgr.isComplete())
            std::cout << RED << "[!] Dota Plus early patch found only "
                      << plusMgr.siteCount() << "/3 status sites; refusing to report it as ON."
                      << RESET << "\n";
        else if (!plusMgr.isEnabled(mem))
            std::cout << RED << "[!] Dota Plus early patch was incomplete; refusing to report it as ON."
                      << RESET << "\n";
    }

    console::waitForEnter("\nPress Enter to continue...");

    bool running = true;
    while (running) {
        printBanner();

        // ── Статус ──
        statusRow(GREEN, "Dota 2   ",
                  GREEN + "running" + RESET + GRAY + "  PID " + std::to_string(dotaPid) + RESET);

        if (camera.isLinked()) {
            std::ostringstream v; v << GREEN << "linked" << RESET << GRAY << "  " << camera.distance() << RESET;
            statusRow(GREEN, "Camera   ", v.str());
        } else {
            statusRow(RED, "Camera   ", GRAY + "not linked" + RESET);
        }

        if (weatherMgr.isLocked()) {
            std::ostringstream v;
            v << GREEN << "ready" << RESET << GRAY << "  offset 0x"
              << std::hex << weatherMgr.offset() << std::dec << RESET;
            statusRow(GREEN, "Weather  ", v.str());
        } else {
            statusRow(RED, "Weather  ", RED + "not found" + RESET);
        }

        if (!riverMgr.isFound()) {
            statusRow(RED, "River    ", RED + "not found" + RESET);
        } else if (const int rid = riverMgr.currentId(mem); rid > 0) {
            std::ostringstream v; v << GREEN << "set" << RESET << GRAY << "  id " << rid << RESET;
            statusRow(GREEN, "River    ", v.str());
        } else {
            statusRow(GREEN, "River    ", GRAY + "ready (default)" + RESET);
        }

        if (!particlesMgr.isFound())
            statusRow(RED, "Particles", RED + "not found" + RESET);
        else if (particlesMgr.isEnabled(mem))
            statusRow(GREEN, "Particles", GREEN + "fog reveal ON" + RESET);
        else
            statusRow(YELLOW, "Particles", GRAY + "default" + RESET);

        if (!plusMgr.isFound())
            statusRow(RED, "Dota Plus", RED + "not found" + RESET);
        else if (plusMgr.isEnabled(mem))
            statusRow(GREEN, "Dota Plus", GREEN + "ON (client-side, launch patch)" + RESET);
        else
            statusRow(YELLOW, "Dota Plus", GRAY + "off (enable at launch)" + RESET);
        std::cout << "\n";

        // ── Главные действия ──
        std::cout << console::divider();
        std::cout << "   " << BOLD << CYAN << "[1]" << RESET << "  Camera distance\n";
        std::cout << "   " << BOLD << CYAN << "[2]" << RESET << "  Change weather\n";
        std::cout << "   ";
        if (particlesMgr.isFound())
            std::cout << BOLD << CYAN << "[3]" << RESET << "  Particles fog-reveal "
                      << GRAY << "(toggle)" << RESET << "\n";
        else
            std::cout << GRAY << "[3]  Particles fog-reveal (gates not found)" << RESET << "\n";
        std::cout << "   " << BOLD << CYAN << "[4]" << RESET << "  Change river\n";
        std::cout << "\n";

        // ── Сервисные действия ──
        std::cout << "   " << GRAY << "[c] relink camera (if already zoomed)" << RESET << "\n";
        std::cout << "   " << GRAY << "[0] exit" << RESET << "\n";
        std::cout << console::divider();
        std::cout << "\n   " << CYAN << "❯ " << RESET;

        std::string cmd;
        if (!(std::cin >> cmd)) {
            console::flushLine();
            continue;
        }

        if (cmd == "0") { running = false; continue; }

        if (cmd == "1") {
            if (!camera.isLinked()) {
                std::cout << YELLOW << "Linking camera (default " << CameraManager::DEFAULT_DISTANCE
                          << ")..." << RESET << "\n";
                camera.linkAt(CameraManager::DEFAULT_DISTANCE);
            }
            if (camera.isLinked()) {
                float val;
                if (readFloat("Enter desired distance (e.g. 1400, 1350): ", val)) {
                    if (camera.setDistance(val))
                        std::cout << GREEN << "[+] Done! Camera distance = " << val << RESET << "\n";
                    else
                        std::cout << RED << "Write failed." << RESET << "\n";
                }
            } else {
                std::cout << RED << "[-] Camera not at default " << CameraManager::DEFAULT_DISTANCE
                          << ".\n    If it's already zoomed out — press [c] and enter the current distance."
                          << RESET << "\n";
            }
            console::pause();
        } else if (cmd == "c" || cmd == "C") {
            float scanVal;
            if (readFloat("\nEnter CURRENT camera distance: ", scanVal)) {
                std::cout << YELLOW << "Scanning for " << scanVal << "..." << RESET << "\n";
                if (camera.linkAt(scanVal)) {
                    std::cout << GREEN << "[+] Camera linked at 0x" << std::hex << camera.address()
                              << std::dec << RESET << "\n";
                    float val;
                    if (readFloat("Enter desired distance (e.g. 1400, 1350): ", val)) {
                        if (camera.setDistance(val))
                            std::cout << GREEN << "[+] Done! Camera distance = " << val << RESET << "\n";
                        else
                            std::cout << RED << "Write failed." << RESET << "\n";
                    }
                } else {
                    std::cout << RED << "[-] Not found. Move the camera and enter the exact value."
                              << RESET << "\n";
                }
            }
            console::pause();
        } else if (cmd == "2") {
            printWeatherList();
            std::cout << "\n" << CYAN << "Select ID: " << RESET;
            int id;
            if (std::cin >> id) weatherMgr.applyWeather(mem, id);
            else { console::flushLine(); std::cout << RED << "Bad input." << RESET << "\n"; }
            console::pause();
        } else if (cmd == "4") {
            printRiverList();
            std::cout << "\n" << CYAN << "Select ID: " << RESET;
            int id;
            if (std::cin >> id) riverMgr.applyRiver(mem, id);
            else { console::flushLine(); std::cout << RED << "Bad input." << RESET << "\n"; }
            console::pause();
        } else if (cmd == "3") {
            particlesMgr.toggle(mem);
            console::pause();
        }
        // прочий ввод — просто перерисовать меню
    }
}

pid_t App::runLaunchScreen(LaunchSelection& sel) {
    sel = loadLaunchSelection();
    while (true) {
        printBanner();

        statusRow(RED, "Dota 2   ", RED + "not running" + RESET);

        auto mark = [](bool on) {
            return on ? (GREEN + "[x]" + RESET) : (GRAY + "[ ]" + RESET);
        };

        std::cout << "\n";
        std::cout << console::divider();
        std::cout << "   " << BOLD << GREEN << "[ENTER]" << RESET << "  Launch Dota 2"
                  << GRAY << "  (with selected mods)" << RESET << "\n";
        std::cout << "\n";
        std::cout << "   " << mark(sel.dotaPlus) << " " << BOLD << CYAN << "[1]" << RESET
                  << "  Dota Plus\n";
        std::cout << "   " << mark(sel.particles) << " " << BOLD << CYAN << "[2]" << RESET
                  << "  Particles fog-reveal\n";
        std::cout << "\n";
        std::cout << "   " << GRAY << "[0] exit" << RESET << "\n";
        std::cout << console::divider();
        std::cout << "\n   " << CYAN << "❯ " << RESET;

        const char key = readKey();
        if (key && key != '\n' && key != '\r') std::cout << key << "\n";
        else std::cout << "\n";

        if (key == '0') return 0;
        if (key == '1') {
            sel.dotaPlus = !sel.dotaPlus;
            saveLaunchSelection(sel);
            continue;
        }
        if (key == '2') {
            sel.particles = !sel.particles;
            saveLaunchSelection(sel);
            continue;
        }

        if (key == '\n' || key == '\r') {
            const pid_t pid = launcher::launchAndWaitForDota();
            if (pid == 0) {
                std::cout << RED << "[!] Failed to detect Dota 2. Try again or launch manually.\n" << RESET;
                pauseAfterHotkey();
                continue;
            }
            // Код-патч требует executable-сегмент libclient.so. Как только он
            // появился, сразу переходим к раннему Dota Plus patch-path.
            if (sel.particles || sel.dotaPlus) {
                if (!launcher::waitForClientLib(pid)) {
                    std::cout << YELLOW << "[!] libclient.so not loaded — mods can be enabled manually from the menu.\n" << RESET;
                    sel = LaunchSelection{};
                } else {
                    std::cout << "[*] libclient.so executable mapping ready";
                    if (sel.dotaPlus) std::cout << " — installing Dota Plus patch immediately";
                    std::cout << ".\n";
                }
            }
            return pid;
        }
    }
}

int App::run() {
    printBanner();

    if (geteuid() != 0) {
        std::cerr << RED << "[!] ROOT REQUIRED. Run with sudo." << RESET << "\n";
        return 1;
    }

    const auto pids = findProcessesByName("dota2");

    pid_t dotaPid = 0;
    LaunchSelection sel;
    bool launchedByUs = false;

    if (pids.empty()) {
        // Dota не запущена — экран выбора модов + запуск.
        dotaPid = runLaunchScreen(sel);
        if (dotaPid == 0) return 0; // пользователь вышел
        launchedByUs = true;
    } else {
        // Dota уже работает — цепляемся к процессу.
        dotaPid = pids[0];
        std::cout << "[*] Attached to Dota 2 PID: " << GREEN << dotaPid << RESET << "\n";
    }

    runExternalMode(dotaPid, sel, launchedByUs);
    return 0;
}
