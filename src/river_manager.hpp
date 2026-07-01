#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "process_memory.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  RiverManager — смена типа реки (River Vials) внешним патчем одной инструкции.
//
//  Тип реки — netvar m_nRiverType на C_DOTAGamerules. Материал воды выбирается в
//  одном месте libclient.so:
//    movslq disp(%rsi),%rdx        48 63 96 <disp32>     ; читаем m_nRiverType
//    lea    table(%rip),%rax       48 8d 05 <rel32>
//    mov    (%rax,%rdx,8),%rsi     48 8b 34 d0           ; выбираем материал
//
//  Патчим чтение поля на константу:
//    48 63 96 <disp32>  ->  BA <id> 00 00 00 90 90       ; mov edx,id ; nop nop
//
//  Адрес НЕ прибит к фиксированному vaddr — он находится сигнатурным сканом, чтобы
//  патч переживал апдейты Доты без ручного перепоиска. disp32 в сигнатуре —
//  wildcard: тогда сдвиг самого schema-оффсета m_nRiverType тоже переживём, а не
//  только сдвиг бинаря. Уникальность держат lea(%rip)+индексная загрузка в %rsi:
//    48 63 96 ?? ?? 00 00 48 8d 05 ?? ?? ?? ?? 48 8b 34 d0
//
//  Состояние (адрес+оригинал) пишется в /tmp — рестарт тулзы в той же сессии Доты
//  подхватывает сайт, даже когда код уже затёрт нашим патчем и сигнатура не ищется.
// ─────────────────────────────────────────────────────────────────────────────
class RiverManager {
    // movslq-инструкция чтения m_nRiverType: 7 байт, которые мы перезаписываем.
    static constexpr size_t SITE_LEN = 7;

    // Сигнатура movslq+lea+mov с wildcard-нутым disp32 (уникальна в .text).
    static const std::string& signature();
    static std::vector<uint8_t> makePatch(int riverId);

    uintptr_t siteAddr_ = 0;              // абсолютный адрес movslq (найден сканом)
    std::vector<uint8_t> orig_;           // оригинальные 7 байт movslq
    uintptr_t moduleBase_ = 0;            // база exec-региона libclient (ключ сессии)
    bool scanned_ = false;
    bool verified_ = false;

    const std::string STATE_PATH = "/tmp/dota_river_patch.state";

    bool readSite(const ProcessMemory& mem, std::vector<uint8_t>& out) const;

    void saveState() const;
    struct State { bool valid = false; uintptr_t base = 0; uintptr_t addr = 0; std::vector<uint8_t> orig; };
    State loadState() const;

public:
    static constexpr int RIVER_COUNT = 8; // 0..7

    bool isFound() const { return verified_; }
    int currentId(const ProcessMemory& mem) const;

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions);
    void applyRiver(const ProcessMemory& mem, int riverId);
};
