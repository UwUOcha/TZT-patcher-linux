#include "plus_manager.hpp"

#include <iostream>

#include "ansi.hpp"

using namespace ansi;

const std::vector<std::string>& PlusManager::signatures() {
    // Ядро каждого сайта: чтение статуса [rax+0x2c] + начало ветвления по нему.
    // Волатильный хвост (rel32/окружающий код) НЕ включён — он и ломается при
    // рекомпиляции. Каждое ядро уникально в .text (проверено на текущем бинаре):
    //   #1  mov eax,[rax+0x2c] ; xor r14d,r14d ; test eax,eax
    //       → Panorama property "IsPlusMember"
    //   #2  mov eax,[rax+0x2c] ; lea r12,[rbp-0x30] ; cmp eax,2/1
    //       → "PlusPrepaid" / "PlusRenewal"
    //   #3  mov edx,[rax+0x2c] ; test edx,edx ; je ; cmp edx,2/1
    //       → обновление локального кеша Plus из того же status getter
    //
    // Live-проверка 2026-07-01, md5 12247b9f14d3: vaddr 0x63fd449,
    // 0x6706f26, 0x6ac496c. Эти числа — только диагностическая история:
    // runtime использует сигнатуры, а не vaddr.
    static const std::vector<std::string> s = {
        "8B 40 2C 45 31 F6 85 C0",
        "8B 40 2C 4C 8D 65 D0",
        "8B 50 2C 85 D2 74 ?? 83 FA 02 74 ?? 83 FA 01",
    };
    return s;
}

std::vector<uint8_t> PlusManager::patchFor(const std::vector<uint8_t>& orig) {
    // mov reg,[rax+0x2c] → push 1 ; pop reg. reg берём из поля reg байта ModRM.
    if (orig.size() != 3 || orig[0] != 0x8B || orig[2] != 0x2C ||
        (orig[1] & 0xC7) != 0x40)
        return {};
    const uint8_t reg = (orig[1] >> 3) & 0x07;
    return { 0x6A, 0x01, static_cast<uint8_t>(0x58 + reg) };
}

bool PlusManager::isEnabled(const ProcessMemory& mem) const {
    if (!isComplete()) return false;
    for (const auto& s : sites_) {
        std::vector<uint8_t> cur;
        if (!mem.readBytes(s.addr, s.patch.size(), cur) || cur != s.patch)
            return false;
    }
    return true;
}

void PlusManager::scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) {
    if (scanned_) return;
    scanned_ = true;

    const auto& sigs = signatures();

    // waitForClientLib() пропускает нас только после появления executable mapping,
    // поэтому повторные 250-ms циклы здесь не нужны: .text уже отображён целиком.
    // Поиск идёт без остановки Dota; каждый сайт принимаем только при уникальном
    // совпадении.
    for (const auto& sigStr : sigs) {
        const std::vector<int> sig = parsePattern(sigStr);
        uintptr_t found = 0;
        int matches = 0;
        for (const auto& region : regions) {
            for (const auto addr : findAllPatterns(mem, region, sig)) {
                if (++matches == 1) found = addr;
                if (matches > 1) break;
            }
            if (matches > 1) break;
        }
        if (matches != 1) continue;

        Site st;
        st.addr = found;
        if (!mem.readBytes(found, 3, st.orig) || st.orig.size() != 3) continue;
        st.patch = patchFor(st.orig);
        if (!st.patch.empty()) sites_.push_back(std::move(st));
    }

    if (sites_.empty()) {
        std::cout << YELLOW << "[!] Dota Plus: no status-read sites found "
                  << "(game updated a lot? refresh the mov[rax+0x2c] core signatures)."
                  << RESET << "\n";
        return;
    }

    std::cout << CYAN << "[*] Dota Plus: " << sites_.size() << "/" << sigs.size()
              << " status-read sites located";
    if (sites_.size() != sigs.size())
        std::cout << YELLOW << " (some signatures stale — still patch what we have)" << CYAN;
    std::cout << " (applied at launch)." << RESET << "\n";
}

int PlusManager::writePatches(const ProcessMemory& mem) {
    int n = 0;
    for (const auto& s : sites_) {
        std::vector<uint8_t> cur;
        if (!mem.readBytes(s.addr, s.orig.size(), cur)) continue;
        if (cur == s.patch) { n++; continue; }                  // уже стоит
        if (cur != s.orig) continue;                            // чужие байты — не трогаем
        if (mem.writeBytes(s.addr, s.patch)) n++;
    }
    return n;
}

void PlusManager::scanAndEnableEarly(const ProcessMemory& mem,
                                     const std::vector<MemoryRegion>& regions) {
    scan(mem, regions);
    if (sites_.empty()) return;

    // Сам сигнатурный поиск может занять заметное время и не требует остановки.
    // SIGSTOP нужен лишь на короткую запись инструкций, чтобы исключить torn write.
    mem.freeze();
    const int n = writePatches(mem);
    mem.unfreeze();

    if (isComplete())
        std::cout << GREEN << "[+] Dota Plus early patch installed before GC init ("
                  << n << "/" << sites_.size() << " sites)." << RESET << "\n";
    else
        std::cout << YELLOW << "[!] Dota Plus early patch is partial ("
                  << n << "/" << signatures().size()
                  << " sites); it will not be reported as ON." << RESET << "\n";
}

void PlusManager::enable(const ProcessMemory& mem) {
    if (sites_.empty()) return;
    mem.freeze();
    const int n = writePatches(mem);
    mem.unfreeze();
    std::cout << GREEN << "[+] Dota Plus ON (client-side) — status reads forced to Active at launch ("
              << n << "/" << sites_.size() << " sites)!" << RESET << "\n";
}
