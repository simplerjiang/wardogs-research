#pragma once

#include <mutex>
#include <utility>

namespace reconstruction {

// Publication and copying share one lock. A busy consumer keeps its old frame.
// This reproduces the observed publication contract, not the reference ABI,
// collector scheduling, double-buffer layout or complete stop/restart lifecycle.
template<class Frame>
class Snapshot {
public:
    void publish(Frame frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = std::move(frame);
    }

    bool try_copy(Frame& destination) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        destination = frame_;
        return true;
    }

private:
    std::mutex mutex_;
    Frame frame_{};
};

} // namespace reconstruction
