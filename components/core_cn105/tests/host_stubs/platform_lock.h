#pragma once
#include <mutex>
namespace platform_lock {
class RecursiveMutex {
public:
    bool init() { return true; }
    bool lock() { mutex_.lock(); return true; }
    void unlock() { mutex_.unlock(); }
private:
    std::recursive_mutex mutex_;
};
class ScopedLock {
public:
    explicit ScopedLock(RecursiveMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
    ~ScopedLock() { mutex_.unlock(); }
    bool locked() const { return true; }
private:
    RecursiveMutex& mutex_;
};
}
