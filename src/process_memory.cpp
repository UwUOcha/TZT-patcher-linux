#include "process_memory.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <csignal>
#include <dirent.h>
#include <fcntl.h>

ProcessMemory::~ProcessMemory() {
    if (memFd_ >= 0) close(memFd_);
}

bool ProcessMemory::attach(pid_t targetPid) {
    pid_ = targetPid;
    const std::string memPath = "/proc/" + std::to_string(pid_) + "/mem";
    memFd_ = open(memPath.c_str(), O_RDWR);
    return memFd_ >= 0;
}

void ProcessMemory::freeze() const   { if (pid_ > 0) kill(pid_, SIGSTOP); }
void ProcessMemory::unfreeze() const { if (pid_ > 0) kill(pid_, SIGCONT); }

bool ProcessMemory::writeBytes(uintptr_t address, const std::vector<uint8_t>& data) const {
    return pwrite(memFd_, data.data(), data.size(), address) == static_cast<ssize_t>(data.size());
}

bool ProcessMemory::readBytes(uintptr_t address, size_t size, std::vector<uint8_t>& buffer) const {
    buffer.resize(size);
    return pread(memFd_, buffer.data(), size, address) == static_cast<ssize_t>(size);
}

bool ProcessMemory::readRegion(const MemoryRegion& region, std::vector<uint8_t>& buffer) const {
    constexpr size_t CHUNK_SIZE = 0x100000; // 1MB
    const size_t total = region.end - region.start;
    buffer.assign(total, 0);
    for (size_t done = 0; done < total;) {
        const size_t want = std::min(CHUNK_SIZE, total - done);
        const ssize_t got = pread(memFd_, buffer.data() + done, want, region.start + done);
        if (got <= 0) return false;
        done += static_cast<size_t>(got);
    }
    return true;
}

std::vector<MemoryRegion> ProcessMemory::memoryRegions() const {
    std::vector<MemoryRegion> regions;
    const std::string mapsPath = "/proc/" + std::to_string(pid_) + "/maps";
    std::ifstream mapsFile(mapsPath);
    if (!mapsFile.is_open()) return regions;

    std::string line;
    while (std::getline(mapsFile, line)) {
        MemoryRegion region;
        const size_t dashPos = line.find('-');
        const size_t spacePos = line.find(' ');
        if (dashPos == std::string::npos) continue;

        region.start = std::stoull(line.substr(0, dashPos), nullptr, 16);
        region.end = std::stoull(line.substr(dashPos + 1, spacePos - dashPos - 1), nullptr, 16);
        region.perms = line.substr(spacePos + 1, 4);

        const size_t pathPos = line.find('/');
        if (pathPos != std::string::npos) region.path = line.substr(pathPos);
        regions.push_back(region);
    }
    return regions;
}

std::vector<pid_t> findProcessesByName(const std::string& processName) {
    std::vector<pid_t> pids;
    DIR* dir = opendir("/proc");
    if (!dir) return pids;

    const pid_t myPid = getpid();
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        const pid_t pid = atoi(entry->d_name);
        if (pid <= 0 || pid == myPid) continue;

        const std::string cmdlinePath = "/proc/" + std::string(entry->d_name) + "/cmdline";
        std::ifstream cmdlineFile(cmdlinePath);
        if (cmdlineFile.is_open()) {
            std::string cmdline;
            std::getline(cmdlineFile, cmdline);
            if (cmdline.find(processName) != std::string::npos) pids.push_back(pid);
        }
    }
    closedir(dir);
    return pids;
}

std::vector<int> parsePattern(const std::string& pattern) {
    std::vector<int> bytes;
    std::stringstream ss(pattern);
    std::string byteStr;
    while (ss >> byteStr) {
        if (byteStr == "??" || byteStr == "?") bytes.push_back(-1);
        else bytes.push_back(std::stoi(byteStr, nullptr, 16));
    }
    return bytes;
}

std::vector<uintptr_t> findAllPatterns(const ProcessMemory& mem, const MemoryRegion& region,
                                       const std::vector<int>& pattern) {
    if (region.perms.find('x') == std::string::npos || pattern.empty())
        return {};

    constexpr size_t CHUNK_SIZE = 0x100000; // 1MB chunks

    std::vector<uint8_t> buffer;
    std::vector<uintptr_t> results;

    for (uintptr_t addr = region.start; addr < region.end; addr += CHUNK_SIZE) {
        const size_t readSize = std::min(CHUNK_SIZE, static_cast<size_t>(region.end - addr));
        if (readSize < pattern.size()) continue;
        if (!mem.readBytes(addr, readSize, buffer)) continue;

        // `i + pattern.size() <= readSize` avoids the size_t underflow that the
        // old `i <= readSize - pattern.size()` had on short region tails.
        for (size_t i = 0; i + pattern.size() <= readSize; ++i) {
            bool found = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                if (pattern[j] != -1 && buffer[i + j] != static_cast<uint8_t>(pattern[j])) {
                    found = false;
                    break;
                }
            }
            if (found) results.push_back(addr + i);
        }
    }
    return results;
}
