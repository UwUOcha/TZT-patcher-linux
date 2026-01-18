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

// ==========================================
// Visual Styles & Colors
// ==========================================
const std::string RESET   = "\033[0m";
const std::string RED     = "\033[31m";
const std::string GREEN   = "\033[32m";
const std::string YELLOW  = "\033[33m";
const std::string BLUE    = "\033[34m";
const std::string MAGENTA = "\033[35m";
const std::string CYAN    = "\033[36m";
const std::string BOLD    = "\033[1m";

void clearScreen() {
    std::cout << "\033[2J\033[1;1H";
}

void printBanner() {
    clearScreen();
    std::cout << CYAN << BOLD
              << "╔════════════════════════════════════════════════════╗\n"
              << "║              EXTERNAL PATCHER (LINUX)              ║\n"
              << "║        " << RESET << "Camera Distance & Weather Changer" << CYAN << BOLD << "         ║\n"
              << "╚════════════════════════════════════════════════════╝" << RESET << "\n\n";
}

// ==========================================
// Memory Classes
// ==========================================

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

    // Вспомогательный метод для чтения любого типа
    template<typename T>
    bool read(uintptr_t address, T& value) {
        return pread(memFd, &value, sizeof(T), address) == sizeof(T);
    }

    // Вспомогательный метод для записи любого типа
    template<typename T>
    bool write(uintptr_t address, const T& value) {
        return pwrite(memFd, &value, sizeof(T), address) == sizeof(T);
    }

    bool writeBytes(uintptr_t address, const std::vector<uint8_t>& data) {
        return pwrite(memFd, data.data(), data.size(), address) == static_cast<ssize_t>(data.size());
    }

    bool readBytes(uintptr_t address, size_t size, std::vector<uint8_t>& buffer) {
        buffer.resize(size);
        return pread(memFd, buffer.data(), size, address) == static_cast<ssize_t>(size);
    }

    std::vector<MemoryRegion> getMemoryRegions() {
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

// ==========================================
// Pattern Scanning
// ==========================================

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

std::vector<uintptr_t> findAllPatterns(ProcessMemory& mem, const MemoryRegion& region, const std::string& patternStr) {
    std::vector<uintptr_t> results;
    auto pattern = parsePattern(patternStr);
    const size_t CHUNK_SIZE = 0x100000; // 1MB chunks
    std::vector<uint8_t> buffer;

    if (region.perms.find('x') == std::string::npos) return results;

    for (uintptr_t addr = region.start; addr < region.end; addr += CHUNK_SIZE) {
        size_t readSize = std::min(CHUNK_SIZE, static_cast<size_t>(region.end - addr));
        if (!mem.readBytes(addr, readSize, buffer)) continue;

        for (size_t i = 0; i <= readSize - pattern.size(); ++i) {
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

uintptr_t scanForCameraAddress(ProcessMemory& mem, const MemoryRegion& region, float targetDistance) {
    if (region.perms.find('x') != std::string::npos) return 0;
    const size_t CHUNK_SIZE = 0x10000;
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

// ==========================================
// Weather Logic (IMPROVED)
// ==========================================

struct CachedPatchLocation {
    uintptr_t address;
    size_t originalSize;
    std::string typeName;
};

class WeatherManager {
    std::vector<CachedPatchLocation> locations;
    bool isScanned = false;

    struct PatternDef {
        std::string name;
        std::string signature;
    };

    const std::vector<PatternDef> PATTERNS = {
        { "Particles", "48 63 83 DC 0A 00 00" }, // MOVSXD RAX, [RBX+0xADC]
        { "Lighting",  "8B 83 DC 0A 00 00"    }  // MOV EAX, [RBX+0xADC]
    };

public:
    void scan(ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
        if (isScanned) return;

        std::cout << YELLOW << "[*] Scanning memory for weather signatures..." << RESET << "\n";

        for (const auto& region : regions) {
            for (const auto& pat : PATTERNS) {
                auto foundAddrs = findAllPatterns(mem, region, pat.signature);
                size_t sigSize = parsePattern(pat.signature).size();

                for (auto addr : foundAddrs) {
                    locations.push_back({ addr, sigSize, pat.name });
                }
            }
        }

        if (locations.empty()) {
            std::cout << RED << "[!] No weather signatures found! Game might be updated." << RESET << "\n";
        } else {
            std::cout << GREEN << "[+] Found " << locations.size() << " patch locations." << RESET << "\n";
            isScanned = true;
        }
    }

    void applyWeather(ProcessMemory& mem, int weatherId) {
        if (!isScanned) {
            std::cout << RED << "[!] Error: Not scanned yet." << RESET << "\n";
            return;
        }

        if (locations.empty()) {
            std::cout << RED << "[!] No locations to patch." << RESET << "\n";
            return;
        }

        std::vector<uint8_t> patch;
        // Генерируем инструкцию: MOV EAX, weatherId (B8 XX XX XX XX) -> 5 байт
        patch.push_back(0xB8);
        patch.resize(5);
        memcpy(&patch[1], &weatherId, sizeof(int));

        int successCount = 0;
        for (const auto& loc : locations) {
            // Создаем копию патча для конкретного места, чтобы добить NOP-ами
            std::vector<uint8_t> currentPatch = patch;
            while (currentPatch.size() < loc.originalSize) {
                currentPatch.push_back(0x90); // NOP
            }

            if (mem.writeBytes(loc.address, currentPatch)) {
                successCount++;
            }
        }

        std::cout << GREEN << "[+] Weather ID " << weatherId << " applied to " << successCount << " locations." << RESET << "\n";
        std::cout << BLUE << "(Move your camera or reconnect to see changes for Sky)" << RESET << "\n";
    }

    bool hasFoundLocations() const { return !locations.empty(); }
};

// ==========================================
// Main & UI
// ==========================================

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

    // Предварительный поиск регионов
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

    // Сразу сканируем погоду при запуске, чтобы сохранить оригинальные адреса
    weatherMgr.scan(mem, clientRegions);

    // Пауза перед входом в меню
    std::cout << "\nPress Enter to continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    while (running) {
        printBanner();

        // Status Info
        std::cout << "Camera Status: " << (cameraAddr != 0 ? (GREEN + "LINKED") : (RED + "NOT LINKED")) << RESET;
        if (cameraAddr != 0) std::cout << " (" << currentCamDist << ")";
        std::cout << "\nWeather Status: " << (weatherMgr.hasFoundLocations() ? (GREEN + "READY") : (RED + "NOT FOUND")) << RESET << "\n";
        std::cout << "------------------------------------------------------\n";

        std::cout << BOLD << "[1]" << RESET << " Setup Camera Distance\n";
        std::cout << BOLD << "[2]" << RESET << " Change Weather\n";
        std::cout << BOLD << "[0]" << RESET << " Exit\n";
        std::cout << "\n" << CYAN << "=> " << RESET;

        int choice;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        if (choice == 0) break;

        switch (choice) {
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
                sleep(2);
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
        }
    }

    return 0;
}
