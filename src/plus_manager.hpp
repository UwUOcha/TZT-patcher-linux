#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "process_memory.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  PlusManager — клиентское (client-side) включение Dota Plus ВНЕШНИМ патчем.
//  Название «Eternal» из виндового инжектора тут не подходит: там код инжектится
//  .so в адресное пространство (internal), а мы патчим ИЗВНЕ через /proc/pid/mem
//  (external) — никакого нашего кода в памяти Доты нет.
//
//  Метод портирован из рабочего build/eternal_plus_extern.py. UI-гейты читают поле
//  статуса подписки инструкцией
//      mov reg, [rax+0x2c]     (3 байта:  8B 40 2C / 8B 50 2C)
//  Патчим её инлайн на
//      push 1; pop reg         (3 байта:  6A 01 58 / 6A 01 5A)
//  → чтение статуса всегда = 1 → клиент считает Plus активным (клиентсайдно).
//
//  РАБОТАЕТ ТОЛЬКО ПРИ ЗАПУСКЕ: чтение статуса одноразовое, на OnSOCreated/welcome
//  ДО GC-логина. Поэтому ранний путь сразу после появления executable-сегмента
//  libclient.so находит сайты без остановки Dota, затем кратко замораживает процесс
//  только на запись патча. В уже идущей игре менять Plus смысла нет → ни тоггла,
//  ни recovery-состояния между рестартами тулзы.
//
//  Адреса НЕ прибиты к фиксированным vaddr — три сайта-потребителя находятся
//  сигнатурным сканом по libclient.so, чтобы патч переживал апдейты Доты без
//  ручного перепоиска. Сигнатура каждого сайта — это ЯДРО «чтение [rax+0x2c] +
//  ветвление по статусу», БЕЗ волатильного хвоста (rel32/окружающий код): именно
//  хвост ломается при рекомпиляции. Каждое ядро уникально в .text. Если сайт
//  найден не уникально (0 или >1 совпадений) — его НЕ трогаем; остальные патчим.
// ─────────────────────────────────────────────────────────────────────────────
class PlusManager {
    // Один потребитель статуса подписки: абсолютный адрес инструкции чтения поля +
    // её оригинальные байты + патч. ВСЕ три — фиксированной длины 3 байта.
    struct Site {
        uintptr_t addr = 0;             // абсолютный адрес инструкции (найден сканом)
        std::vector<uint8_t> orig;      // mov reg,[rax+0x2c]  (8B 40 2C / 8B 50 2C)
        std::vector<uint8_t> patch;     // push 1; pop reg     (6A 01 58 / 6A 01 5A)
    };

    // Ядровые сигнатуры трёх потребителей (стабильная часть, без хвоста).
    static const std::vector<std::string>& signatures();

    // push 1; pop reg — патч выводится из reg-поля modrm оригинала.
    static std::vector<uint8_t> patchFor(const std::vector<uint8_t>& orig);

    std::vector<Site> sites_;
    bool scanned_ = false;

    // Записать патчи, когда вызывающий уже обеспечил безопасную остановку процесса.
    int writePatches(const ProcessMemory& mem);

public:
    // Найден хотя бы один сайт — есть что патчить.
    bool isFound() const { return !sites_.empty(); }
    bool isComplete() const { return sites_.size() == signatures().size(); }
    size_t siteCount() const { return sites_.size(); }

    // Патч включён, только если найдены и пропатчены ВСЕ ожидаемые сайты.
    bool isEnabled(const ProcessMemory& mem) const;

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions);

    // Ранний launch-path: найти сайты без остановки Dota, затем кратко заморозить
    // процесс только на запись проверенных трёхбайтовых патчей.
    void scanAndEnableEarly(const ProcessMemory& mem,
                            const std::vector<MemoryRegion>& regions);

    // Обычная запись под заморозкой процесса. Патчим ТОЛЬКО сайты в оригинале.
    void enable(const ProcessMemory& mem);
    // Тоггла «выключить» нет: статус читается одноразово на логине.
};
