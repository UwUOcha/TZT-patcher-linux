#include "weather_manager.hpp"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

#include "ansi.hpp"

using namespace ansi;

const std::string& WeatherManager::applyAnchor() {
    // movslq off(%reg),%rax ; mov edx,<N> ; cmp edx,eax ; cmovg rax,rdx ;
    // xor edx,edx ; test eax,eax ; cmovs rax,rdx ; lea table(%rip),%rdx
    // modrm и low16 disp32 у movslq wildcard-нуты (база/оффсет), high16 = 00 00;
    // число погод <N> тоже wildcard (переживём добавление типов). Уникален в .text.
    static const std::string s =
        "48 63 ?? ?? ?? 00 00 BA ?? 00 00 00 39 D0 48 0F 4F C2 31 D2 85 C0 48 0F 48 C2 48 8D 15";
    return s;
}

const std::vector<WeatherManager::ReadForm>& WeatherManager::readForms() {
    // Инструкции чтения поля погоды, которые умеем форсить в константу.
    // Длина = prefix + modrm(1) + disp32(4).
    static const std::vector<ReadForm> f = {
        { "movsxd", { 0x48, 0x63 } }, // movsxd r64,[base+disp32]
        { "mov",    { 0x8B }       }, // mov    r32,[base+disp32]
        { "movzbl", { 0x0F, 0xB6 } }, // movzbl r32,[base+disp32]
    };
    return f;
}

std::vector<uint8_t> WeatherManager::makePatch(int weatherId, size_t size, uint8_t movOpcode) {
    std::vector<uint8_t> p(5, 0);
    p[0] = movOpcode;                 // mov <dest>, imm32  (0xB8 + destReg)
    memcpy(&p[1], &weatherId, sizeof(int));
    while (p.size() < size) p.push_back(0x90);
    return p;
}

// ── Автопоиск: оффсет типа погоды по якорю ─────────────────────────────────────
uint32_t WeatherManager::findWeatherOffset(const ProcessMemory& mem,
                                           const std::vector<MemoryRegion>& regions) const {
    const std::vector<int> sig = parsePattern(applyAnchor());
    for (const auto& region : regions) {
        for (const auto addr : findAllPatterns(mem, region, sig)) {
            uint32_t disp = 0;
            if (!mem.read(addr + ANCHOR_DISP_POS, disp)) continue;
            if (disp >= MIN_OFFSET && disp <= MAX_OFFSET) return disp;
        }
    }
    return 0;
}

// ── Захват всех патч-сайтов чтения поля погоды по известному оффсету ───────────
std::vector<WeatherManager::PatchSite> WeatherManager::captureFieldSites(
    const ProcessMemory& mem, const std::vector<MemoryRegion>& regions, uint32_t offset) const {

    std::vector<PatchSite> out;
    if (offset > 0xFFFF) return out; // держим disp32 с нулевыми старшими байтами

    for (const auto& form : readForms()) {
        // sig = prefix + wildcard(modrm) + disp32(low16 offset, high16 = 00 00)
        std::vector<int> sig(form.prefix.begin(), form.prefix.end());
        sig.push_back(-1); // modrm
        sig.push_back(offset & 0xFF);
        sig.push_back((offset >> 8) & 0xFF);
        sig.push_back(0x00);
        sig.push_back(0x00);
        const size_t modrmIdx = form.prefix.size();
        const size_t size = form.prefix.size() + 1 + 4;

        for (const auto& region : regions) {
            for (const auto addr : findAllPatterns(mem, region, sig)) {
                uint8_t modrm = 0;
                if (!mem.read(addr + modrmIdx, modrm)) continue;
                if ((modrm & 0xC0) != 0x80) continue;       // нужен mod=10 (disp32)
                const uint8_t rm = modrm & 0x07;
                if (rm == 4 || rm == 5) continue;           // без SIB / rip-relative
                const uint8_t destReg = (modrm >> 3) & 0x07;

                PatchSite s{ addr, size, {}, static_cast<uint8_t>(0xB8 + destReg) };
                if (mem.readBytes(addr, size, s.original))
                    out.push_back(std::move(s));
            }
        }
    }
    return out;
}

void WeatherManager::saveState() const {
    std::ofstream f(STATE_PATH, std::ios::trunc);
    if (!f) return;
    f << "base " << std::hex << moduleBase_ << "\n";
    f << "offset " << std::hex << resolvedOffset_ << "\n";
    for (const auto& s : sites_) {
        f << "site " << std::hex << s.address << " " << std::dec << s.size << " "
          << std::hex << static_cast<int>(s.movOpcode) << " ";
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
        else if (tok == "site") {
            PatchSite s; std::string hex; int op = 0xB8;
            f >> std::hex >> s.address >> std::dec >> s.size >> std::hex >> op >> hex;
            s.movOpcode = static_cast<uint8_t>(op);
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                s.original.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            st.sites.push_back(std::move(s));
        }
    }
    st.valid = (st.base != 0 && !st.sites.empty());
    return st;
}

void WeatherManager::scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
    if (isScanned_) return;
    isScanned_ = true;

    if (!regions.empty()) moduleBase_ = regions.front().start;

    const State st = loadState();
    const bool sameSession = (st.base != 0 && st.base == moduleBase_);

    // Случай 0 (та же сессия, код уже мог быть затёрт нашим патчем): восстановить
    // оффсет и места патча из файла — сигнатуры искать не нужно.
    if (st.valid && sameSession) {
        resolvedOffset_ = st.offset;
        sites_ = st.sites;
        locked_ = true;
        std::cout << GREEN << "[+] Weather recovered from state — offset 0x" << std::hex << st.offset
                  << std::dec << " (" << sites_.size() << " sites), no Dota restart needed." << RESET << "\n";
        return;
    }

    std::cout << YELLOW << "[*] Auto-locating weather field via particle-table anchor..." << RESET << "\n";

    // ── ГЛАВНЫЙ ПУТЬ: автопоиск оффсета по якорю ──────────────────────────────
    const uint32_t off = findWeatherOffset(mem, regions);
    if (off != 0) {
        sites_ = captureFieldSites(mem, regions, off);
        if (!sites_.empty()) {
            resolvedOffset_ = off;
            locked_ = true;
            saveState();
            std::cout << GREEN << "[+] Weather auto-located: offset 0x" << std::hex << off << std::dec
                      << " (" << sites_.size() << " read sites) — ready."
                      << RESET << "\n";
            return;
        }
        std::cout << YELLOW << "[!] Weather anchor found offset 0x" << std::hex << off << std::dec
                  << " but no read sites were captured." << RESET << "\n";
        return;
    }

    std::cout << RED << "[!] Weather auto-location failed: particle-table anchor not found."
              << RESET << "\n";
}

void WeatherManager::applyWeather(const ProcessMemory& mem, int weatherId) {
    if (!locked_ || sites_.empty()) {
        std::cout << RED << "[!] Weather offset not resolved yet." << RESET << "\n";
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
            if (mem.writeBytes(s.address, makePatch(weatherId, s.size, s.movOpcode))) count++;
        std::cout << GREEN << "[+] Weather ID " << weatherId << " applied to " << count
                  << " locations (offset 0x" << std::hex << resolvedOffset_ << std::dec << ")." << RESET << "\n";
    }
}
