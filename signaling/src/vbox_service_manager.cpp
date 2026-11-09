#include "vbox_service_manager.h"
#include <iostream>
#include <cstdlib>
#include <spdlog/spdlog.h>

bool ScopedVBoxServiceManager::IsServiceRunning() {
    FILE* pipe = _popen("sc.exe query VBoxUsbMon", "r");
    if (!pipe) return false;

    char buffer[128];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    _pclose(pipe);

    return result.find("RUNNING") != std::string::npos;
}

ScopedVBoxServiceManager::ScopedVBoxServiceManager() {
    if (!IsServiceRunning()) {
        SPDLOG_INFO("[ServiceManager] VBoxUsbMon is stopped. Starting service...");
        int ret = system("sc.exe start VBoxUsbMon");
        service_was_started_by_us_ = (ret == 0);
    } else {
        SPDLOG_INFO("[ServiceManager] VBoxUsbMon is already running.");
        service_was_started_by_us_ = false;
    }
}

ScopedVBoxServiceManager::~ScopedVBoxServiceManager() {
    if (service_was_started_by_us_) {
        SPDLOG_INFO("[ServiceManager] Client exiting. Stopping VBoxUsbMon service...");
        system("sc.exe stop VBoxUsbMon");
    }
}