#include "river_manager.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include "ansi.hpp"

using namespace ansi;

const std::vector<uint8_t>& RiverManager::origBytes() {
    static const std::vector<uint8_t> o = { 0x48, 0x63, 0x96, 0xe0, 0x06, 0x00, 0x00 };
    return o;
}

std::vector<uint8_t> RiverManager::makePatch(int riverId) {
    std::vector<uint8_t> p = { 0xBA, 0, 0, 0, 0, 0x90, 0x90 };
    p[1] = static_cast<uint8_t>(riverId & 0xff);
    return p;
}

bool RiverManager::readSite(const ProcessMemory& mem, std::vector<uint8_t>& out) const {
    return mem.readBytes(libBase_ + SITE_VADDR, origBytes().size(), out);
}

int RiverManager::currentId(const ProcessMemory& mem) const {
    if (!verified_) return -1;
    std::vector<uint8_t> cur;
    if (!readSite(mem, cur) || cur.size() < origBytes().size()) return -1;
    if (cur == origBytes()) return 0;
    if (cur[0] == 0xBA) return cur[1];
    return -1;
}

void RiverManager::scan(const ProcessMemory& mem, uintptr_t base) {
    if (scanned_) return;
    scanned_ = true;
    libBase_ = base;
    if (libBase_ == 0) {
        std::cout << RED << "[!] River: libclient base unknown — cannot locate site." << RESET << "\n";
        return;
    }

    bool ok = false;
    for (int attempt = 0; attempt < 8 && !ok; ++attempt) {
        std::vector<uint8_t> cur;
        if (readSite(mem, cur) && (cur == origBytes() || (cur.size() == origBytes().size() && cur[0] == 0xBA)))
            ok = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    verified_ = ok;

    if (verified_)
        std::cout << CYAN << "[*] River: dispatch site verified at libclient+0x"
                  << std::hex << SITE_VADDR << std::dec << " — ready." << RESET << "\n";
    else
        std::cout << YELLOW << "[!] River: dispatch site doesn't match "
                  << "(game updated? re-find vaddr via the m_nRiverType material table)." << RESET << "\n";
}

void RiverManager::applyRiver(const ProcessMemory& mem, int riverId) {
    if (!verified_) {
        std::cout << RED << "[!] River site not verified — cannot patch." << RESET << "\n";
        return;
    }
    if (riverId < 0 || riverId >= RIVER_COUNT) {
        std::cout << RED << "[!] River id out of range (0.." << (RIVER_COUNT - 1) << ")." << RESET << "\n";
        return;
    }

    std::vector<uint8_t> cur;
    if (!readSite(mem, cur) || cur.size() < origBytes().size()) {
        std::cout << RED << "[!] Could not read river site." << RESET << "\n";
        return;
    }
    if (cur != origBytes() && cur[0] != 0xBA) {
        std::cout << RED << "[!] River site holds unexpected bytes — refusing to patch." << RESET << "\n";
        return;
    }

    const std::vector<uint8_t> bytes = (riverId == 0) ? origBytes() : makePatch(riverId);
    mem.freeze();
    const bool wrote = mem.writeBytes(libBase_ + SITE_VADDR, bytes);
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
