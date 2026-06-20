#include "plus_manager.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include "ansi.hpp"

using namespace ansi;

const std::vector<PlusManager::Site>& PlusManager::table() {
    //   8B 40 2C = mov eax,[rax+0x2c]  -> 6A 01 58 = push 1; pop rax
    //   8B 50 2C = mov edx,[rax+0x2c]  -> 6A 01 5A = push 1; pop rdx
    static const std::vector<Site> t = {
        { 0x62107e9, { 0x8b, 0x40, 0x2c }, { 0x6a, 0x01, 0x58 } },
        { 0x651a926, { 0x8b, 0x40, 0x2c }, { 0x6a, 0x01, 0x58 } },
        { 0x6897a47, { 0x8b, 0x50, 0x2c }, { 0x6a, 0x01, 0x5a } },
    };
    return t;
}

bool PlusManager::readSite(const ProcessMemory& mem, const Site& s, std::vector<uint8_t>& out) const {
    return mem.readBytes(libBase_ + s.vaddr, s.orig.size(), out);
}

bool PlusManager::isEnabled(const ProcessMemory& mem) const {
    if (!verified_) return false;
    std::vector<uint8_t> buf;
    return readSite(mem, table().front(), buf) && !buf.empty() && buf[0] == 0x6A;
}

void PlusManager::scan(const ProcessMemory& mem, uintptr_t base) {
    if (scanned_) return;
    scanned_ = true;
    libBase_ = base;
    if (libBase_ == 0) {
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
    verified_ = allOk;

    if (verified_)
        std::cout << CYAN << "[*] Dota Plus: " << table().size()
                  << " status-read sites verified (applied at launch)." << RESET << "\n";
    else
        std::cout << YELLOW << "[!] Dota Plus: status-read sites don't match "
                  << "(game updated? re-find vaddr like in eternal_plus_extern.py)." << RESET << "\n";
}

void PlusManager::enable(const ProcessMemory& mem) {
    if (!verified_ || isEnabled(mem)) return;
    mem.freeze();
    int n = 0;
    for (const auto& s : table()) {
        std::vector<uint8_t> cur;
        if (!readSite(mem, s, cur)) continue;
        if (cur == s.patch) { n++; continue; }                  // уже стоит
        if (cur != s.orig) continue;                            // чужие байты — не трогаем
        if (mem.writeBytes(libBase_ + s.vaddr, s.patch)) n++;
    }
    mem.unfreeze();
    std::cout << GREEN << "[+] Dota Plus ON (client-side) — status reads forced to Active at launch ("
              << n << "/" << table().size() << " sites)!" << RESET << "\n";
}
