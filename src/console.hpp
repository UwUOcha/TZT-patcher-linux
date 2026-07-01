#pragma once
#include <iostream>
#include <limits>
#include <string>

#include "ansi.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Мелкие хелперы для консольного UI: разделитель, пауза, чтение строки.
//  Вынесены отдельно, чтобы не дублировать одни и те же `cin.ignore/get` по меню.
// ─────────────────────────────────────────────────────────────────────────────
namespace console {

// Горизонтальный разделитель блоков меню.
inline std::string divider() {
    return "  " + ansi::GRAY + "──────────────────────────────────────────────" + ansi::RESET + "\n";
}

// Сбросить остаток строки во входном буфере (после `cin >> x`).
inline void flushLine() {
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

// Ожидание Enter после raw-key экрана: там символ запуска уже прочитан напрямую,
// поэтому сбрасывать строку нельзя — иначе первый новый Enter уйдёт в ignore().
inline void waitForEnter(const std::string& msg) {
    std::cout << msg;
    std::cin.clear();
    std::cin.get();
}

// «Нажмите Enter, чтобы вернуться» — съедаем хвост строки и ждём отдельного Enter.
inline void pause(const std::string& msg = "\nPress Enter to return...") {
    std::cout << msg;
    flushLine();
    std::cin.get();
}

} // namespace console
