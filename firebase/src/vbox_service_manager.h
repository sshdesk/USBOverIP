#pragma once
#include <string>

class ScopedVBoxServiceManager {
private:
    bool service_was_started_by_us_;
    bool IsServiceRunning();

public:
    ScopedVBoxServiceManager();
    ~ScopedVBoxServiceManager();
};
