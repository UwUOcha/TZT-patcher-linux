#include "river_manager.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

#include "ansi.hpp"

using namespace ansi;

const std::string& RiverManager::signature() {
    // movslq ?? ??(%rsi),%rdx ; lea table(%rip),%rax ; mov (%rax,%rdx,8),%rsi
    // disp32 movslq'а wildcard-нут (низкие 2 байта), высокие держим 00 00 → оффсет
    // маленький; хвост lea+индексная загрузка в %rsi делает сигнатуру уникальной.
    static const std::string s =
        "48 63 96 ?? ?? 00 00 48 8d 05 ?? ?? ?? ?? 48 8b 34 d0";
    return s;
}

std::vector<uint8_t> RiverManager::makePatch(int riverId) {
    std::vector<uint8_t> p = { 0xBA, 0, 0, 0, 0, 0x90, 0x90 };
    p[1] = static_cast<uint8_t>(riverId & 0xff);
    return p;
}

bool RiverManager::readSite(const ProcessMemory& mem, std::vector<uint8_t>& out) const {
    return mem.readBytes(siteAddr_, SITE_LEN, out);
}

void RiverManager::saveState() const {
    if (siteAddr_ == 0 || orig_.size() != SITE_LEN) return;
    std::ofstream f(STATE_PATH, std::ios::trunc);
    if (!f) return;
    f << "base " << std::hex << moduleBase_ << "\n";
    f << "site " << std::hex << siteAddr_ << " ";
    for (const uint8_t b : orig_)
        f << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    f << "\n";
}

RiverManager::State RiverManager::loadState() const {
    State st;
    std::ifstream f(STATE_PATH);
    if (!f) return st;
    std::string tok;
    while (f >> tok) {
        if (tok == "base") f >> std::hex >> st.base;
        else if (tok == "site") {
            std::string hex;
            f >> std::hex >> st.addr >> hex;
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                st.orig.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        }
    }
    st.valid = (st.base != 0 && st.addr != 0 && st.orig.size() == SITE_LEN);
    return st;
}

int RiverManager::currentId(const ProcessMemory& mem) const {
    if (!verified_) return -1;
    std::vector<uint8_t> cur;
    if (!readSite(mem, cur) || cur.size() < SITE_LEN) return -1;
    if (cur == orig_) return 0;
    if (cur[0] == 0xBA) return cur[1];
    return -1;
}

void RiverManager::scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
    if (scanned_) return;
    scanned_ = true;
    if (!regions.empty()) moduleBase_ = regions.front().start;

    // Та же сессия Доты → подхватить сайт из файла (код мог быть уже патчен, тогда
    // сигнатура movslq не найдётся, а адрес+оригинал остаются валидны).
    const State st = loadState();
    if (st.valid && st.base == moduleBase_) {
        siteAddr_ = st.addr;
        orig_ = st.orig;
        verified_ = true;
        std::cout << GREEN << "[+] River: dispatch site recovered from state (libclient+0x"
                  << std::hex << (siteAddr_ - moduleBase_) << std::dec << ")." << RESET << "\n";
        return;
    }

    // .text мог ещё домапливаться на раннем старте — сканируем с ретраями.
    const std::vector<int> sig = parsePattern(signature());
    for (int attempt = 0; attempt < 8 && !verified_; ++attempt) {
        for (const auto& region : regions) {
            const auto hits = findAllPatterns(mem, region, sig);
            if (hits.empty()) continue;
            siteAddr_ = hits.front();
            if (mem.readBytes(siteAddr_, SITE_LEN, orig_) && orig_.size() == SITE_LEN) {
                verified_ = true;
                break;
            }
        }
        if (!verified_) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (verified_) {
        saveState();
        std::cout << CYAN << "[*] River: dispatch site located at libclient+0x"
                  << std::hex << (siteAddr_ - moduleBase_) << std::dec << " — ready." << RESET << "\n";
    } else {
        std::cout << YELLOW << "[!] River: dispatch signature not found "
                  << "(game updated a lot? refresh the movslq+lea+mov signature from Binary Ninja)."
                  << RESET << "\n";
    }
}

void RiverManager::applyRiver(const ProcessMemory& mem, int riverId) {
    if (!verified_) {
        std::cout << RED << "[!] River site not located — cannot patch." << RESET << "\n";
        return;
    }
    if (riverId < 0 || riverId >= RIVER_COUNT) {
        std::cout << RED << "[!] River id out of range (0.." << (RIVER_COUNT - 1) << ")." << RESET << "\n";
        return;
    }

    std::vector<uint8_t> cur;
    if (!readSite(mem, cur) || cur.size() < SITE_LEN) {
        std::cout << RED << "[!] Could not read river site." << RESET << "\n";
        return;
    }
    if (cur != orig_ && cur[0] != 0xBA) {
        std::cout << RED << "[!] River site holds unexpected bytes — refusing to patch." << RESET << "\n";
        return;
    }

    const std::vector<uint8_t> bytes = (riverId == 0) ? orig_ : makePatch(riverId);
    mem.freeze();
    const bool wrote = mem.writeBytes(siteAddr_, bytes);
    mem.unfreeze();

    if (!wrote) {
        std::cout << RED << "[!] Write failed." << RESET << "\n";
        return;
    }
    if (riverId == 0)
        std::cout << GREEN << "[+] River reset to Default (original code restored)." << RESET << "\n";
    else
        std::cout << GREEN << "[+] River set to id " << riverId << " (water material forced)." << RESET << "\n";
}
