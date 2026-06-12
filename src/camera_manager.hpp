#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "process_memory.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  CameraManager — поиск и изменение дистанции камеры в data-регионах libclient.
//
//  Дистанция хранится как float в rw-памяти. «Привязка» (link) — это скан региона
//  на значение, близкое к искомому; найденный адрес запоминается и потом в него
//  пишутся новые значения. Адрес отдаётся наружу через колбэк persist, чтобы
//  WeatherManager сохранил его в общий файл состояния сессии.
// ─────────────────────────────────────────────────────────────────────────────
class CameraManager {
    const ProcessMemory& mem_;
    const std::vector<MemoryRegion>& dataRegions_;
    std::function<void(uintptr_t)> persist_;   // сохранить адрес в файл состояния

    uintptr_t addr_ = 0;
    float distance_ = DEFAULT_DISTANCE;

public:
    static constexpr float DEFAULT_DISTANCE = 1200.0f;

    CameraManager(const ProcessMemory& mem, const std::vector<MemoryRegion>& dataRegions,
                  std::function<void(uintptr_t)> persist)
        : mem_(mem), dataRegions_(dataRegions), persist_(std::move(persist)) {}

    bool isLinked() const { return addr_ != 0; }
    uintptr_t address() const { return addr_; }
    float distance() const { return distance_; }

    // Найти адрес дистанции рядом с dist и привязаться к нему.
    bool linkAt(float dist);

    // Привязка из восстановленного адреса (файл состояния прошлой сессии).
    void linkRecovered(uintptr_t addr, float dist);

    // Записать новую дистанцию в привязанный адрес.
    bool setDistance(float value);
};
