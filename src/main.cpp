#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdint>
#include <vector>
#include <iomanip>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits>
#include <algorithm>
#include <sstream>
#include <map>

const std::string RESET   = "\033[0m";
const std::string RED     = "\033[31m";
const std::string GREEN   = "\033[32m";
const std::string YELLOW  = "\033[33m";
const std::string BLUE    = "\033[34m";
const std::string MAGENTA = "\033[35m";
const std::string CYAN    = "\033[36m";
const std::string BOLD    = "\033[1m";
const std::string DIM     = "\033[2m";
const std::string GRAY    = "\033[90m";

void clearScreen() {
    std::cout << "\033[2J\033[1;1H";
}

void printBanner() {
    clearScreen();
    std::cout << CYAN << BOLD
              << "╔════════════════════════════════════════════════════╗\n"
              << "║              EXTERNAL PATCHER (LINUX)              ║\n"
              << "║        " << RESET << "Camera Distance & Weather Changer  " << CYAN << BOLD << "         ║\n"
              << "╚════════════════════════════════════════════════════╝" << RESET << "\n\n";
}

struct MemoryRegion {
    uintptr_t start;
    uintptr_t end;
    std::string perms;
    std::string path;
};

class ProcessMemory {
    pid_t pid;
    int memFd;

public:
    ProcessMemory() : pid(-1), memFd(-1) {}

    ~ProcessMemory() {
        if (memFd >= 0) close(memFd);
    }

    bool attach(pid_t target_pid) {
        pid = target_pid;
        const std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
        memFd = open(memPath.c_str(), O_RDWR);
        return (memFd >= 0);
    }

    template<typename T>
    bool read(uintptr_t address, T& value) const {
        return pread(memFd, &value, sizeof(T), address) == sizeof(T);
    }

    template<typename T>
    bool write(uintptr_t address, const T& value) const {
        return pwrite(memFd, &value, sizeof(T), address) == sizeof(T);
    }

    bool writeBytes(uintptr_t address, const std::vector<uint8_t>& data) const {
        return pwrite(memFd, data.data(), data.size(), address) == static_cast<ssize_t>(data.size());
    }

    bool readBytes(uintptr_t address, size_t size, std::vector<uint8_t>& buffer) const {
        buffer.resize(size);
        return pread(memFd, buffer.data(), size, address) == static_cast<ssize_t>(size);
    }

    std::vector<MemoryRegion> getMemoryRegions() const {
        std::vector<MemoryRegion> regions;
        std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
        std::ifstream mapsFile(mapsPath);
        if (!mapsFile.is_open()) return regions;

        std::string line;
        while (std::getline(mapsFile, line)) {
            MemoryRegion region;
            size_t dashPos = line.find('-');
            size_t spacePos = line.find(' ');
            if (dashPos == std::string::npos) continue;

            region.start = std::stoull(line.substr(0, dashPos), nullptr, 16);
            region.end = std::stoull(line.substr(dashPos + 1, spacePos - dashPos - 1), nullptr, 16);
            region.perms = line.substr(spacePos + 1, 4);

            size_t pathPos = line.find('/');
            if (pathPos != std::string::npos) region.path = line.substr(pathPos);
            regions.push_back(region);
        }
        return regions;
    }
};

std::vector<pid_t> findProcessesByName(const std::string& processName) {
    std::vector<pid_t> pids;
    DIR* dir = opendir("/proc");
    if (!dir) return pids;

    pid_t myPid = getpid();
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        pid_t pid = atoi(entry->d_name);
        if (pid <= 0 || pid == myPid) continue;

        std::string cmdlinePath = "/proc/" + std::string(entry->d_name) + "/cmdline";
        std::ifstream cmdlineFile(cmdlinePath);
        if (cmdlineFile.is_open()) {
            std::string cmdline;
            std::getline(cmdlineFile, cmdline);
            if (cmdline.find(processName) != std::string::npos) pids.push_back(pid);
        }
    }
    closedir(dir);
    return pids;
}

std::vector<int> parsePattern(const std::string& pattern) {
    std::vector<int> bytes;
    std::stringstream ss(pattern);
    std::string byteStr;
    while (ss >> byteStr) {
        if (byteStr == "??" || byteStr == "?") bytes.push_back(-1);
        else bytes.push_back(std::stoi(byteStr, nullptr, 16));
    }
    return bytes;
}

std::vector<uintptr_t> findAllPatterns(const ProcessMemory& mem, const MemoryRegion& region, const std::vector<int>& pattern) {
    if (region.perms.find('x') == std::string::npos || pattern.empty())
        return {};

    constexpr size_t CHUNK_SIZE = 0x100000; // 1MB chunks

    std::vector<uint8_t> buffer;
    std::vector<uintptr_t> results;

    for (uintptr_t addr = region.start; addr < region.end; addr += CHUNK_SIZE) {
        const size_t readSize = std::min(CHUNK_SIZE, static_cast<size_t>(region.end - addr));
        if (readSize < pattern.size()) continue;
        if (!mem.readBytes(addr, readSize, buffer)) continue;

        // `i + pattern.size() <= readSize` avoids the size_t underflow that the
        // old `i <= readSize - pattern.size()` had on short region tails.
        for (size_t i = 0; i + pattern.size() <= readSize; ++i) {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                if (pattern[j] != -1 && buffer[i + j] != static_cast<uint8_t>(pattern[j])) {
                    found = false;
                    break;
                }
            }
            if (found) {
                results.push_back(addr + i);
            }
        }
    }
    return results;
}

uintptr_t scanForCameraAddress(const ProcessMemory& mem, const MemoryRegion& region, float targetDistance) {
    if (region.perms.find('x') != std::string::npos) return 0;
    constexpr size_t CHUNK_SIZE = 0x10000;
    std::vector<uint8_t> buffer;

    for (uintptr_t addr = region.start; addr < region.end - sizeof(float); addr += CHUNK_SIZE) {
        size_t readSize = std::min(CHUNK_SIZE, static_cast<size_t>(region.end - addr));
        if (!mem.readBytes(addr, readSize, buffer)) continue;

        for (size_t i = 0; i <= readSize - sizeof(float); i += 4) {
            float value;
            memcpy(&value, &buffer[i], sizeof(float));
            if (value >= targetDistance - 10 && value <= targetDistance + 10) {
                return addr + i;
            }
        }
    }
    return 0;
}

class WeatherManager {
    // Одно совпадение инструкции, читающей поле [RBX+disp].
    struct Hit {
        uintptr_t address;
        std::string type;   // "Particles" (movsxd) или "Lighting" (mov)
        uint32_t disp;      // реальное смещение, прочитанное из инструкции
        size_t size;        // длина инструкции (сколько байт перезаписываем)
    };

    // Снимок оригинальных байтов — чтобы откатить пробный патч.
    struct SavedBytes {
        uintptr_t address;
        std::vector<uint8_t> bytes;
    };

    std::vector<Hit> allHits;          // все найденные чтения [RBX+small]
    std::vector<SavedBytes> backup;    // оригиналы текущего НЕподтверждённого патча
    uint32_t resolvedOffset = 0;       // подтверждённый оффсет погоды
    bool isScanned = false;
    bool locked = false;               // оффсет подтверждён сменой погоды в игре

    // ─────────────────────────────────────────────────────────────────────────
    //  HINT_OFFSET — последний РАБОЧИЙ оффсет погоды. По одним байтам отличить
    //  поле погоды от сотни других полей нельзя, поэтому единственный надёжный
    //  критерий — реально ли поменялась погода в игре. Перебор идёт от кандидатов,
    //  ближайших к этому числу. Когда подтвердишь новый оффсет — впиши его сюда,
    //  чтобы в следующий раз перебор начинался прямо с него.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint32_t HINT_OFFSET = 0xAD4;
    static constexpr uint32_t MIN_OFFSET  = 0x100;  // нижняя граница разумного смещения
    static constexpr uint32_t MAX_OFFSET  = 0x4000; // верхняя граница разумного смещения
    static constexpr uint32_t TRIAL_WINDOW = 0x80;  // насколько далеко от HINT перебираем

    struct PatternDef {
        std::string name;
        std::string opcodePrefix; // байты инструкции ДО 32-битного смещения
    };

    const std::vector<PatternDef> PATTERNS = {
        { "Particles", "48 63 83" }, // MOVSXD RAX, [RBX+offset]
        { "Lighting",  "8B 83"    }  // MOV   EAX, [RBX+offset]
    };

    static uint32_t absDiff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

    // Байты патча "mov eax, id" + NOP-добивка до длины инструкции.
    static std::vector<uint8_t> makePatch(int weatherId, size_t size) {
        std::vector<uint8_t> p(5, 0);
        p[0] = 0xB8;
        memcpy(&p[1], &weatherId, sizeof(int));
        while (p.size() < size) p.push_back(0x90);
        return p;
    }

    // Все совпадения с данным смещением.
    std::vector<const Hit*> hitsForOffset(uint32_t offset) const {
        std::vector<const Hit*> out;
        for (const auto& h : allHits)
            if (h.disp == offset) out.push_back(&h);
        return out;
    }

public:
    uint32_t getOffset() const { return resolvedOffset; }
    bool hasCandidates() const { return !allHits.empty(); }
    bool isLocked() const { return locked; }

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
        if (isScanned) return;

        std::cout << YELLOW << "[*] Scanning weather instructions (baseline 0x"
                  << std::hex << HINT_OFFSET << std::dec << ")..." << RESET << "\n";

        // Маски с "дыркой" вместо 32-битного смещения:
        //   8B 83 ?? ?? 00 00       MOV    EAX, [RBX+offset]
        //   48 63 83 ?? ?? 00 00    MOVSXD RAX, [RBX+offset]
        // Старшие 2 байта смещения держим нулевыми => только небольшие оффсеты.
        struct WildPattern { std::string name; std::vector<int> sig; size_t prefixLen; };
        std::vector<WildPattern> wilds;
        for (const auto& pat : PATTERNS) {
            std::vector<int> sig = parsePattern(pat.opcodePrefix);
            const size_t prefixLen = sig.size();
            sig.push_back(-1);   sig.push_back(-1);
            sig.push_back(0x00); sig.push_back(0x00);
            wilds.push_back({ pat.name, std::move(sig), prefixLen });
        }

        for (const auto& region : regions) {
            for (const auto& w : wilds) {
                for (const auto addr : findAllPatterns(mem, region, w.sig)) {
                    uint32_t disp = 0;
                    if (!mem.read(addr + w.prefixLen, disp)) continue;
                    if (disp < MIN_OFFSET || disp > MAX_OFFSET) continue;
                    allHits.push_back({ addr, w.name, disp, w.sig.size() });
                }
            }
        }

        isScanned = true;
        if (allHits.empty()) {
            std::cout << RED << "[!] No candidate instructions found. Game changed too much." << RESET << "\n";
            return;
        }

        // Если известный рабочий оффсет на месте — сразу закрепляем его, чтобы в
        // обычном случае погода работала без всякого тюна.
        if (!hitsForOffset(HINT_OFFSET).empty()) {
            resolvedOffset = HINT_OFFSET;
            locked = true;
            std::cout << GREEN << "[+] Weather offset 0x" << std::hex << HINT_OFFSET << std::dec
                      << " is in place — ready to use." << RESET << "\n";
        } else {
            std::cout << YELLOW << "[!] Known offset 0x" << std::hex << HINT_OFFSET << std::dec
                      << " not found — game was probably updated.\n"
                      << "    Run [t] Recalibrate to find the new one." << RESET << "\n";
        }
    }

    // Уникальные смещения в окне вокруг HINT, отсортированные по близости к нему.
    std::vector<uint32_t> trialOffsets() const {
        std::vector<uint32_t> offs;
        for (const auto& h : allHits)
            if (absDiff(h.disp, HINT_OFFSET) <= TRIAL_WINDOW)
                offs.push_back(h.disp);
        std::sort(offs.begin(), offs.end());
        offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
        std::sort(offs.begin(), offs.end(), [](uint32_t a, uint32_t b) {
            return absDiff(a, HINT_OFFSET) < absDiff(b, HINT_OFFSET);
        });
        return offs;
    }

    // Запатчить ВСЕ инструкции с данным смещением, предварительно сохранив оригиналы.
    // Сколько мест записано — столько и возвращаем.
    int applyAt(const ProcessMemory& mem, uint32_t offset, int weatherId, bool keepBackup) {
        backup.clear();
        int count = 0;
        for (const Hit* h : hitsForOffset(offset)) {
            std::vector<uint8_t> orig;
            if (keepBackup && mem.readBytes(h->address, h->size, orig))
                backup.push_back({ h->address, orig });
            if (mem.writeBytes(h->address, makePatch(weatherId, h->size)))
                count++;
        }
        if (!keepBackup) backup.clear();
        return count;
    }

    // Вернуть оригинальные байты последнего пробного патча.
    void revert(const ProcessMemory& mem) {
        for (const auto& s : backup)
            mem.writeBytes(s.address, s.bytes);
        backup.clear();
    }

    // Зафиксировать найденный оффсет как рабочий.
    void lock(uint32_t offset) {
        resolvedOffset = offset;
        locked = true;
        backup.clear(); // подтверждённый патч откатывать не нужно
    }

    // Обычная смена погоды по уже подтверждённому оффсету.
    void applyWeather(const ProcessMemory& mem, int weatherId) {
        if (!locked) {
            std::cout << RED << "[!] Offset not confirmed yet. Run [3] Auto-tune first." << RESET << "\n";
            return;
        }
        const int count = applyAt(mem, resolvedOffset, weatherId, false);
        std::cout << GREEN << "[+] Weather ID " << weatherId << " applied to " << count
                  << " locations (offset 0x" << std::hex << resolvedOffset << std::dec << ")." << RESET << "\n";
        std::cout << BLUE << "(Move your camera or reconnect to see changes for Sky)" << RESET << "\n";
    }
};

void printWeatherList() {
    std::cout << "\n" << BOLD << "Available Weather IDs:" << RESET << "\n";
    std::cout << std::left << std::setw(20) << "1  - Snow" << std::setw(20) << "2  - Rain" << "\n";
    std::cout << std::left << std::setw(20) << "3  - Moonbeam" << std::setw(20) << "4  - Pestilence" << "\n";
    std::cout << std::left << std::setw(20) << "5  - Harvest" << std::setw(20) << "6  - Sirocco" << "\n";
    std::cout << std::left << std::setw(20) << "7  - Spring" << std::setw(20) << "8  - Ash" << "\n";
    std::cout << std::left << std::setw(20) << "9  - Aurora" << std::setw(20) << "0  - Default" << "\n";
}

int main(int, char**) {
    printBanner();

    if (geteuid() != 0) {
        std::cerr << RED << "[!] ROOT REQUIRED. Run with sudo." << RESET << "\n";
        return 1;
    }

    auto pids = findProcessesByName("dota2");
    if (pids.empty()) {
        std::cerr << RED << "[!] Dota 2 process not found. Launch the game first." << RESET << "\n";
        return 1;
    }
    pid_t dotaPid = pids[0];
    std::cout << "[*] Attached to Dota 2 PID: " << GREEN << dotaPid << RESET << "\n";

    ProcessMemory mem;
    if (!mem.attach(dotaPid)) {
        std::cerr << RED << "[!] Failed to open process memory." << RESET << "\n";
        return 1;
    }

    std::cout << "[*] Parsing memory maps...\n";
    auto regions = mem.getMemoryRegions();
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
    uintptr_t cameraAddr = 0;
    bool running = true;
    float currentCamDist = 1200.0f;

    weatherMgr.scan(mem, clientRegions);

    std::cout << "\nPress Enter to continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    while (running) {
        printBanner();

        const bool weatherNeedsCalib = !weatherMgr.isLocked() && weatherMgr.hasCandidates();
        const std::string divider = "  " + GRAY + "──────────────────────────────────────────────" + RESET + "\n";

        // ── Статус ──
        std::cout << "  " << (cameraAddr != 0 ? GREEN : RED) << "●" << RESET
                  << " Camera   " << GRAY << ":" << RESET << " ";
        if (cameraAddr != 0) std::cout << GREEN << "linked" << RESET << GRAY << "  " << currentCamDist << RESET;
        else                 std::cout << GRAY << "not linked" << RESET;
        std::cout << "\n";

        std::cout << "  ";
        if (weatherMgr.isLocked())
            std::cout << GREEN << "●" << RESET << " Weather  " << GRAY << ":" << RESET << " "
                      << GREEN << "ready" << RESET << GRAY << "  offset 0x"
                      << std::hex << weatherMgr.getOffset() << std::dec << RESET;
        else if (weatherMgr.hasCandidates())
            std::cout << YELLOW << "●" << RESET << " Weather  " << GRAY << ":" << RESET << " "
                      << YELLOW << "needs calibration" << RESET;
        else
            std::cout << RED << "●" << RESET << " Weather  " << GRAY << ":" << RESET << " "
                      << RED << "not found" << RESET;
        std::cout << "\n\n";

        // ── Главные действия ──
        std::cout << divider;
        std::cout << "   " << BOLD << CYAN << "[1]" << RESET << "  Camera distance\n";
        std::cout << "   " << BOLD << CYAN << "[2]" << RESET << "  Change weather\n";
        std::cout << "\n";

        // ── Второстепенная строка: тюн (подсвечен только если нужен) и выход ──
        std::cout << "   ";
        if (weatherNeedsCalib)
            std::cout << BOLD << YELLOW << "[t]" << RESET << YELLOW << " Recalibrate weather" << RESET
                      << YELLOW << "  ← сделай это сначала" << RESET;
        else
            std::cout << GRAY << "[t] recalibrate weather" << RESET;
        std::cout << GRAY << "   ·   [0] exit" << RESET << "\n";
        std::cout << divider;
        std::cout << "\n   " << CYAN << "❯ " << RESET;

        // Команды — строкой, чтобы принимать и цифры, и букву 't'.
        std::string cmd;
        if (!(std::cin >> cmd)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        int choice;
        if (cmd == "t" || cmd == "T")      choice = 3;
        else if (cmd == "0")               choice = 0;
        else if (cmd == "1")               choice = 1;
        else if (cmd == "2")               choice = 2;
        else                               continue; // неизвестная команда — перерисовать меню

        switch (choice) {
            case 0: {
                running = false;
                break;
            }
            case 1: {
                if (cameraAddr == 0) {
                    std::cout << "\nEnter current in-game distance (default 1200 or 1134): ";
                    float scanVal;
                    std::cin >> scanVal;
                    std::cout << YELLOW << "Scanning..." << RESET << "\n";
                    for (const auto& region : dataRegions) {
                        cameraAddr = scanForCameraAddress(mem, region, scanVal);
                        if (cameraAddr != 0) break;
                    }
                }

                if (cameraAddr != 0) {
                    std::cout << GREEN << "[+] Found Camera at 0x" << std::hex << cameraAddr << std::dec << RESET << "\n";
                    std::cout << "Enter new distance (e.g., 1400, 1350): ";
                    float val;
                    std::cin >> val;
                    if (mem.write(cameraAddr, val)) {
                        currentCamDist = val;
                        std::cout << GREEN << "Done!" << RESET << "\n";
                    } else {
                        std::cout << RED << "Write failed." << RESET << "\n";
                    }
                } else {
                    std::cout << RED << "[-] Camera entity not found. Try moving camera ingame." << RESET << "\n";
                }
                break;
            }
            case 2: {
                printWeatherList();
                std::cout << "\n" << CYAN << "Select ID: " << RESET;
                int id;
                std::cin >> id;

                weatherMgr.applyWeather(mem, id);

                std::cout << "\nPress Enter to return...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
            case 3: {
                auto offsets = weatherMgr.trialOffsets();
                if (offsets.empty()) {
                    std::cout << RED << "[!] No candidate offsets near baseline to try." << RESET << "\n";
                } else {
                    constexpr int TEST_WEATHER = 1; // Snow — частицы снега видно на земле сразу
                    std::cout << "\n" << BOLD << "Auto-tune offset" << RESET << "\n"
                              << "Программа по очереди ставит ВИДНУЮ погоду (Snow) на каждый кандидат,\n"
                              << "а ты смотришь в игру и отвечаешь y (да) / n (нет) / q (стоп).\n"
                              << YELLOW << "Зайди в матч или демо, где видно небо/землю, потом начинай.\n" << RESET
                              << "Кандидатов рядом с базой: " << offsets.size() << "\n";
                    std::cout << "Нажми Enter чтобы начать...";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

                    bool found = false;
                    for (const uint32_t off : offsets) {
                        const int n = weatherMgr.applyAt(mem, off, TEST_WEATHER, true);
                        std::cout << "\n[*] Offset 0x" << std::hex << off << std::dec
                                  << " — патч в " << n << " местах.\n";
                        std::cout << CYAN << "    Погода в игре ИЗМЕНИЛАСЬ? (y/n/q): " << RESET;
                        std::string ans;
                        std::cin >> ans;

                        if (ans == "y" || ans == "Y") {
                            weatherMgr.lock(off);
                            std::cout << GREEN << "[+] Оффсет 0x" << std::hex << off << std::dec
                                      << " зафиксирован! Теперь меняй погоду через [2]." << RESET << "\n";
                            std::cout << YELLOW << "    Впиши HINT_OFFSET = 0x" << std::hex << off << std::dec
                                      << " в исходник, чтобы в след. раз пробовался первым." << RESET << "\n";
                            found = true;
                            break;
                        }

                        weatherMgr.revert(mem); // не сработало — вернуть оригинальные байты
                        if (ans == "q" || ans == "Q") break;
                    }

                    if (!found)
                        std::cout << RED << "\n[!] Ни один близкий оффсет не сработал. "
                                            "Увеличь TRIAL_WINDOW в исходнике или ищи в Binary Ninja." << RESET << "\n";
                }

                std::cout << "\nPress Enter to return...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
        }
    }

    return 0;
}
