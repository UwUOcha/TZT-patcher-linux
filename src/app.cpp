#include "app.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
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
    uintptr_t libBase = 0; // база загрузки libclient.so (минимальный mapped-адрес = ELF vaddr 0)

    for (const auto& region : regions) {
        if (region.path.find("libclient.so") != std::string::npos ||
            region.path.find("client.dll") != std::string::npos) {
            if (libBase == 0 || region.start < libBase) libBase = region.start;
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

    weatherMgr.scan(mem, clientRegions);
    particlesMgr.scan(mem, clientRegions);
    riverMgr.scan(mem, libBase);
    plusMgr.scan(mem, libBase); // Dota Plus патчит фиксированные vaddr = libBase + offset

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
        if (plusMgr.isFound() && !plusMgr.isEnabled(mem))
            plusMgr.enable(mem);
        else if (!plusMgr.isFound())
            std::cout << YELLOW << "[!] Dota Plus selected, but status-read sites not verified "
                      << "(game updated? re-find vaddr as in eternal_plus_extern.py)." << RESET << "\n";
    }

    console::pause("\nPress Enter to continue...");

    bool running = true;
    while (running) {
        printBanner();

        const bool weatherNeedsCalib = !weatherMgr.isLocked() && weatherMgr.hasCandidates();

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
        } else if (weatherMgr.hasCandidates()) {
            statusRow(YELLOW, "Weather  ", YELLOW + "needs calibration" + RESET);
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
        std::cout << "   ";
        if (weatherNeedsCalib)
            std::cout << BOLD << YELLOW << "[t]" << RESET << YELLOW << " Recalibrate weather" << RESET
                      << YELLOW << "  ← do this first" << RESET;
        else
            std::cout << GRAY << "[t] recalibrate weather" << RESET;
        std::cout << GRAY << "   ·   [0] exit" << RESET << "\n";
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
        } else if (cmd == "t" || cmd == "T") {
            auto offsets = weatherMgr.trialOffsets();
            if (offsets.empty()) {
                std::cout << RED << "[!] No candidate offsets near baseline to try." << RESET << "\n";
            } else {
                constexpr int TEST_WEATHER = 1;
                std::cout << "\n" << BOLD << "Auto-tune offset" << RESET << "\n"
                          << "The tool applies a visible weather (Snow) for each candidate offset.\n"
                          << "Watch the game and answer y (yes) / n (no) / q (stop).\n"
                          << YELLOW << "Enter a match or demo where sky/ground is visible before starting.\n" << RESET
                          << "Candidates near baseline: " << offsets.size() << "\n";
                std::cout << "Press Enter to start...";
                console::flushLine();

                bool found = false;
                for (const uint32_t off : offsets) {
                    const int n = weatherMgr.applyAt(mem, off, TEST_WEATHER);
                    std::cout << "\n[*] Offset 0x" << std::hex << off << std::dec
                              << " — patched at " << n << " sites.\n";
                    std::cout << CYAN << "    Did weather CHANGE in game? (y/n/q): " << RESET;
                    std::string ans; std::cin >> ans;
                    if (ans == "y" || ans == "Y") {
                        weatherMgr.lock(off);
                        std::cout << GREEN << "[+] Offset 0x" << std::hex << off << std::dec
                                  << " confirmed! Use [2] to change weather now." << RESET << "\n";
                        std::cout << YELLOW << "    Update HINT_OFFSET = 0x" << std::hex << off << std::dec
                                  << " in the source so it's tried first next time." << RESET << "\n";
                        found = true;
                        break;
                    }
                    weatherMgr.revert(mem);
                    if (ans == "q" || ans == "Q") break;
                }
                if (!found)
                    std::cout << RED << "\n[!] No nearby offset worked. "
                                        "Increase TRIAL_WINDOW or search in Binary Ninja." << RESET << "\n";
            }
            console::pause();
        } else if (cmd == "3") {
            particlesMgr.toggle(mem);
            console::pause();
        }
        // прочий ввод — просто перерисовать меню
    }
}

pid_t App::runLaunchScreen(LaunchSelection& sel) {
    sel = LaunchSelection{}; // по умолчанию ничего не включено — «может не быть вайба»
    while (true) {
        printBanner();

        statusRow(RED, "Dota 2   ", RED + "not running" + RESET);
        std::cout << "\n";

        auto mark = [](bool on) {
            return on ? (GREEN + "[x]" + RESET) : (GRAY + "[ ]" + RESET);
        };

        std::cout << "  " << BOLD << "Enable at launch:" << RESET << "\n";
        std::cout << "   " << mark(sel.dotaPlus) << " " << BOLD << CYAN << "[d]" << RESET
                  << "  Dota Plus\n";
        std::cout << "   " << mark(sel.particles) << " " << BOLD << CYAN << "[p]" << RESET
                  << "  Particles fog-reveal\n";
        std::cout << "   " << GRAY << "(camera and weather are interactive — available in the menu after launch)"
                  << RESET << "\n\n";

        std::cout << console::divider();
        std::cout << "   " << BOLD << GREEN << "[L]" << RESET << "  Launch Dota 2"
                  << GRAY << "  (with selected mods)" << RESET << "\n";
        std::cout << "   " << GRAY << "[d]/[p] toggle mod   ·   [0] exit" << RESET << "\n";
        std::cout << console::divider();
        std::cout << "\n   " << CYAN << "❯ " << RESET;

        std::string cmd;
        if (!(std::cin >> cmd)) {
            console::flushLine();
            continue;
        }

        if (cmd == "0") return 0;
        if (cmd == "d" || cmd == "D") { sel.dotaPlus  = !sel.dotaPlus;  continue; }
        if (cmd == "p" || cmd == "P") { sel.particles = !sel.particles; continue; }

        if (cmd == "L" || cmd == "l") {
            const pid_t pid = launcher::launchAndWaitForDota();
            if (pid == 0) {
                std::cout << RED << "[!] Failed to detect Dota 2. Try again or launch manually.\n" << RESET;
                console::pause();
                continue;
            }
            // Код-патч требует загруженной libclient.so. Если выбран мод — дождёмся её.
            // Для Dota Plus патчим как можно РАНЬШE (до GC-логина), init-пауза короткая.
            if (sel.particles || sel.dotaPlus) {
                if (!launcher::waitForClientLib(pid)) {
                    std::cout << YELLOW << "[!] libclient.so not loaded — mods can be enabled manually from the menu.\n" << RESET;
                    sel = LaunchSelection{};
                } else {
                    std::cout << "[*] Waiting for init (patching before GC login)...";
                    std::cout.flush();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                    std::cout << " done.\n";
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
