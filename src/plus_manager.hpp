#pragma once
#include <cstdint>
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
//  ДО GC-логина. Поэтому патчим из экрана запуска, ПОКА игра грузится. Менять Plus
//  в уже идущей игре смысла нет → в игровом меню тоггла нет.
//
//  Адреса — ФИКСИРОВАННЫЕ vaddr внутри libclient.so (libBase + vaddr), три места
//  потребителей (см. PATCHES в .py). Если оригинальные байты не совпали — Dota
//  обновилась, бинарь сдвинулся: НЕ патчим, нужно перенайти vaddr.
// ─────────────────────────────────────────────────────────────────────────────
class PlusManager {
    // Один потребитель статуса подписки: vaddr инструкции чтения поля + ожидаемые
    // оригинальные байты + патч. ВСЕ три — фиксированной длины 3 байта.
    struct Site {
        uintptr_t vaddr;                // смещение внутри libclient.so (как в .py)
        std::vector<uint8_t> orig;      // mov reg,[rax+0x2c]  (8B 40 2C / 8B 50 2C)
        std::vector<uint8_t> patch;     // push 1; pop reg     (6A 01 58 / 6A 01 5A)
    };

    // (vaddr, оригинал, патч) — синхронизировано с PATCHES в .py.
    static const std::vector<Site>& table();

    uintptr_t libBase_ = 0;  // база загрузки libclient.so (ELF vaddr 0)
    bool scanned_ = false;
    bool verified_ = false;  // libBase известна и все сайты читаются как orig или patch

    // Прочитать текущие 3 байта сайта по его vaddr.
    bool readSite(const ProcessMemory& mem, const Site& s, std::vector<uint8_t>& out) const;

public:
    bool isFound() const { return verified_; }

    // Патч включён, если ПЕРВЫЙ сайт сейчас читается как push 1 (0x6A).
    bool isEnabled(const ProcessMemory& mem) const;

    // libBase — наименьший mapped-адрес libclient.so (ELF vaddr 0).
    void scan(const ProcessMemory& mem, uintptr_t base);

    // Запись под заморозкой процесса. Патчим ТОЛЬКО сайты в оригинале.
    void enable(const ProcessMemory& mem);
    // Тоггла «выключить» нет: статус читается одноразово на логине.
};
