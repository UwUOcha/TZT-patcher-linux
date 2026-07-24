#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "process_memory.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  WeatherManager — смена погоды в Dota патчем инструкций чтения поля погоды.
//
//  Тип погоды — целое поле на объекте погоды (this+offset), которое клиент читает
//  и по нему выбирает частицы/освещение. Форсим эти чтения в константу → погода.
//
//  АВТОПОИСК (главный путь): оффсет находится СЕМАНТИЧЕСКИ, без ручной калибровки.
//  Якорь — уникальный идиом функции применения погоды:
//      movslq off(%rdi),%rax        ; читаем тип погоды
//      mov    edx, <N>              ; клэмп к числу типов погоды
//      cmp/cmovg/xor/test/cmovs     ; clamp [0,N]
//      lea    weather_particle_table(%rip),%rdx
//      mov    (%rdx,%rax,8), r64    ; particles/rain_fx/econ_*.vpcf по типу
//  Из movslq читаем disp32 = оффсет типа погоды. Затем патчим ВСЕ чтения этого
//  поля (movsxd/mov/movzbl, любой базовый регистр) в "mov <dest>, id".
// ─────────────────────────────────────────────────────────────────────────────
class WeatherManager {
    // Место патча: адрес инструкции + ОРИГИНАЛ + опкод "mov <dest>,imm32" (0xB8+reg),
    // чтобы патч писал константу в ТОТ ЖЕ регистр-приёмник, что и оригинал.
    struct PatchSite {
        uintptr_t address;
        size_t size;
        std::vector<uint8_t> original;
        uint8_t movOpcode = 0xB8;   // 0xB8+destReg (mov r32, imm32)
    };

    std::vector<PatchSite> sites_;    // подтверждённые места патча (+оригиналы)
    uintptr_t moduleBase_ = 0;        // база exec-региона libclient (ключ сессии)
    uint32_t resolvedOffset_ = 0;     // подтверждённый оффсет погоды
    bool isScanned_ = false;
    bool locked_ = false;             // оффсет подтверждён/восстановлен

    // Файл состояния: чтобы при перезапуске проги (та же сессия Dota) подхватить
    // оффсет даже когда инструкции уже затёрты нашим патчем.
    const std::string STATE_PATH = "/tmp/dota_weather_patch.state";

    // ─────────────────────────────────────────────────────────────────────────
    //  Якорь применения погоды. movslq disp32 (любой базовый регистр) + клэмп
    //  (mov edx,<N> — число погод, wildcard) + lea таблицы econ-частиц. disp32
    //  типа погоды читается по смещению ANCHOR_DISP_POS от начала совпадения.
    // ─────────────────────────────────────────────────────────────────────────
    static const std::string& applyAnchor();
    static constexpr size_t ANCHOR_DISP_POS = 3;

    // Формы чтения поля погоды, которые умеем патчить (префикс до modrm).
    struct ReadForm { std::string name; std::vector<uint8_t> prefix; };
    static const std::vector<ReadForm>& readForms();

    static constexpr uint32_t MIN_OFFSET   = 0x100;  // нижняя граница разумного смещения
    static constexpr uint32_t MAX_OFFSET   = 0x4000; // верхняя граница разумного смещения

    // Байты патча "mov <dest>, id" + NOP-добивка до длины инструкции.
    static std::vector<uint8_t> makePatch(int weatherId, size_t size, uint8_t movOpcode);

    // Автопоиск: найти оффсет погоды по якорю (0 = не найден).
    uint32_t findWeatherOffset(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions) const;
    // Захватить все патч-сайты (movsxd/mov/movzbl [base+offset]) для оффсета.
    std::vector<PatchSite> captureFieldSites(const ProcessMemory& mem,
                                             const std::vector<MemoryRegion>& regions,
                                             uint32_t offset) const;

    void saveState() const;

    struct State {
        bool valid = false;        // есть валидные данные погоды (база + места патча)
        uintptr_t base = 0;
        uint32_t offset = 0;
        std::vector<PatchSite> sites;
    };
    State loadState() const;

public:
    uint32_t offset() const { return resolvedOffset_; }
    bool isLocked() const { return locked_; }

    void scan(const ProcessMemory& mem, const std::vector<MemoryRegion>& regions);

    // Смена погоды по подтверждённому оффсету. id == 0 (Default) ВОССТАНАВЛИВАЕТ
    // оригинальный код, а не пишет ноль: погода становится настоящей дефолтной.
    void applyWeather(const ProcessMemory& mem, int weatherId);
};
