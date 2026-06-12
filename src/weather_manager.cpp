#include "weather_manager.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

#include "ansi.hpp"

using namespace ansi;

const std::vector<WeatherManager::PatternDef>& WeatherManager::patterns() {
    static const std::vector<PatternDef> p = {
        { "Particles", "48 63 83" }, // MOVSXD RAX, [RBX+offset]
        { "Lighting",  "8B 83"    }  // MOV   EAX, [RBX+offset]
    };
    return p;
}

std::vector<uint8_t> WeatherManager::makePatch(int weatherId, size_t size) {
    std::vector<uint8_t> p(5, 0);
    p[0] = 0xB8;
    memcpy(&p[1], &weatherId, sizeof(int));
    while (p.size() < size) p.push_back(0x90);
    return p;
}

std::vector<const WeatherManager::Hit*> WeatherManager::hitsForOffset(uint32_t offset) const {
    std::vector<const Hit*> out;
    for (const auto& h : allHits_)
        if (h.disp == offset) out.push_back(&h);
    return out;
}

std::vector<WeatherManager::PatchSite> WeatherManager::captureSites(const ProcessMemory& mem, uint32_t offset) const {
    std::vector<PatchSite> out;
    for (const Hit* h : hitsForOffset(offset)) {
        PatchSite s{ h->address, h->size, {} };
        if (mem.readBytes(h->address, h->size, s.original))
            out.push_back(std::move(s));
    }
    return out;
}

void WeatherManager::saveState() const {
    std::ofstream f(STATE_PATH, std::ios::trunc);
    if (!f) return;
    f << "base " << std::hex << moduleBase_ << "\n";
    f << "offset " << std::hex << resolvedOffset_ << "\n";
    f << "camera " << std::hex << persistedCamera_ << "\n";
    for (const auto& s : sites_) {
        f << "site " << std::hex << s.address << " " << std::dec << s.size << " ";
        for (const uint8_t b : s.original)
            f << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        f << "\n";
    }
}

WeatherManager::State WeatherManager::loadState() const {
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

void WeatherManager::persistCamera(uintptr_t addr) {
    persistedCamera_ = addr;
    saveState();
}

void WeatherManager::scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
    if (isScanned_) return;

    if (!regions.empty()) moduleBase_ = regions.front().start;

    std::cout << YELLOW << "[*] Scanning weather instructions (baseline 0x"
              << std::hex << HINT_OFFSET << std::dec << ")..." << RESET << "\n";

    // Маски с "дыркой" вместо 32-битного смещения:
    //   8B 83 ?? ?? 00 00       MOV    EAX, [RBX+offset]
    //   48 63 83 ?? ?? 00 00    MOVSXD RAX, [RBX+offset]
    // Старшие 2 байта смещения держим нулевыми => только небольшие оффсеты.
    struct WildPattern { std::string name; std::vector<int> sig; size_t prefixLen; };
    std::vector<WildPattern> wilds;
    for (const auto& pat : patterns()) {
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
                allHits_.push_back({ addr, w.name, disp, w.sig.size() });
            }
        }
    }

    isScanned_ = true;

    // Файл состояния читаем один раз — он нужен и для камеры, и для погоды.
    const State st = loadState();
    const bool sameSession = (st.base != 0 && st.base == moduleBase_);
    if (sameSession) {
        persistedCamera_ = st.cameraAddr; // не потерять при последующих перезаписях файла
        loadedCamera_    = st.cameraAddr; // отдать main для восстановления камеры
    }

    if (allHits_.empty()) {
        std::cout << RED << "[!] No candidate instructions found. Game changed too much." << RESET << "\n";
        return;
    }

    // Случай 1: сигнатуры целы (код не патчен) — известный оффсет на месте.
    if (!hitsForOffset(HINT_OFFSET).empty()) {
        resolvedOffset_ = HINT_OFFSET;
        sites_ = captureSites(mem, HINT_OFFSET);
        locked_ = true;
        saveState();
        std::cout << GREEN << "[+] Weather offset 0x" << std::hex << HINT_OFFSET << std::dec
                  << " is in place — ready to use." << RESET << "\n";
        return;
    }

    // Случай 2: сигнатур нет — код уже затёрт нашим патчем в этой же сессии Dota.
    if (st.valid && sameSession) {
        resolvedOffset_ = st.offset;
        sites_ = st.sites;
        locked_ = true;
        std::cout << GREEN << "[+] Recovered weather offset 0x" << std::hex << st.offset << std::dec
                  << " from previous session — no Dota restart needed." << RESET << "\n";
        return;
    }

    // Случай 3: ни сигнатур, ни валидного состояния — нужна рекалибровка.
    std::cout << YELLOW << "[!] Known offset 0x" << std::hex << HINT_OFFSET << std::dec
              << " not found — game was probably updated.\n"
              << "    Run [t] Recalibrate to find the new one." << RESET << "\n";
}

std::vector<uint32_t> WeatherManager::trialOffsets() const {
    std::vector<uint32_t> offs;
    for (const auto& h : allHits_)
        if (absDiff(h.disp, HINT_OFFSET) <= TRIAL_WINDOW)
            offs.push_back(h.disp);
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    std::sort(offs.begin(), offs.end(), [](uint32_t a, uint32_t b) {
        return absDiff(a, HINT_OFFSET) < absDiff(b, HINT_OFFSET);
    });
    return offs;
}

int WeatherManager::applyAt(const ProcessMemory& mem, uint32_t offset, int weatherId) {
    backup_.clear();
    int count = 0;
    for (const Hit* h : hitsForOffset(offset)) {
        PatchSite s{ h->address, h->size, {} };
        if (mem.readBytes(h->address, h->size, s.original))
            backup_.push_back(s);
        if (mem.writeBytes(h->address, makePatch(weatherId, h->size)))
            count++;
    }
    return count;
}

void WeatherManager::revert(const ProcessMemory& mem) {
    for (const auto& s : backup_)
        mem.writeBytes(s.address, s.original);
    backup_.clear();
}

void WeatherManager::lock(uint32_t offset) {
    resolvedOffset_ = offset;
    locked_ = true;
    sites_ = std::move(backup_);
    backup_.clear();
    saveState();
}

void WeatherManager::applyWeather(const ProcessMemory& mem, int weatherId) {
    if (!locked_ || sites_.empty()) {
        std::cout << RED << "[!] Offset not confirmed yet. Run [t] Recalibrate first." << RESET << "\n";
        return;
    }

    int count = 0;
    if (weatherId == 0) {
        for (const auto& s : sites_)
            if (mem.writeBytes(s.address, s.original)) count++;
        std::cout << GREEN << "[+] Default restored (original code) at " << count
                  << " locations." << RESET << "\n";
    } else {
        for (const auto& s : sites_)
            if (mem.writeBytes(s.address, makePatch(weatherId, s.size))) count++;
        std::cout << GREEN << "[+] Weather ID " << weatherId << " applied to " << count
                  << " locations (offset 0x" << std::hex << resolvedOffset_ << std::dec << ")." << RESET << "\n";
    }
}
