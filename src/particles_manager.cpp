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
    // Два FoW-хелпера видимости, которые дёргает per-frame particle-cull перед
    // SetRenderingEnabled(bool): большие эффекты → IsPointVisible(x,y,z), мелкие →
    // box-visibility(&p1,&p2). Оба читают один набор FoW-grid глобалов и возвращают
    // bool в eax. Форс обоих в `mov eax,1; ret` = каждая точка «видима» → эффект
    // рендерится сквозь туман. Сигнатуры = ВХОД каждой функции (адрес = патч-сайт);
    // уникальны в .text. Старый якорь-хеш 0x6b4ed927 Valve убрала — держим прологи.
    static const std::vector<std::string> s = {
        // IsPointVisible(x,y@xmm0, z@xmm1) -> bool
        "55 48 89 E5 48 83 EC 10 8B 35 ?? ?? ?? ?? 66 0F D6 45 F0 F3 0F 11 4D F8",
        // box-visibility(&p1, &p2) -> bool  (sibling, тот же FoW-grid набор)
        "44 8B 05 ?? ?? ?? ?? 45 85 C0 0F 84 ?? ?? ?? ?? F7 05 ?? ?? ?? ?? FF FF FF 7F",
    };
    return s;
}

void ParticlesManager::saveState() const {
    if (sites_.empty()) return;
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
    st.valid = (st.base != 0 && !st.sites.empty());
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

    // Патчим что нашли: оба хелпера дают полное вскрытие (крупные эффекты —
    // IsPointVisible, мелкие — box-visibility), но и один лучше нуля. Не гейтим
    // фичу на «найдены оба» — так одна протухшая сигнатура не убивает reveal.
    if (sites_.empty()) {
        std::cout << YELLOW << "[!] Particle FoW gates not found "
                  << "(game updated a lot? refresh the visibility-helper signatures)." << RESET << "\n";
        return;
    }
    saveState();
    if (sites_.size() == signatures().size())
        std::cout << CYAN << "[*] Particles: both FoW gates located."
                  << RESET << "\n";
    else
        std::cout << YELLOW << "[*] Particles: " << sites_.size() << "/" << signatures().size()
                  << " FoW gates located (some signatures stale — partial reveal)."
                  << RESET << "\n";
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
