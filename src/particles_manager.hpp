#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "process_memory.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  ParticlesManager — раскрытие партиклов в тумане войны (libclient.so).
//
//  Клиент каждый кадр для каждого эффекта спрашивает «видна ли точка в FoW» и по
//  ответу зовёт SetRenderingEnabled. Сам FoW-запрос — две функции-хелпера:
//      IsPointVisible(x,y,z) -> bool     — для крупных эффектов (несколько точек)
//      box-visibility(&p1,&p2) -> bool   — для мелких эффектов (радиус ≤ 1024)
//  Обе несут хеш-id (mov edx, 0x6b4ed927) и зовут общий CFoW-резолвер — это делает
//  их прологи уникальными. Форсим ОБЕ вернуть true (mov eax,1; ret) → каждая точка
//  «видима» → эффект рендерится всегда, в т.ч. сквозь туман.
//
//  Подтверждено вживую: вражеские спелл-партиклы видны сквозь FoW. Адреса берём
//  сигнатурным сканом по libclient.so; состояние переживает рестарт тулзы в той
//  же сессии Доты.
// ─────────────────────────────────────────────────────────────────────────────
class ParticlesManager {
    // Один патч-сайт: точка входа функции-хелпера + её оригинальные байты.
    struct Site {
        uintptr_t addr = 0;
        std::vector<uint8_t> original;
    };

    std::vector<Site> sites_;    // оба FoW-хелпера видимости
    bool scanned_ = false;
    uintptr_t moduleBase_ = 0;

    // Файл состояния: подхватить сайты после рестарта тулзы в той же сессии Доты
    // (когда код уже затёрт патчем и сигнатуры не находятся).
    const std::string STATE_PATH = "/tmp/dota_particles_patch.state";

    // mov eax, 1 ; ret — хелпер всегда возвращает «видно».
    static const std::vector<uint8_t>& patchBytes();

    // Прологи двух хелперов видимости (несут `mov edx, 0x6b4ed927`).
    static const std::vector<std::string>& signatures();

    void saveState() const;

    struct State { bool valid = false; uintptr_t base = 0; std::vector<Site> sites; };
    State loadState() const;

public:
    bool isFound() const { return !sites_.empty(); }
    size_t siteCount() const { return sites_.size(); }

    // Патч включён, если на первом сайте стоит наш `mov eax,1` (0xB8).
    bool isEnabled(const ProcessMemory& mem) const;

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions);

    // Вкл/выкл: пропатчить ОБА хелпера на «всегда видно» / вернуть оригиналы.
    void toggle(const ProcessMemory& mem);
};
