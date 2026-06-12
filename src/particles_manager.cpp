#include "particles_manager.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>

#include "ansi.hpp"

using namespace ansi;

const std::vector<uint8_t>& ParticlesManager::patchBytes() {
    static const std::vector<uint8_t> p = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    return p;
}

const std::vector<std::string>& ParticlesManager::signatures() {
    static const std::vector<std::string> s = {
        // IsPointVisible(x@xmm0, y@xmm1, z@xmm2) -> bool
        "55 31 C9 31 F6 BA 27 D9 4E 6B 48 89 E5 53 48 89 FB",
        // box-visibility(&p1, &p2) -> bool
        "55 31 C9 48 89 E5 41 55 49 89 D5 BA 27 D9 4E 6B 41 54 49 89 F4",
    };
    return s;
}

void ParticlesManager::saveState() const {
    if (sites_.size() != signatures().size()) return;
    std::ofstream f(STATE_PATH, std::ios::trunc);
    if (!f) return;
    f << "base " << std::hex << moduleBase_ << "\n";
    for (const auto& s : sites_) {
        f << "site " << std::hex << s.addr << " ";
        for (const uint8_t b : s.original)
            f << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        f << "\n";
    }
}

ParticlesManager::State ParticlesManager::loadState() const {
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

bool ParticlesManager::isEnabled(const ProcessMemory& mem) const {
    if (!isFound()) return false;
    std::vector<uint8_t> buf;
    return mem.readBytes(sites_.front().addr, 1, buf) && buf[0] == 0xB8;
}

void ParticlesManager::scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
    if (scanned_) return;
    scanned_ = true;
    if (!regions.empty()) moduleBase_ = regions.front().start;

    // Та же сессия Доты → подхватить сайты из файла (код мог быть уже патчен).
    const State st = loadState();
    if (st.valid && st.base == moduleBase_) {
        sites_ = st.sites;
        std::cout << GREEN << "[+] Particles FoW gates recovered from state ("
                  << sites_.size() << " sites)." << RESET << "\n";
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
            sites_.push_back(std::move(s));
    }

    if (isFound()) {
        saveState();
        std::cout << CYAN << "[*] Particles: both FoW gates located — [p] to reveal fog particles."
                  << RESET << "\n";
    } else if (sites_.empty()) {
        std::cout << YELLOW << "[!] Particle FoW gates not found "
                  << "(game updated? refresh signatures from Binary Ninja)." << RESET << "\n";
    } else {
        // Нужны ОБА хелпера: мелкие эффекты идут через box-visibility, крупные —
        // через IsPointVisible. С одним патч не вскроет всё, поэтому отключаем.
        sites_.clear();
        std::cout << YELLOW << "[!] Only one particle gate found — need both, patch disabled."
                  << RESET << "\n";
    }
}

void ParticlesManager::toggle(const ProcessMemory& mem) {
    if (!isFound()) {
        std::cout << RED << "[!] Particle gates not located. Game may have updated." << RESET << "\n";
        return;
    }
    if (isEnabled(mem)) {
        int n = 0;
        for (const auto& s : sites_)
            if (mem.writeBytes(s.addr, s.original)) n++;
        std::cout << YELLOW << "[-] Particles fog-reveal OFF (restored " << n
                  << " sites)." << RESET << "\n";
    } else {
        int n = 0;
        for (const auto& s : sites_)
            if (mem.writeBytes(s.addr, patchBytes())) n++;
        std::cout << GREEN << "[+] Particles fog-reveal ON — enemy particles show through fog ("
                  << n << " sites)!" << RESET << "\n";
    }
}
