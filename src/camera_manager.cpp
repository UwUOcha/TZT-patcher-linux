#include "camera_manager.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include "ansi.hpp"

using namespace ansi;

namespace {

const char CVAR_NAME[] = "dota_camera_distance";

constexpr size_t BACK_WINDOW = 96;  // назад от `lea rsi,[rip+name]` до `lea rax,[rip+convar]`
constexpr size_t FWD_LOAD    = 16;  // от `mov rax,[rip+convar+8]` до `movss xmm0,[rax+0x58]`
constexpr size_t FWD_STORE   = 24;  // от загрузки значения до записи в кеш

constexpr size_t MOVSS_LEN  = 8;    // F3 0F 11 05 <rel32>
constexpr size_t MOVLPS_LEN = 7;    // 0F 13 05 <rel32>

int32_t rel32(const std::vector<uint8_t>& b, size_t i) {
    int32_t v;
    memcpy(&v, &b[i], sizeof(v));
    return v;
}

bool eq(const std::vector<uint8_t>& b, size_t i, std::initializer_list<uint8_t> want) {
    if (i + want.size() > b.size()) return false;
    size_t k = 0;
    for (const uint8_t w : want)
        if (b[i + k++] != w) return false;
    return true;
}

bool contains(const std::vector<MemoryRegion>& regions, uintptr_t addr) {
    for (const auto& r : regions)
        if (addr >= r.start && addr < r.end) return true;
    return false;
}

} // namespace

std::vector<uint8_t> CameraManager::nopOfSize(size_t size) {
    // Рекомендованные Intel многобайтовые NOP: nopl 0x0(%rax) / nopl 0x0(%rax,%rax,1).
    if (size == MOVLPS_LEN) return { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 };
    if (size == MOVSS_LEN)  return { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return {};
}

void CameraManager::saveState() const {
    if (cacheAddr_ == 0 || sites_.empty()) return;
    std::ofstream f(STATE_PATH, std::ios::trunc);
    if (!f) return;
    f << "base " << std::hex << moduleBase_ << "\n";
    f << "cache " << std::hex << cacheAddr_ << "\n";
    f << "convar " << std::hex << convarAddr_ << "\n";
    for (const auto& s : sites_) {
        f << "site " << std::hex << s.addr << " ";
        for (const uint8_t b : s.original)
            f << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        f << "\n";
    }
}

CameraManager::State CameraManager::loadState() const {
    State st;
    std::ifstream f(STATE_PATH);
    if (!f) return st;
    std::string tok;
    while (f >> tok) {
        if (tok == "base")        f >> std::hex >> st.base;
        else if (tok == "cache")  f >> std::hex >> st.cache;
        else if (tok == "convar") f >> std::hex >> st.convar;
        else if (tok == "site") {
            Site s;
            std::string hex;
            f >> std::hex >> s.addr >> hex;
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                s.original.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            st.sites.push_back(std::move(s));
        }
    }
    st.valid = (st.base != 0 && st.cache != 0 && st.convar != 0 && !st.sites.empty());
    return st;
}

float CameraManager::distance(const ProcessMemory& mem) const {
    float v = 0.0f;
    if (cacheAddr_ != 0 && mem.read(cacheAddr_, v) && v >= MIN_SANE && v <= MAX_SANE)
        return v;
    // Кеш ещё не прогрет (заполняется при создании первой камеры) — показываем
    // значение конвара, которое туда и попадёт.
    float live = 0.0f;
    if (convarAddr_ != 0 && mem.read(convarAddr_, live)) return live;
    return v;
}

bool CameraManager::isForced(const ProcessMemory& mem) const {
    if (sites_.empty()) return false;
    for (const auto& s : sites_) {
        std::vector<uint8_t> cur;
        if (!mem.readBytes(s.addr, s.original.size(), cur)) return false;
        if (cur == s.original) return false;                   // хоть один сайт живой
    }
    return true;
}

int CameraManager::writeSites(const ProcessMemory& mem, bool patched) const {
    int n = 0;
    for (const auto& s : sites_) {
        const std::vector<uint8_t> want = patched ? nopOfSize(s.original.size()) : s.original;
        if (want.empty()) continue;
        std::vector<uint8_t> cur;
        if (!mem.readBytes(s.addr, want.size(), cur)) continue;
        if (cur == want) { n++; continue; }                    // уже в нужном состоянии
        // Пишем только если там ровно то, что мы ожидаем увидеть.
        if (cur != (patched ? s.original : nopOfSize(s.original.size()))) continue;
        if (mem.writeBytes(s.addr, want)) n++;
    }
    return n;
}

void CameraManager::scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& allRegions) {
    if (scanned_) return;
    scanned_ = true;

    // ── Классификация регионов модуля ────────────────────────────────────────
    std::vector<MemoryRegion> exec, readOnly, writable;
    uintptr_t moduleBase = 0, moduleEnd = 0;
    for (const auto& r : allRegions) {
        if (r.path.find("libclient.so") == std::string::npos) continue;
        if (moduleBase == 0 || r.start < moduleBase) moduleBase = r.start;
        moduleEnd = std::max(moduleEnd, r.end);
        if (r.perms.find('x') != std::string::npos) exec.push_back(r);
        else if (r.perms.compare(0, 2, "rw") == 0) writable.push_back(r);
        else if (r.perms[0] == 'r') readOnly.push_back(r);
    }
    // Хвост .bss не влезает в файловое отображение и ложится в АНОНИМНЫЙ rw-регион
    // сразу за модулем. Объект ConVar живёт именно там, поэтому регион добираем
    // по смежности (в /proc/pid/maps регионы идут по возрастанию адреса).
    for (const auto& r : allRegions)
        if (r.path.empty() && r.perms.compare(0, 2, "rw") == 0 && r.start == moduleEnd) {
            writable.push_back(r);
            moduleEnd = r.end;
        }

    if (exec.empty() || readOnly.empty() || writable.empty()) {
        std::cout << YELLOW << "[!] Camera: libclient.so mappings incomplete." << RESET << "\n";
        return;
    }
    moduleBase_ = moduleBase;

    // Та же сессия Dota: инструкции записи в кеш уже могли быть затёрты нашими
    // NOP'ами, и тогда сканом их не найти — берём адреса и оригиналы из файла.
    if (const State st = loadState(); st.valid && st.base == moduleBase_) {
        cacheAddr_ = st.cache;
        convarAddr_ = st.convar;
        sites_ = st.sites;
        std::cout << GREEN << "[+] Camera recovered from state (cache libclient+0x" << std::hex
                  << (cacheAddr_ - moduleBase_) << std::dec << ", " << sites_.size()
                  << " refresh sites) — distance " << distance(mem) << "." << RESET << "\n";
        return;
    }

    // ── 1. Строка имени конвара (вместе с терминатором, иначе матчится
    //       `dota_camera_distance_min`) ──────────────────────────────────────
    const std::string needle(CVAR_NAME, sizeof(CVAR_NAME)); // включая '\0'
    uintptr_t nameAddr = 0;
    std::vector<uint8_t> buf;
    for (const auto& r : readOnly) {
        if (!mem.readRegion(r, buf)) continue;
        const auto it = std::search(buf.begin(), buf.end(), needle.begin(), needle.end());
        if (it != buf.end()) {
            nameAddr = r.start + static_cast<size_t>(it - buf.begin());
            break;
        }
    }
    if (nameAddr == 0) {
        std::cout << YELLOW << "[!] Camera: convar name string not found in libclient." << RESET << "\n";
        return;
    }

    // Держим .text в памяти: по нему идут три независимых прохода.
    std::vector<std::vector<uint8_t>> code(exec.size());
    for (size_t i = 0; i < exec.size(); ++i)
        if (!mem.readRegion(exec[i], code[i])) code[i].clear();

    // ── 2. Единственный `lea rsi,[rip+name]` — сайт регистрации ConVar ───────
    size_t regRegion = 0, regOff = 0;
    int regHits = 0;
    for (size_t ri = 0; ri < code.size(); ++ri) {
        const auto& b = code[ri];
        for (size_t i = 0; i + 7 <= b.size(); ++i) {
            if (!eq(b, i, { 0x48, 0x8D, 0x35 })) continue;
            if (exec[ri].start + i + 7 + rel32(b, i + 3) != nameAddr) continue;
            if (++regHits == 1) { regRegion = ri; regOff = i; }
        }
    }
    if (regHits != 1) {
        std::cout << YELLOW << "[!] Camera: convar registration site is not unique ("
                  << regHits << " hits) — refusing to guess." << RESET << "\n";
        return;
    }

    // ── 3. Назад от него — `lea rax,[rip+convar]` в rw-память модуля ─────────
    uintptr_t convarObj = 0;
    {
        const auto& b = code[regRegion];
        for (size_t back = 1; back <= BACK_WINDOW && back <= regOff; ++back) {
            const size_t i = regOff - back;
            if (!eq(b, i, { 0x48, 0x8D, 0x05 })) continue;
            const uintptr_t t = exec[regRegion].start + i + 7 + rel32(b, i + 3);
            if (contains(writable, t)) { convarObj = t; break; }
        }
    }
    if (convarObj == 0) {
        std::cout << YELLOW << "[!] Camera: convar object not found near registration site."
                  << RESET << "\n";
        return;
    }

    // ── 4. Читатели значения → адрес кеша и места его перезаливки ────────────
    const uintptr_t dataPtrSlot = convarObj + CONVAR_DATA_PTR;
    std::map<uintptr_t, std::vector<Site>> byCache;
    for (size_t ri = 0; ri < code.size(); ++ri) {
        const auto& b = code[ri];
        for (size_t i = 0; i + 7 <= b.size(); ++i) {
            if (!eq(b, i, { 0x48, 0x8B, 0x05 })) continue;                 // mov rax,[rip+slot]
            if (exec[ri].start + i + 7 + rel32(b, i + 3) != dataPtrSlot) continue;

            size_t load = 0;
            for (size_t j = i + 7; j < i + 7 + FWD_LOAD && j + 5 <= b.size(); ++j)
                if (eq(b, j, { 0xF3, 0x0F, 0x10, 0x40, 0x58 })) { load = j; break; } // movss xmm0,[rax+0x58]
            if (load == 0) continue;

            for (size_t k = load + 5; k < load + 5 + FWD_STORE && k + MOVSS_LEN <= b.size(); ++k) {
                uintptr_t cache = 0;
                size_t len = 0;
                if (eq(b, k, { 0xF3, 0x0F, 0x11, 0x05 })) {                // movss [rip+G],xmm0
                    cache = exec[ri].start + k + MOVSS_LEN + rel32(b, k + 4);
                    len = MOVSS_LEN;
                } else if (eq(b, k, { 0x0F, 0x13, 0x05 })) {               // movlps [rip+G],xmm0
                    cache = exec[ri].start + k + MOVLPS_LEN + rel32(b, k + 3);
                    len = MOVLPS_LEN;
                } else {
                    continue;
                }
                Site s;
                s.addr = exec[ri].start + k;
                s.original.assign(b.begin() + static_cast<long>(k), b.begin() + static_cast<long>(k + len));
                byCache[cache].push_back(std::move(s));
                break;
            }
        }
    }

    uintptr_t best = 0;
    size_t bestVotes = 0;
    for (const auto& [addr, list] : byCache)
        if (list.size() > bestVotes) { best = addr; bestVotes = list.size(); }

    if (bestVotes < 2) {
        std::cout << YELLOW << "[!] Camera: settings cache not confirmed ("
                  << bestVotes << " agreeing sites, need 2) — refusing to guess."
                  << RESET << "\n";
        return;
    }
    if (!contains(writable, best)) {
        std::cout << YELLOW << "[!] Camera: resolved cache is outside writable module memory."
                  << RESET << "\n";
        return;
    }

    // ── 5. Значение ConVar + sanity ──────────────────────────────────────────
    uintptr_t dataPtr = 0;
    float cached = 0.0f, live = 0.0f;
    if (!mem.read(dataPtrSlot, dataPtr) || dataPtr == 0 ||
        !mem.read(dataPtr + CONVAR_VALUE, live) || !mem.read(best, cached)) {
        std::cout << YELLOW << "[!] Camera: convar data is not readable yet." << RESET << "\n";
        return;
    }
    // Кеш клиент заполняет ЛЕНИВО — при создании первой камеры. Сразу после старта
    // игры он ещё нулевой, и это НЕ повод считать резолв неверным: адрес подтверждён
    // четырьмя независимыми сайтами. Поэтому гейтим по значению конвара, а нулевой
    // кеш пропускаем — своё значение мы всё равно пишем туда сами.
    if (live < MIN_SANE || live > MAX_SANE ||
        (cached != 0.0f && (cached < MIN_SANE || cached > MAX_SANE))) {
        std::cout << YELLOW << "[!] Camera: resolved values look wrong (cache " << cached
                  << ", convar " << live << ") — refusing to use them." << RESET << "\n";
        return;
    }

    cacheAddr_ = best;
    convarAddr_ = dataPtr + CONVAR_VALUE;
    sites_ = std::move(byCache[best]);
    saveState();
    std::cout << CYAN << "[*] Camera: resolved via dota_camera_distance (cache libclient+0x"
              << std::hex << (best - moduleBase) << std::dec << ", " << sites_.size()
              << " refresh sites) — distance " << distance(mem) << "." << RESET << "\n";
}

bool CameraManager::setDistance(const ProcessMemory& mem, float value) {
    if (!isLinked()) return false;
    if (value < MIN_SANE || value > MAX_SANE) return false;

    // Сначала лочим кеш, иначе ближайший рефреш вернёт значение конвара.
    // Патч кода — под заморозкой, чтобы исключить torn write.
    mem.freeze();
    const int n = writeSites(mem, true);
    mem.unfreeze();
    if (n == 0) {
        std::cout << RED << "[!] Camera: could not lock the settings cache." << RESET << "\n";
        return false;
    }
    if (static_cast<size_t>(n) != sites_.size())
        std::cout << YELLOW << "[!] Camera: locked " << n << "/" << sites_.size()
                  << " refresh sites — value may still be reverted." << RESET << "\n";
    return mem.write(cacheAddr_, value);
}

bool CameraManager::restoreDefault(const ProcessMemory& mem) {
    if (!isLinked()) return false;
    mem.freeze();
    const int n = writeSites(mem, false);
    mem.unfreeze();

    // Вернуть в кеш то, что там было бы без нас — актуальное значение конвара.
    float live = 0.0f;
    if (mem.read(convarAddr_, live) && live >= MIN_SANE && live <= MAX_SANE)
        mem.write(cacheAddr_, live);
    return n > 0;
}
