#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "process_memory.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  WeatherManager — смена погоды в Dota патчем инструкций чтения поля [RBX+disp].
//
//  По одним байтам поле погоды от других полей не отличить, поэтому единственный
//  надёжный критерий правильного смещения — реально ли поменялась погода в игре.
//  Калибровка ([t] в UI) перебирает кандидатов вокруг HINT_OFFSET и подтверждает
//  рабочий по глазам пользователя.
// ─────────────────────────────────────────────────────────────────────────────
class WeatherManager {
    // Одно совпадение инструкции, читающей поле [RBX+disp].
    struct Hit {
        uintptr_t address;
        std::string type;   // "Particles" (movsxd) или "Lighting" (mov)
        uint32_t disp;      // реальное смещение, прочитанное из инструкции
        size_t size;        // длина инструкции (сколько байт перезаписываем)
    };

    // Место патча: адрес инструкции + её ОРИГИНАЛЬНЫЕ байты (чтобы вернуть как было).
    struct PatchSite {
        uintptr_t address;
        size_t size;
        std::vector<uint8_t> original;
    };

    std::vector<Hit> allHits_;        // все найденные чтения [RBX+small]
    std::vector<PatchSite> sites_;    // подтверждённые места патча (+оригиналы)
    std::vector<PatchSite> backup_;   // оригиналы текущего НЕподтверждённого пробного патча
    uintptr_t moduleBase_ = 0;        // база exec-региона libclient (ключ сессии)
    uint32_t resolvedOffset_ = 0;     // подтверждённый оффсет погоды
    bool isScanned_ = false;
    bool locked_ = false;             // оффсет подтверждён/восстановлен

    // Адрес дистанции камеры тоже хранится в общем файле состояния, чтобы после
    // перезапуска проги (та же сессия Dota) сразу менять даже изменённую дистанцию.
    uintptr_t persistedCamera_ = 0;   // что писать в файл (0 = нет)
    uintptr_t loadedCamera_ = 0;      // что подхватили из файла этой сессии (0 = нет)

    // Файл состояния: чтобы при перезапуске проги (та же сессия Dota) подхватить
    // оффсет даже когда инструкции уже затёрты нашим патчем.
    const std::string STATE_PATH = "/tmp/dota_weather_patch.state";

    // ─────────────────────────────────────────────────────────────────────────
    //  HINT_OFFSET — последний РАБОЧИЙ оффсет погоды. Перебор идёт от кандидатов,
    //  ближайших к этому числу. Когда подтвердишь новый оффсет — впиши его сюда,
    //  чтобы в следующий раз перебор начинался прямо с него.
    // ─────────────────────────────────────────────────────────────────────────
    static constexpr uint32_t HINT_OFFSET  = 0xAD4;
    static constexpr uint32_t MIN_OFFSET   = 0x100;  // нижняя граница разумного смещения
    static constexpr uint32_t MAX_OFFSET   = 0x4000; // верхняя граница разумного смещения
    static constexpr uint32_t TRIAL_WINDOW = 0x80;   // насколько далеко от HINT перебираем

    struct PatternDef {
        std::string name;
        std::string opcodePrefix; // байты инструкции ДО 32-битного смещения
    };
    static const std::vector<PatternDef>& patterns();

    static uint32_t absDiff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

    // Байты патча "mov eax, id" + NOP-добивка до длины инструкции.
    static std::vector<uint8_t> makePatch(int weatherId, size_t size);

    std::vector<const Hit*> hitsForOffset(uint32_t offset) const;
    std::vector<PatchSite> captureSites(const ProcessMemory& mem, uint32_t offset) const;

    void saveState() const;

    struct State {
        bool valid = false;        // есть валидные данные погоды (база + места патча)
        uintptr_t base = 0;
        uint32_t offset = 0;
        uintptr_t cameraAddr = 0;
        std::vector<PatchSite> sites;
    };
    State loadState() const;

public:
    // Адрес камеры, подхваченный из файла этой же сессии (0 = нечего восстанавливать).
    uintptr_t recoveredCameraAddr() const { return loadedCamera_; }

    // Запомнить адрес камеры и переписать файл состояния (вместе с данными погоды).
    void persistCamera(uintptr_t addr);

    uint32_t offset() const { return resolvedOffset_; }
    bool hasCandidates() const { return !allHits_.empty(); }
    bool isLocked() const { return locked_; }

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions);

    // Уникальные смещения в окне вокруг HINT, отсортированные по близости к нему.
    std::vector<uint32_t> trialOffsets() const;

    // Пробно запатчить все инструкции с данным смещением, сохранив оригиналы в backup.
    int applyAt(const ProcessMemory& mem, uint32_t offset, int weatherId);

    // Вернуть оригинальные байты последнего пробного патча.
    void revert(const ProcessMemory& mem);

    // Зафиксировать найденный оффсет как рабочий: пробные оригиналы становятся
    // постоянными местами патча, состояние пишется в файл.
    void lock(uint32_t offset);

    // Смена погоды по подтверждённому оффсету. id == 0 (Default) ВОССТАНАВЛИВАЕТ
    // оригинальный код, а не пишет ноль: погода становится настоящей дефолтной.
    void applyWeather(const ProcessMemory& mem, int weatherId);
};
