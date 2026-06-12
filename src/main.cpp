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
#include <sys/wait.h>
#include <csignal>
#include <cstdlib>
#include <pwd.h>
#include <grp.h>
#include <limits>
#include <algorithm>
#include <sstream>
#include <map>
#include <chrono>
#include <thread>

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
// 256-цветные оттенки для иконок погоды
const std::string WHITE   = "\033[97m";
const std::string ORANGE  = "\033[38;5;208m";
const std::string PURPLE  = "\033[38;5;141m";
const std::string PINK    = "\033[38;5;213m";

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

    pid_t getPid() const { return pid; }

    // Заморозка/разморозка всего процесса (как SuspendAllThreads из виндового DLL).
    // /proc/pid/mem пишется и без остановки, но для патча КОДА, который прямо сейчас
    // исполняют другие потоки, остановка избавляет от torn write. SIGSTOP/SIGCONT
    // тормозят весь процесс целиком — этого достаточно.
    void freeze() const   { if (pid > 0) kill(pid, SIGSTOP); }
    void unfreeze() const { if (pid > 0) kill(pid, SIGCONT); }

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

        for (size_t i = 0; i + sizeof(float) <= readSize; i += 4) {
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

    // Место патча: адрес инструкции + её ОРИГИНАЛЬНЫЕ байты (чтобы вернуть как было).
    struct PatchSite {
        uintptr_t address;
        size_t size;
        std::vector<uint8_t> original;
    };

    std::vector<Hit> allHits;          // все найденные чтения [RBX+small]
    std::vector<PatchSite> sites;      // подтверждённые места патча (+оригиналы)
    std::vector<PatchSite> backup;     // оригиналы текущего НЕподтверждённого пробного патча
    uintptr_t moduleBase = 0;          // база exec-региона libclient (ключ сессии)
    uint32_t resolvedOffset = 0;       // подтверждённый оффсет погоды
    bool isScanned = false;
    bool locked = false;               // оффсет подтверждён/восстановлен

    // Адрес дистанции камеры — тоже хранится в общем файле состояния, чтобы после
    // перезапуска проги (та же сессия Dota) сразу менять даже изменённую дистанцию.
    uintptr_t persistedCamera = 0;     // что писать в файл (0 = нет)
    uintptr_t loadedCamera = 0;        // что подхватили из файла этой сессии (0 = нет)

    // Файл состояния: чтобы при перезапуске проги (та же сессия Dota) подхватить
    // оффсет даже когда инструкции уже затёрты нашим патчем.
    const std::string STATE_PATH = "/tmp/dota_weather_patch.state";

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

    // Прочитать текущие (оригинальные на момент скана) байты мест данного смещения.
    std::vector<PatchSite> captureSites(const ProcessMemory& mem, uint32_t offset) const {
        std::vector<PatchSite> out;
        for (const Hit* h : hitsForOffset(offset)) {
            PatchSite s{ h->address, h->size, {} };
            if (mem.readBytes(h->address, h->size, s.original))
                out.push_back(std::move(s));
        }
        return out;
    }

    void saveState() const {
        std::ofstream f(STATE_PATH, std::ios::trunc);
        if (!f) return;
        f << "base " << std::hex << moduleBase << "\n";
        f << "offset " << std::hex << resolvedOffset << "\n";
        f << "camera " << std::hex << persistedCamera << "\n";
        for (const auto& s : sites) {
            f << "site " << std::hex << s.address << " " << std::dec << s.size << " ";
            for (const uint8_t b : s.original)
                f << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            f << "\n";
        }
    }

    struct State {
        bool valid = false;        // есть валидные данные погоды (база + места патча)
        uintptr_t base = 0;
        uint32_t offset = 0;
        uintptr_t cameraAddr = 0;
        std::vector<PatchSite> sites;
    };

    State loadState() const {
        State st;
        std::ifstream f(STATE_PATH);
        if (!f) return st;
        std::string tok;
        while (f >> tok) {
            if (tok == "base")        f >> std::hex >> st.base;
            else if (tok == "offset") f >> std::hex >> st.offset;
            else if (tok == "camera") f >> std::hex >> st.cameraAddr;
            else if (tok == "site") {
                PatchSite s; std::string hex;
                f >> std::hex >> s.address >> std::dec >> s.size >> hex;
                for (size_t i = 0; i + 1 < hex.size(); i += 2)
                    s.original.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
                st.sites.push_back(std::move(s));
            }
        }
        st.valid = (st.base != 0 && !st.sites.empty());
        return st;
    }

public:
    // Адрес камеры, подхваченный из файла этой же сессии (0 = нечего восстанавливать).
    uintptr_t recoveredCameraAddr() const { return loadedCamera; }

    // Запомнить адрес камеры и переписать файл состояния (вместе с данными погоды).
    void persistCamera(uintptr_t addr) {
        persistedCamera = addr;
        saveState();
    }

    uint32_t getOffset() const { return resolvedOffset; }
    bool hasCandidates() const { return !allHits.empty(); }
    bool isLocked() const { return locked; }

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
        if (isScanned) return;

        if (!regions.empty()) moduleBase = regions.front().start;

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

        // Файл состояния читаем один раз — он нужен и для камеры, и для погоды.
        const State st = loadState();
        const bool sameSession = (st.base != 0 && st.base == moduleBase);
        if (sameSession) {
            persistedCamera = st.cameraAddr; // не потерять при последующих перезаписях файла
            loadedCamera    = st.cameraAddr; // отдать main для восстановления камеры
        }

        if (allHits.empty()) {
            std::cout << RED << "[!] No candidate instructions found. Game changed too much." << RESET << "\n";
            return;
        }

        // Случай 1: сигнатуры целы (код не патчен) — известный оффсет на месте.
        // Сохраняем оригинальные байты и сразу готовы к работе.
        if (!hitsForOffset(HINT_OFFSET).empty()) {
            resolvedOffset = HINT_OFFSET;
            sites = captureSites(mem, HINT_OFFSET);
            locked = true;
            saveState();
            std::cout << GREEN << "[+] Weather offset 0x" << std::hex << HINT_OFFSET << std::dec
                      << " is in place — ready to use." << RESET << "\n";
            return;
        }

        // Случай 2: сигнатур нет — код уже затёрт нашим патчем в этой же сессии Dota.
        // Восстанавливаем оффсет из файла состояния (без рестарта игры).
        if (st.valid && sameSession) {
            resolvedOffset = st.offset;
            sites = st.sites;
            locked = true;
            std::cout << GREEN << "[+] Recovered weather offset 0x" << std::hex << st.offset << std::dec
                      << " from previous session — no Dota restart needed." << RESET << "\n";
            return;
        }

        // Случай 3: ни сигнатур, ни валидного состояния — нужна рекалибровка.
        std::cout << YELLOW << "[!] Known offset 0x" << std::hex << HINT_OFFSET << std::dec
                  << " not found — game was probably updated.\n"
                  << "    Run [t] Recalibrate to find the new one." << RESET << "\n";
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

    // Пробно запатчить все инструкции с данным смещением, сохранив оригиналы в backup.
    int applyAt(const ProcessMemory& mem, uint32_t offset, int weatherId) {
        backup.clear();
        int count = 0;
        for (const Hit* h : hitsForOffset(offset)) {
            PatchSite s{ h->address, h->size, {} };
            if (mem.readBytes(h->address, h->size, s.original))
                backup.push_back(s);
            if (mem.writeBytes(h->address, makePatch(weatherId, h->size)))
                count++;
        }
        return count;
    }

    // Вернуть оригинальные байты последнего пробного патча.
    void revert(const ProcessMemory& mem) {
        for (const auto& s : backup)
            mem.writeBytes(s.address, s.original);
        backup.clear();
    }

    // Зафиксировать найденный оффсет как рабочий: пробные оригиналы становятся
    // постоянными местами патча, состояние пишется в файл.
    void lock(uint32_t offset) {
        resolvedOffset = offset;
        locked = true;
        sites = std::move(backup);
        backup.clear();
        saveState();
    }

    // Смена погоды по подтверждённому оффсету.
    // id == 0 (Default) — ВОССТАНАВЛИВАЕМ оригинальный код, а не пишем ноль:
    // погода становится по-настоящему дефолтной и сигнатура снова на месте.
    void applyWeather(const ProcessMemory& mem, int weatherId) {
        if (!locked || sites.empty()) {
            std::cout << RED << "[!] Offset not confirmed yet. Run [t] Recalibrate first." << RESET << "\n";
            return;
        }

        int count = 0;
        if (weatherId == 0) {
            for (const auto& s : sites)
                if (mem.writeBytes(s.address, s.original)) count++;
            std::cout << GREEN << "[+] Default restored (original code) at " << count
                      << " locations." << RESET << "\n";
        } else {
            for (const auto& s : sites)
                if (mem.writeBytes(s.address, makePatch(weatherId, s.size))) count++;
            std::cout << GREEN << "[+] Weather ID " << weatherId << " applied to " << count
                      << " locations (offset 0x" << std::hex << resolvedOffset << std::dec << ")." << RESET << "\n";
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ParticlesManager — раскрытие партиклов в тумане войны (libclient.so).
//
//  Клиент каждый кадр (CDOTA_ParticleManager-cull) для каждого эффекта спрашивает
//  «видна ли точка в FoW» и по ответу зовёт SetRenderingEnabled. Сам FoW-запрос —
//  две функции-хелпера в libclient.so:
//      IsPointVisible(x,y,z) -> bool     — для крупных эффектов (несколько точек)
//      box-visibility(&p1,&p2) -> bool   — для мелких эффектов (радиус ≤ 1024)
//  Обе несут хеш-id (mov edx, 0x6b4ed927) и зовут общий CFoW-резолвер — это делает
//  их прологи уникальными. Форсим ОБЕ вернуть true (mov eax,1; ret) → каждая точка
//  «видима» → эффект рендерится всегда, в т.ч. сквозь туман.
//
//  Подтверждено вживую: вражеские спелл-партиклы видны сквозь FoW. Адреса берём
//  сигнатурным сканом по libclient.so; состояние переживает рестарт тулзы в той
//  же сессии Доты.
// ─────────────────────────────────────────────────────────────────────────────
class ParticlesManager {
    // Один патч-сайт: точка входа функции-хелпера + её оригинальные байты.
    struct Site {
        uintptr_t addr = 0;
        std::vector<uint8_t> original;
    };

    std::vector<Site> sites;     // оба FoW-хелпера видимости
    bool scanned = false;
    uintptr_t moduleBase = 0;

    // Файл состояния: подхватить сайты после рестарта тулзы в той же сессии Доты
    // (когда код уже затёрт патчем и сигнатуры не находятся).
    const std::string STATE_PATH = "/tmp/dota_particles_patch.state";

    // mov eax, 1 ; ret — хелпер всегда возвращает «видно».
    static const std::vector<uint8_t>& patchBytes() {
        static const std::vector<uint8_t> p = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
        return p;
    }

    // Прологи двух хелперов видимости. Обе несут `mov edx, 0x6b4ed927`
    // (BA 27 D9 4E 6B) — id для общего CFoW-резолвера; вместе с порядком пушей
    // это делает сигнатуры уникальными в libclient.so.
    static const std::vector<std::string>& signatures() {
        static const std::vector<std::string> s = {
            // IsPointVisible(x@xmm0, y@xmm1, z@xmm2) -> bool
            "55 31 C9 31 F6 BA 27 D9 4E 6B 48 89 E5 53 48 89 FB",
            // box-visibility(&p1, &p2) -> bool
            "55 31 C9 48 89 E5 41 55 49 89 D5 BA 27 D9 4E 6B 41 54 49 89 F4",
        };
        return s;
    }

    void saveState() const {
        if (sites.size() != signatures().size()) return;
        std::ofstream f(STATE_PATH, std::ios::trunc);
        if (!f) return;
        f << "base " << std::hex << moduleBase << "\n";
        for (const auto& s : sites) {
            f << "site " << std::hex << s.addr << " ";
            for (const uint8_t b : s.original)
                f << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            f << "\n";
        }
    }

    struct State { bool valid = false; uintptr_t base = 0; std::vector<Site> sites; };

    State loadState() const {
        State st;
        std::ifstream f(STATE_PATH);
        if (!f) return st;
        std::string tok;
        while (f >> tok) {
            if (tok == "base") f >> std::hex >> st.base;
            else if (tok == "site") {
                Site s; std::string hex;
                f >> std::hex >> s.addr >> hex;
                for (size_t i = 0; i + 1 < hex.size(); i += 2)
                    s.original.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
                st.sites.push_back(std::move(s));
            }
        }
        st.valid = (st.base != 0 && st.sites.size() == signatures().size());
        return st;
    }

public:
    bool isFound() const { return sites.size() == signatures().size(); }
    size_t siteCount() const { return sites.size(); }

    // Патч включён, если на первом сайте стоит наш `mov eax,1` (0xB8).
    bool isEnabled(const ProcessMemory& mem) const {
        if (!isFound()) return false;
        std::vector<uint8_t> buf;
        return mem.readBytes(sites.front().addr, 1, buf) && buf[0] == 0xB8;
    }

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
        if (scanned) return;
        scanned = true;
        if (!regions.empty()) moduleBase = regions.front().start;

        // Та же сессия Доты → подхватить сайты из файла (код мог быть уже патчен).
        const State st = loadState();
        if (st.valid && st.base == moduleBase) {
            sites = st.sites;
            std::cout << GREEN << "[+] Particles FoW gates recovered from state ("
                      << sites.size() << " sites)." << RESET << "\n";
            return;
        }

        for (const auto& sigStr : signatures()) {
            const std::vector<int> sig = parsePattern(sigStr);
            uintptr_t found = 0;
            for (const auto& region : regions) {
                const auto hits = findAllPatterns(mem, region, sig);
                if (!hits.empty()) { found = hits.front(); break; }
            }
            if (!found) continue;
            Site s{ found, {} };
            if (mem.readBytes(found, patchBytes().size(), s.original))
                sites.push_back(std::move(s));
        }

        if (isFound()) {
            saveState();
            std::cout << CYAN << "[*] Particles: both FoW gates located — [p] to reveal fog particles."
                      << RESET << "\n";
        } else if (sites.empty()) {
            std::cout << YELLOW << "[!] Particle FoW gates not found "
                      << "(game updated? refresh signatures from Binary Ninja)." << RESET << "\n";
        } else {
            // Нужны ОБА хелпера: мелкие эффекты идут через box-visibility, крупные —
            // через IsPointVisible. С одним патч не вскроет всё, поэтому отключаем.
            sites.clear();
            std::cout << YELLOW << "[!] Only one particle gate found — need both, patch disabled."
                      << RESET << "\n";
        }
    }

    // Вкл/выкл: пропатчить ОБА хелпера на «всегда видно» / вернуть оригиналы.
    void toggle(const ProcessMemory& mem) {
        if (!isFound()) {
            std::cout << RED << "[!] Particle gates not located. Game may have updated." << RESET << "\n";
            return;
        }
        if (isEnabled(mem)) {
            int n = 0;
            for (const auto& s : sites)
                if (mem.writeBytes(s.addr, s.original)) n++;
            std::cout << YELLOW << "[-] Particles fog-reveal OFF (restored " << n
                      << " sites)." << RESET << "\n";
        } else {
            int n = 0;
            for (const auto& s : sites)
                if (mem.writeBytes(s.addr, patchBytes())) n++;
            std::cout << GREEN << "[+] Particles fog-reveal ON — enemy particles show through fog ("
                      << n << " sites)!" << RESET << "\n";
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  PlusManager — клиентское (client-side) включение Dota Plus ВНЕШНИМ патчем.
//  Название «Eternal» из виндового инжектора тут не подходит: там код инжектится
//  .so в адресное пространство (internal), а мы патчим ИЗВНЕ через /proc/pid/mem
//  (external) — никакого нашего кода в памяти Доты нет.
//
//  Метод портирован из рабочего build/eternal_plus_extern.py (линуксовый аналог
//  виндового Находка.txt). UI-гейты читают поле статуса подписки инструкцией
//      mov reg, [rax+0x2c]     (3 байта:  8B 40 2C / 8B 50 2C)
//  Патчим её инлайн на
//      push 1; pop reg         (3 байта:  6A 01 58 / 6A 01 5A)
//  → чтение статуса всегда = 1 → клиент считает Plus активным (клиентсайдно).
//
//  РАБОТАЕТ ТОЛЬКО ПРИ ЗАПУСКЕ: чтение статуса одноразовое, на OnSOCreated/welcome
//  ДО GC-логина. Поэтому патчим из экрана запуска, ПОКА игра грузится. Менять Plus
//  в уже идущей игре смысла нет → в игровом меню тоггла нет.
//
//  Адреса — ФИКСИРОВАННЫЕ vaddr внутри libclient.so (libBase + vaddr), три места
//  потребителей (см. PATCHES в .py). Никакого vtbl-геттера — та цель (sub_5402eb0)
//  оказалась проверкой EVENT-GAME (0x330) и крашила; патчим именно чтение поля.
//
//  Если оригинальные байты не совпали — Dota обновилась, бинарь сдвинулся: НЕ
//  патчим (чтобы не покалечить), нужно перенайти vaddr под новый libclient.
// ─────────────────────────────────────────────────────────────────────────────
class PlusManager {
    // Один потребитель статуса подписки: vaddr инструкции чтения поля + ожидаемые
    // оригинальные байты + патч. ВСЕ три — фиксированной длины 3 байта (mov reg,
    // [rax+0x2c]  ⇄  push 1; pop reg), ровно как PATCHES в eternal_plus_extern.py.
    struct Site {
        uintptr_t vaddr;                // смещение внутри libclient.so (как в .py)
        std::vector<uint8_t> orig;      // mov reg,[rax+0x2c]  (8B 40 2C / 8B 50 2C)
        std::vector<uint8_t> patch;     // push 1; pop reg     (6A 01 58 / 6A 01 5A)
    };

    // (vaddr, оригинал, патч) — синхронизировано с PATCHES в .py.
    //   8B 40 2C = mov eax,[rax+0x2c]  -> 6A 01 58 = push 1; pop rax
    //   8B 50 2C = mov edx,[rax+0x2c]  -> 6A 01 5A = push 1; pop rdx
    static const std::vector<Site>& table() {
        static const std::vector<Site> t = {
            { 0x620e1a9, { 0x8b, 0x40, 0x2c }, { 0x6a, 0x01, 0x58 } },
            { 0x6518326, { 0x8b, 0x40, 0x2c }, { 0x6a, 0x01, 0x58 } },
            { 0x6895447, { 0x8b, 0x50, 0x2c }, { 0x6a, 0x01, 0x5a } },
        };
        return t;
    }

    uintptr_t libBase = 0;   // база загрузки libclient.so (ELF vaddr 0)
    bool scanned = false;
    bool verified = false;   // libBase известна и все сайты читаются как orig или patch

    // Прочитать текущие 3 байта сайта по его vaddr.
    bool readSite(const ProcessMemory& mem, const Site& s, std::vector<uint8_t>& out) const {
        return mem.readBytes(libBase + s.vaddr, s.orig.size(), out);
    }

public:
    bool isFound() const { return verified; }

    // Патч включён, если ПЕРВЫЙ сайт сейчас читается как push 1 (0x6A).
    bool isEnabled(const ProcessMemory& mem) const {
        if (!verified) return false;
        std::vector<uint8_t> buf;
        return readSite(mem, table().front(), buf) && !buf.empty() && buf[0] == 0x6A;
    }

    // libBase — наименьший mapped-адрес libclient.so (ELF vaddr 0), как
    // libclient_base() в .py. Сверяем все сайты с ожидаемыми байтами.
    void scan(const ProcessMemory& mem, uintptr_t base) {
        if (scanned) return;
        scanned = true;
        libBase = base;
        if (libBase == 0) {
            std::cout << RED << "[!] Dota Plus: libclient base unknown — cannot locate sites." << RESET << "\n";
            return;
        }

        // .text мог ещё домапливаться (как retry-цикл в .py). Сайт валиден, если
        // читается как оригинал ЛИБО как наш патч (уже включён в этой сессии).
        bool allOk = false;
        for (int attempt = 0; attempt < 8 && !allOk; ++attempt) {
            allOk = true;
            for (const auto& s : table()) {
                std::vector<uint8_t> cur;
                if (!readSite(mem, s, cur) || (cur != s.orig && cur != s.patch)) {
                    allOk = false;
                    break;
                }
            }
            if (!allOk) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        verified = allOk;

        if (verified)
            std::cout << CYAN << "[*] Dota Plus: " << table().size()
                      << " status-read sites verified (applied at launch)." << RESET << "\n";
        else
            std::cout << YELLOW << "[!] Dota Plus: status-read sites don't match "
                      << "(game updated? re-find vaddr like in eternal_plus_extern.py)." << RESET << "\n";
    }

    // Запись под заморозкой процесса (torn-write по живому коду опасен). Патчим
    // ТОЛЬКО сайты, что сейчас в оригинале; уже-патченные пропускаем.
    void enable(const ProcessMemory& mem) {
        if (!verified || isEnabled(mem)) return;
        mem.freeze();
        int n = 0;
        for (const auto& s : table()) {
            std::vector<uint8_t> cur;
            if (!readSite(mem, s, cur)) continue;
            if (cur == s.patch) { n++; continue; }                  // уже стоит
            if (cur != s.orig) continue;                            // чужие байты — не трогаем
            if (mem.writeBytes(libBase + s.vaddr, s.patch)) n++;
        }
        mem.unfreeze();
        std::cout << GREEN << "[+] Dota Plus ON (client-side) — status reads forced to Active at launch ("
                  << n << "/" << table().size() << " sites)!" << RESET << "\n";
    }
    // Тоггла «выключить» в игре нет: статус читается одноразово на логине, поэтому
    // править его в уже идущей игре бессмысленно. Plus включается только при запуске.
};

void printWeatherList() {
    struct WeatherItem {
        std::string color;
        std::string icon; // глиф Nerd Font
        char key;
        std::string name;
    };

    // Иконки заданы кодпоинтами (\u / \U), чтобы не зависеть от вставки глифа.
    static const std::vector<WeatherItem> items = {
        { WHITE,  "\uF2DC",     '1', "Snow"       },
        { BLUE,   "\uE318",     '2', "Rain"       },
        { PURPLE, "\uF4EE",     '3', "Moonbeam"   },
        { GREEN,  "\uF043",     '4', "Pestilence" },
        { ORANGE, "\U000F0E69", '5', "Harvest"    },
        { YELLOW, "\uE37A",     '6', "Sirocco"    },
        { PINK,   "\U000F09F2", '7', "Spring"     },
        { RED,    "\uEF2E",     '8', "Ash"        },
        { CYAN,   "\U000F078D", '9', "Aurora"     },
        { GRAY,   "•",     '0', "Default"    },
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

// ─────────────────────────────────────────────────────────────────────────────
//  Запуск Dota 2 через Steam и ожидание старта процесса.
//  Возвращает PID или 0 при таймауте (60 секунд).
// ─────────────────────────────────────────────────────────────────────────────
// Сбросить root-привилегии до пользователя, вызвавшего sudo, и запустить Steam-URI.
// Вызывается ТОЛЬКО в дочернем процессе и завершается exec/_exit.
// Steam отказывается работать от root, поэтому уходим под SUDO_UID/SUDO_GID и
// восстанавливаем пользовательское окружение (HOME, XDG_RUNTIME_DIR), чтобы Steam
// нашёл свой сокет уже запущенного инстанса.
static void dropPrivAndLaunchSteam() {
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

    // PATH-поиск (execlp) — на разных дистрибутивах бинарь лежит по-разному.
    execlp("xdg-open", "xdg-open", "steam://run/570", static_cast<char*>(nullptr));
    execlp("steam",    "steam",    "steam://run/570", static_cast<char*>(nullptr));
    _exit(127);
}

static pid_t launchAndWaitForDota() {
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
        auto pids = findProcessesByName("dota2");
        if (!pids.empty()) {
            std::cout << "\n";
            return pids[0];
        }
        if (i % 4 == 0) { std::cout << "."; std::cout.flush(); }
    }
    std::cout << "\n" << RED << "[!] Timeout waiting for Dota 2." << RESET << "\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Ожидание загрузки libclient.so в карту памяти процесса.
//  Нужно для код-патчей (партиклы): патчить можно только после того, как .so отмаплен.
// ─────────────────────────────────────────────────────────────────────────────
static bool waitForClientLib(pid_t pid, int timeoutSec = 60) {
    std::cout << "[*] Waiting for libclient.so";
    std::cout.flush();
    for (int i = 0; i < timeoutSec * 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
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

// Какие «включил и забыл» код-патчи активировать сразу после запуска игры.
// Камера и погода интерактивны (нужны значения/калибровка) — их тут нет.
struct LaunchSelection {
    bool particles = false;
    bool dotaPlus  = false;  // Dota Plus (client-side): ставить ДО GC-логина, чтобы welcome/UI открылись
};

// ─────────────────────────────────────────────────────────────────────────────
//  Экстернал-цикл: основной UI для работы с запущенной Dota.
//    sel          — что применить сразу (только если launchedByUs).
//    launchedByUs — true, если Доту запустили мы → можно сразу включить выбранные
//                   на экране запуска моды.
// ─────────────────────────────────────────────────────────────────────────────
static void runExternalMode(pid_t dotaPid, const LaunchSelection& sel, bool launchedByUs) {
    ProcessMemory mem;
    if (!mem.attach(dotaPid)) {
        std::cerr << RED << "[!] Failed to open process memory (PID " << dotaPid << ")." << RESET << "\n";
        return;
    }

    std::cout << "[*] Parsing memory maps...\n";
    auto regions = mem.getMemoryRegions();
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

    constexpr float DEFAULT_CAM_DISTANCE = 1200.0f;

    WeatherManager weatherMgr;
    ParticlesManager particlesMgr;
    PlusManager plusMgr;
    uintptr_t cameraAddr = 0;
    bool running = true;
    float currentCamDist = DEFAULT_CAM_DISTANCE;

    auto linkCameraAt = [&](float dist) -> bool {
        cameraAddr = 0;
        for (const auto& region : dataRegions) {
            cameraAddr = scanForCameraAddress(mem, region, dist);
            if (cameraAddr != 0) break;
        }
        if (cameraAddr != 0) {
            currentCamDist = dist;
            weatherMgr.persistCamera(cameraAddr);
        }
        return cameraAddr != 0;
    };

    auto promptAndSetDistance = [&]() {
        std::cout << "Enter desired distance (e.g. 1400, 1350): " << CYAN;
        float val;
        const bool ok = static_cast<bool>(std::cin >> val);
        std::cout << RESET;
        if (!ok) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << RED << "Bad input." << RESET << "\n";
            return;
        }
        if (mem.write(cameraAddr, val)) {
            currentCamDist = val;
            std::cout << GREEN << "[+] Done! Camera distance = " << val << RESET << "\n";
        } else {
            std::cout << RED << "Write failed." << RESET << "\n";
        }
    };

    weatherMgr.scan(mem, clientRegions);
    particlesMgr.scan(mem, clientRegions);
    plusMgr.scan(mem, libBase); // Dota Plus патчит фиксированные vaddr = libBase + offset

    // Восстановление адреса камеры из файла состояния той же сессии Dota
    if (const uintptr_t rc = weatherMgr.recoveredCameraAddr()) {
        float dist = 0.0f;
        if (mem.read(rc, dist) && dist > 100.0f && dist < 10000.0f) {
            cameraAddr = rc;
            currentCamDist = dist;
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
    // Dota Plus (client-side) патчит ЧТЕНИЕ статуса подписки (метод из
    // eternal_plus_extern.py) — это нужно поставить ДО GC-логина, чтобы welcome/UI
    // открылись уже как Plus. Работает ТОЛЬКО при запуске, поэтому применяем прямо
    // здесь, сразу после старта игры (мы пришли с экрана запуска).
    if (launchedByUs && sel.dotaPlus) {
        if (plusMgr.isFound() && !plusMgr.isEnabled(mem))
            plusMgr.enable(mem);
        else if (!plusMgr.isFound())
            std::cout << YELLOW << "[!] Dota Plus selected, but status-read sites not verified "
                      << "(game updated? re-find vaddr as in eternal_plus_extern.py)." << RESET << "\n";
    }

    std::cout << "\nPress Enter to continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    while (running) {
        printBanner();

        const bool weatherNeedsCalib = !weatherMgr.isLocked() && weatherMgr.hasCandidates();
        const std::string divider = "  " + GRAY + "──────────────────────────────────────────────" + RESET + "\n";

        // ── Статус ──
        std::cout << "  " << GREEN << "●" << RESET << " Dota 2   " << GRAY << ":" << RESET << " "
                  << GREEN << "running" << RESET << GRAY << "  PID " << dotaPid << RESET << "\n";

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
        std::cout << "\n";

        std::cout << "  ";
        if (!particlesMgr.isFound())
            std::cout << RED << "●" << RESET << " Particles" << GRAY << ":" << RESET << " "
                      << RED << "not found" << RESET;
        else if (particlesMgr.isEnabled(mem))
            std::cout << GREEN << "●" << RESET << " Particles" << GRAY << ":" << RESET << " "
                      << GREEN << "fog reveal ON" << RESET;
        else
            std::cout << YELLOW << "●" << RESET << " Particles" << GRAY << ":" << RESET << " "
                      << GRAY << "default" << RESET;
        std::cout << "\n";

        std::cout << "  ";
        if (!plusMgr.isFound())
            std::cout << RED << "●" << RESET << " Dota Plus" << GRAY << ":" << RESET << " "
                      << RED << "not found" << RESET;
        else if (plusMgr.isEnabled(mem))
            std::cout << GREEN << "●" << RESET << " Dota Plus" << GRAY << ":" << RESET << " "
                      << GREEN << "ON (client-side, launch patch)" << RESET;
        else
            std::cout << YELLOW << "●" << RESET << " Dota Plus" << GRAY << ":" << RESET << " "
                      << GRAY << "off (enable at launch)" << RESET;
        std::cout << "\n\n";

        // ── Главные действия ──
        std::cout << divider;
        std::cout << "   " << BOLD << CYAN << "[1]" << RESET << "  Camera distance\n";
        std::cout << "   " << BOLD << CYAN << "[2]" << RESET << "  Change weather\n";
        std::cout << "   ";
        if (particlesMgr.isFound())
            std::cout << BOLD << CYAN << "[3]" << RESET << "  Particles fog-reveal "
                      << GRAY << "(toggle)" << RESET << "\n";
        else
            std::cout << GRAY << "[3]  Particles fog-reveal (gates not found)" << RESET << "\n";
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
        std::cout << divider;
        std::cout << "\n   " << CYAN << "❯ " << RESET;

        std::string cmd;
        if (!(std::cin >> cmd)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        int choice;
        if (cmd == "t" || cmd == "T")      choice = 3;
        else if (cmd == "c" || cmd == "C") choice = 4;
        else if (cmd == "0")               choice = 0;
        else if (cmd == "1")               choice = 1;
        else if (cmd == "2")               choice = 2;
        else if (cmd == "3")               choice = 5;
        else                               continue;

        switch (choice) {
            case 0: running = false; break;

            case 1: {
                if (cameraAddr == 0) {
                    std::cout << YELLOW << "Linking camera (default " << DEFAULT_CAM_DISTANCE
                              << ")..." << RESET << "\n";
                    linkCameraAt(DEFAULT_CAM_DISTANCE);
                }
                if (cameraAddr != 0) {
                    promptAndSetDistance();
                } else {
                    std::cout << RED << "[-] Camera not at default " << DEFAULT_CAM_DISTANCE
                              << ".\n    If it's already zoomed out — press [c] and enter the current distance."
                              << RESET << "\n";
                }
                std::cout << "\nPress Enter to return...";
                std::cin.ignore(); std::cin.get();
                break;
            }

            case 4: {
                std::cout << "\nEnter CURRENT camera distance: " << CYAN;
                float scanVal;
                const bool ok = static_cast<bool>(std::cin >> scanVal);
                std::cout << RESET;
                if (!ok) {
                    std::cin.clear();
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    std::cout << RED << "Bad input." << RESET << "\n";
                } else {
                    std::cout << YELLOW << "Scanning for " << scanVal << "..." << RESET << "\n";
                    if (linkCameraAt(scanVal)) {
                        std::cout << GREEN << "[+] Camera linked at 0x" << std::hex << cameraAddr
                                  << std::dec << RESET << "\n";
                        promptAndSetDistance();
                    } else {
                        std::cout << RED << "[-] Not found. Move the camera and enter the exact value."
                                  << RESET << "\n";
                    }
                }
                std::cout << "\nPress Enter to return...";
                std::cin.ignore(); std::cin.get();
                break;
            }

            case 2: {
                printWeatherList();
                std::cout << "\n" << CYAN << "Select ID: " << RESET;
                int id; std::cin >> id;
                weatherMgr.applyWeather(mem, id);
                std::cout << "\nPress Enter to return...";
                std::cin.ignore(); std::cin.get();
                break;
            }

            case 3: {
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
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

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
                std::cout << "\nPress Enter to return...";
                std::cin.ignore(); std::cin.get();
                break;
            }

            case 5: {
                particlesMgr.toggle(mem);
                std::cout << "\nPress Enter to return...";
                std::cin.ignore(); std::cin.get();
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Экран запуска: Dota 2 ещё не запущена.
//  Пользователь тогглит, какие моды включить при запуске, затем жмёт [L].
//  Возвращает:
//    > 0  — PID запущенной Dota; sel = что включить сразу после старта
//    0    — пользователь вышел
// ─────────────────────────────────────────────────────────────────────────────
static pid_t runLaunchScreen(LaunchSelection& sel) {
    sel = LaunchSelection{}; // по умолчанию ничего не включено — «может не быть вайба»
    while (true) {
        printBanner();
        const std::string divider = "  " + GRAY + "──────────────────────────────────────────────" + RESET + "\n";

        std::cout << "  " << RED << "●" << RESET << " Dota 2   " << GRAY << ":" << RESET << " "
                  << RED << "not running" << RESET << "\n\n";

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

        std::cout << divider;
        std::cout << "   " << BOLD << GREEN << "[L]" << RESET << "  Launch Dota 2"
                  << GRAY << "  (with selected mods)" << RESET << "\n";
        std::cout << "   " << GRAY << "[d]/[p] toggle mod   ·   [0] exit" << RESET << "\n";
        std::cout << divider;
        std::cout << "\n   " << CYAN << "❯ " << RESET;

        std::string cmd;
        if (!(std::cin >> cmd)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        if (cmd == "0") return 0;
        if (cmd == "d" || cmd == "D") { sel.dotaPlus  = !sel.dotaPlus;  continue; }
        if (cmd == "p" || cmd == "P") { sel.particles = !sel.particles; continue; }

        if (cmd == "L" || cmd == "l") {
            pid_t pid = launchAndWaitForDota();
            if (pid == 0) {
                std::cout << RED << "[!] Failed to detect Dota 2. Try again or launch manually.\n" << RESET;
                std::cout << "\nPress Enter to return...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cin.get();
                continue;
            }
            // Код-патч требует загруженной libclient.so. Если выбран мод —
            // дождёмся её. Для Dota Plus патчим как можно РАНЬШE (до GC-логина),
            // поэтому init-паузу держим короткой.
            if (sel.particles || sel.dotaPlus) {
                if (!waitForClientLib(pid)) {
                    std::cout << YELLOW << "[!] libclient.so not loaded — mods can be enabled manually from the menu.\n" << RESET;
                    sel = LaunchSelection{};
                } else {
                    // Короткая пауза: .so отмаплена, даём ей чуть проинициализироваться,
                    // но успеваем ДО обработки статуса подписки от GC.
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

int main(int, char**) {
    printBanner();

    if (geteuid() != 0) {
        std::cerr << RED << "[!] ROOT REQUIRED. Run with sudo." << RESET << "\n";
        return 1;
    }

    auto pids = findProcessesByName("dota2");

    pid_t dotaPid = 0;
    LaunchSelection sel;
    bool launchedByUs = false;

    if (pids.empty()) {
        // Dota не запущена — экран выбора модов + запуск
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
