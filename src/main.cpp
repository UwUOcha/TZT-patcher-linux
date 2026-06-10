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
        std::cout << BLUE << "(Move your camera or reconnect to see changes for Sky)" << RESET << "\n";
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
    bool isCalibrated() const { return isFound(); } // отдельная калибровка не нужна
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

    constexpr float DEFAULT_CAM_DISTANCE = 1200.0f; // дефолтная дистанция камеры в Dota

    WeatherManager weatherMgr;
    ParticlesManager particlesMgr;
    uintptr_t cameraAddr = 0;
    bool running = true;
    float currentCamDist = DEFAULT_CAM_DISTANCE;

    // Привязать камеру: найти в памяти float рядом с указанной дистанцией.
    auto linkCameraAt = [&](float dist) -> bool {
        cameraAddr = 0;
        for (const auto& region : dataRegions) {
            cameraAddr = scanForCameraAddress(mem, region, dist);
            if (cameraAddr != 0) break;
        }
        if (cameraAddr != 0) {
            currentCamDist = dist;
            weatherMgr.persistCamera(cameraAddr); // запомнить адрес на будущие запуски
        }
        return cameraAddr != 0;
    };

    // Спросить желаемую дистанцию и записать её (камера уже должна быть привязана).
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

    // Восстановление дистанции камеры из прошлого запуска (та же сессия Dota):
    // если сохранённый адрес ещё жив и хранит правдоподобную дистанцию — цепляемся
    // сразу, чтобы [1] менял даже уже изменённую дистанцию без релинка.
    if (const uintptr_t rc = weatherMgr.recoveredCameraAddr()) {
        float dist = 0.0f;
        if (mem.read(rc, dist) && dist > 100.0f && dist < 10000.0f) {
            cameraAddr = rc;
            currentCamDist = dist;
            std::cout << GREEN << "[+] Camera linked from previous session — distance "
                      << dist << "." << RESET << "\n";
        }
    }

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

        // ── Второстепенные сервисные действия ──
        std::cout << "   " << GRAY << "[c] relink camera (if already zoomed)" << RESET << "\n";
        std::cout << "   ";
        if (weatherNeedsCalib)
            std::cout << BOLD << YELLOW << "[t]" << RESET << YELLOW << " Recalibrate weather" << RESET
                      << YELLOW << "  ← сделай это сначала" << RESET;
        else
            std::cout << GRAY << "[t] recalibrate weather" << RESET;
        std::cout << GRAY << "   ·   [0] exit" << RESET << "\n";
        std::cout << divider;
        std::cout << "\n   " << CYAN << "❯ " << RESET;

        // Команды — строкой, чтобы принимать и цифры, и буквы 't'/'c'.
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
        else if (cmd == "3")               choice = 5; // частицы — основное действие
        else                               continue; // неизвестная команда — перерисовать меню

        switch (choice) {
            case 0: {
                running = false;
                break;
            }
            case 1: {
                // Авто-подхват: если камера ещё не привязана, считаем, что она на
                // дефолте, и цепляемся без лишних вопросов.
                if (cameraAddr == 0) {
                    std::cout << YELLOW << "Linking camera (default " << DEFAULT_CAM_DISTANCE
                              << ")..." << RESET << "\n";
                    linkCameraAt(DEFAULT_CAM_DISTANCE);
                }

                if (cameraAddr != 0) {
                    promptAndSetDistance();
                } else {
                    std::cout << RED << "[-] Camera not at default " << DEFAULT_CAM_DISTANCE
                              << ".\n    Если она уже отдалена — нажми [c] и введи её текущую дистанцию."
                              << RESET << "\n";
                }

                std::cout << "\nPress Enter to return...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
            case 4: {
                // Релинк по текущей дистанции — для случая, когда камера уже была
                // отдалена, а программу перезапустили (дефолт 1200 уже не найдётся).
                std::cout << "\nВведи ТЕКУЩУЮ дистанцию камеры (что стоит сейчас в игре): " << CYAN;
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
                        std::cout << RED << "[-] Не найдено. Подвигай камеру в игре и введи точное значение."
                                  << RESET << "\n";
                    }
                }

                std::cout << "\nPress Enter to return...";
                std::cin.ignore();
                std::cin.get();
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
                        const int n = weatherMgr.applyAt(mem, off, TEST_WEATHER);
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
            case 5: {
                particlesMgr.toggle(mem);

                std::cout << "\nPress Enter to return...";
                std::cin.ignore();
                std::cin.get();
                break;
            }
        }
    }

    return 0;
}
