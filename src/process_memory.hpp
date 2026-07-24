#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Один регион из /proc/pid/maps.
// ─────────────────────────────────────────────────────────────────────────────
struct MemoryRegion {
    uintptr_t start = 0;
    uintptr_t end = 0;
    std::string perms;
    std::string path;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessMemory — RAII-обёртка над /proc/pid/mem: чтение/запись чужой памяти
//  и заморозка процесса на время патча кода.
// ─────────────────────────────────────────────────────────────────────────────
class ProcessMemory {
    pid_t pid_ = -1;
    int memFd_ = -1;

public:
    ProcessMemory() = default;
    ~ProcessMemory();

    ProcessMemory(const ProcessMemory&) = delete;
    ProcessMemory& operator=(const ProcessMemory&) = delete;

    bool attach(pid_t targetPid);
    pid_t pid() const { return pid_; }

    // Заморозка/разморозка всего процесса (как SuspendAllThreads из виндового DLL).
    // /proc/pid/mem пишется и без остановки, но для патча КОДА, который прямо сейчас
    // исполняют другие потоки, остановка избавляет от torn write. SIGSTOP/SIGCONT
    // тормозят весь процесс целиком — этого достаточно.
    void freeze() const;
    void unfreeze() const;

    template <typename T>
    bool read(uintptr_t address, T& value) const {
        return pread(memFd_, &value, sizeof(T), address) == static_cast<ssize_t>(sizeof(T));
    }

    template <typename T>
    bool write(uintptr_t address, const T& value) const {
        return pwrite(memFd_, &value, sizeof(T), address) == static_cast<ssize_t>(sizeof(T));
    }

    bool writeBytes(uintptr_t address, const std::vector<uint8_t>& data) const;
    bool readBytes(uintptr_t address, size_t size, std::vector<uint8_t>& buffer) const;

    // Регион целиком одним буфером (чтение идёт чанками — pread на десятки
    // мегабайт /proc/pid/mem отдаёт короткое чтение). Нужно там, где анализ
    // смотрит и назад, и вперёд от найденного места, и рвать поток нельзя.
    bool readRegion(const MemoryRegion& region, std::vector<uint8_t>& buffer) const;

    std::vector<MemoryRegion> memoryRegions() const;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Поиск процессов и сигнатурный скан.
// ─────────────────────────────────────────────────────────────────────────────

// PID'ы процессов, в cmdline которых встречается processName.
std::vector<pid_t> findProcessesByName(const std::string& processName);

// Разобрать строку байтов вида "48 63 83 ?? ?? 00 00"; "??"/"?" → -1 (wildcard).
std::vector<int> parsePattern(const std::string& pattern);

// Все адреса в исполняемом регионе, где совпала маска pattern.
std::vector<uintptr_t> findAllPatterns(const ProcessMemory& mem, const MemoryRegion& region,
                                       const std::vector<int>& pattern);

