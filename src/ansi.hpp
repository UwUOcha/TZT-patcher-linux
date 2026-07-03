#pragma once
#include <iostream>
#include <string>

namespace ansi {
        inline const std::string RESET = "\033[0m";
        inline const std::string RED = "\033[31m";
        inline const std::string GREEN = "\033[32m";
        inline const std::string YELLOW = "\033[33m";
        inline const std::string BLUE = "\033[34m";
        inline const std::string MAGENTA = "\033[35m";
        inline const std::string CYAN = "\033[36m";
        inline const std::string BOLD = "\033[1m";
        inline const std::string DIM = "\033[2m";
        inline const std::string GRAY = "\033[90m";

        inline const std::string WHITE = "\033[97m";
        inline const std::string ORANGE = "\033[38;5;208m";
        inline const std::string PURPLE = "\033[38;5;141m";
        inline const std::string PINK = "\033[38;5;213m";
} // namespace ansi
