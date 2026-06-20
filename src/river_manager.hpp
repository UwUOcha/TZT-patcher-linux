#pragma once
#include <cstdint>
#include <vector>

#include "process_memory.hpp"

// RiverManager — смена типа реки (River Vials) внешним патчем одной инструкции.
//
// Тип реки — netvar m_nRiverType на C_DOTAGamerules (schema offset 0x6e0).
// В текущем libclient.so материал воды выбирается в одном месте:
//   55a9490: 48 63 96 e0 06 00 00   movslq 0x6e0(%rsi),%rdx
//   55a9497: 48 8d 05 ...           lea    table,%rax
//   55a949e: 48 8b 34 d0            mov    (%rax,%rdx,8),%rsi
//
// Патчим чтение поля на константу:
//   48 63 96 e0 06 00 00 -> BA <id> 00 00 00 90 90
class RiverManager {
    static constexpr uintptr_t SITE_VADDR = 0x55a9490;
    static const std::vector<uint8_t>& origBytes();
    static std::vector<uint8_t> makePatch(int riverId);

    uintptr_t libBase_ = 0;
    bool scanned_ = false;
    bool verified_ = false;

    bool readSite(const ProcessMemory& mem, std::vector<uint8_t>& out) const;

public:
    static constexpr int RIVER_COUNT = 8; // 0..7

    bool isFound() const { return verified_; }
    int currentId(const ProcessMemory& mem) const;

    void scan(const ProcessMemory& mem, uintptr_t base);
    void applyRiver(const ProcessMemory& mem, int riverId);
};
