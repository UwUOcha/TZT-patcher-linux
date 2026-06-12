#include "camera_manager.hpp"

bool CameraManager::linkAt(float dist) {
    addr_ = 0;
    for (const auto& region : dataRegions_) {
        addr_ = scanForCameraAddress(mem_, region, dist);
        if (addr_ != 0) break;
    }
    if (addr_ != 0) {
        distance_ = dist;
        if (persist_) persist_(addr_);
    }
    return addr_ != 0;
}

void CameraManager::linkRecovered(uintptr_t addr, float dist) {
    addr_ = addr;
    distance_ = dist;
}

bool CameraManager::setDistance(float value) {
    if (addr_ == 0) return false;
    if (!mem_.write(addr_, value)) return false;
    distance_ = value;
    return true;
}
