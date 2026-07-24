#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "process_memory.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  CameraManager — дистанция камеры через РЕЗОЛВ ПО КОДУ, без скана по значению
//  и БЕЗ записи в сам ConVar.
//
//  Как оно устроено в клиенте (RE libclient.so):
//    • `dota_camera_distance` — обычный ConVar (флаги 0x4000 = CHEAT). Объект
//      CConVar<float> в .bss: на +0x08 указатель на ConVarData, значение — float
//      на ConVarData+0x58. ЭТО ЗНАЧЕНИЕ МЫ НЕ ТРОГАЕМ: конвар — штатная сущность,
//      её значение клиент умеет отдавать наружу по запросу, в отличие от
//      внутреннего кеша ниже. Читаем его только чтобы знать дефолт.
//    • Клиент держит РАСПАКОВАННЫЙ кеш настроек камеры — массив float в .data:
//          g_cameraSettings[0] = dota_camera_distance
//          g_cameraSettings[1] = dota_camera_fov_max
//          [2..5] = -1 (значит «не переопределено»)
//    • Итоговый зум каждый кадр считается как
//          lerp(dota_camera_distance_min, g_cameraSettings[0], t)
//      то есть g_cameraSettings[0] — «максимальное отдаление».
//    • Кеш перезаливается из конваров колбэком на изменение любого camera-конвара.
//      Поэтому просто записать кеш мало — ближайший рефреш вернёт дефолт.
//
//  Отсюда механика: НОПИМ инструкции, которые пишут в g_cameraSettings[0]
//  (`movss [rip+G],xmm0` / `movlps [rip+G],xmm0`), и пишем значение в кеш сами.
//  Дальше клиент не может его перетереть. Это тот же класс воздействия, что уже
//  используется для weather/river/particles/plus — патч кода, а не подмена
//  штатного состояния игры. Default восстанавливает оригинальные байты и
//  возвращает в кеш значение из ConVar.
//
//  Резолв — цепочка якорей, каждый из которых проверяет следующий:
//    1) строка "dota_camera_distance" в r-- регионе libclient;
//    2) ЕДИНСТВЕННЫЙ `lea rsi,[rip+str]` в .text — место регистрации ConVar;
//    3) ближайший назад `lea rax,[rip+obj]`, obj в rw-памяти модуля → CConVar;
//    4) в .text ищем читателей `mov rax,[rip+obj+8]` + `movss xmm0,[rax+0x58]`,
//       за которыми идёт `movss/movlps [rip+G],xmm0` → G = g_cameraSettings[0],
//       а сами эти store-инструкции и есть места патча.
//  Принимаем G только при КОНСЕНСУСЕ ≥2 независимых сайтов. Не сошлось —
//  считаем камеру ненайденной и НИЧЕГО не пишем: лучше «not found», чем запись
//  в случайный float (ровно это и делал старый скан ±10 от 1200).
// ─────────────────────────────────────────────────────────────────────────────
class CameraManager {
    // Инструкция записи в g_cameraSettings[0] + её оригинальные байты.
    struct Site {
        uintptr_t addr = 0;
        std::vector<uint8_t> original;
    };

    std::vector<Site> sites_;    // все места, откуда кеш перезаливается из конвара
    uintptr_t cacheAddr_ = 0;    // g_cameraSettings[0] — то, что читает рендер камеры
    uintptr_t convarAddr_ = 0;   // ConVarData+0x58 — ТОЛЬКО ЧТЕНИЕ (дефолт для Default)
    uintptr_t moduleBase_ = 0;   // база libclient (ключ сессии Dota)
    bool scanned_ = false;

    // Файл состояния: после нашего патча инструкции записи в кеш затёрты NOP'ами,
    // и повторный запуск проги себя уже не найдёт. Поэтому в рамках одной сессии
    // Dota (совпала база модуля) адреса и оригиналы подхватываются отсюда —
    // ровно как это делают river/particles/weather.
    const std::string STATE_PATH = "/tmp/dota_camera_patch.state";

    struct State {
        bool valid = false;
        uintptr_t base = 0;
        uintptr_t cache = 0;
        uintptr_t convar = 0;
        std::vector<Site> sites;
    };
    void saveState() const;
    State loadState() const;

    // CConVar<float> / ConVarData: смещения, добытые из кода клиента.
    static constexpr size_t CONVAR_DATA_PTR = 0x08;
    static constexpr size_t CONVAR_VALUE    = 0x58;

    // Разумные границы для sanity-проверки найденного значения.
    static constexpr float MIN_SANE = 100.0f;
    static constexpr float MAX_SANE = 100000.0f;

    // Многобайтовый NOP нужной длины (7 для movlps, 8 для movss).
    static std::vector<uint8_t> nopOfSize(size_t size);

    // Занопить/восстановить места перезаливки кеша. Возвращает число сайтов.
    int writeSites(const ProcessMemory& mem, bool patched) const;

public:
    static constexpr float DEFAULT_DISTANCE = 1200.0f;

    bool isLinked() const { return cacheAddr_ != 0 && !sites_.empty(); }
    uintptr_t address() const { return cacheAddr_; }

    // Текущая дистанция из кеша настроек камеры (0 если не привязаны).
    float distance(const ProcessMemory& mem) const;

    // Кеш «залочен» нами — клиент не может перезалить его из конвара.
    bool isForced(const ProcessMemory& mem) const;

    // Найти кеш и места его перезаливки. Полный список регионов нужен потому, что
    // хвост .bss libclient ложится в АНОНИМНЫЙ rw-регион сразу за модулем — по
    // имени модуля он не находится, а объект ConVar лежит именно там.
    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& allRegions);

    // Залочить кеш и записать в него дистанцию.
    bool setDistance(const ProcessMemory& mem, float value);

    // Вернуть оригинальный код и значение конвара в кеш.
    bool restoreDefault(const ProcessMemory& mem);
};
