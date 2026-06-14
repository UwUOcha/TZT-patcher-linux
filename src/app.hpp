#pragma once
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
//  App — точка входа консольного UI: определяет, запущена ли Dota, показывает
//  экран запуска модов или цепляется к процессу, затем крутит главное меню.
// ─────────────────────────────────────────────────────────────────────────────
class App {
public:
    int run();

private:
    // Какие «включил и забыл» код-патчи активировать сразу после запуска игры.
    // Камера и погода интерактивны (нужны значения/калибровка) — их тут нет.
    struct LaunchSelection {
        bool particles = false;
        bool dotaPlus  = false; // Dota Plus (client-side): ставить ДО GC-логина
    };

    // Экран запуска: Dota ещё не запущена. Возвращает PID запущенной игры или 0.
    pid_t runLaunchScreen(LaunchSelection& sel);

    // Главное меню по уже запущенной Dota.
    void runExternalMode(pid_t dotaPid, const LaunchSelection& sel, bool launchedByUs);

    static void printWeatherList();
    static void printRiverList();
};
