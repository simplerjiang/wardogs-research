#include "snapshot.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

using reconstruction::Snapshot;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Frame {
    unsigned sequence = 0;
    std::array<unsigned, 256> fields{};
};

// A test payload whose copy assignment can hold the store lock. The store
// itself has no test hooks, sleeps or timing-dependent behavior.
struct HeldCopy {
    unsigned value = 0;
    std::promise<void>* entered = nullptr;
    std::shared_future<void> release;
    HeldCopy() = default;
    HeldCopy(const HeldCopy&) = default;
    HeldCopy(HeldCopy&&) = default;
    HeldCopy& operator=(HeldCopy&&) = default;
    HeldCopy& operator=(const HeldCopy& other) {
        if (entered) {
            entered->set_value();
            release.wait();
        }
        value = other.value;
        return *this;
    }
};

void contention() {
    Snapshot<HeldCopy> store;
    HeldCopy published;
    published.value = 7;
    store.publish(std::move(published));
    std::promise<void> entered, release;
    auto copying = entered.get_future();
    HeldCopy holder;
    holder.entered = &entered;
    holder.release = release.get_future().share();
    bool copied = false;
    std::thread reader([&] { copied = store.try_copy(holder); });
    copying.wait();
    HeldCopy previous;
    previous.value = 99;
    const bool busy = !store.try_copy(previous);
    release.set_value();
    reader.join();
    require(copied && holder.value == 7, "Holding reader did not receive frame");
    require(busy && previous.value == 99, "Busy consumer overwrote its previous frame");
    require(store.try_copy(previous) && previous.value == 7, "Consumer did not recover");
    std::puts("PASS contention: busy copy preserves destination; next copy succeeds");
}

void consistency() {
    Snapshot<Frame> store;
    std::atomic<bool> complete{false};
    std::promise<void> first_published, first_copied;
    auto ready = first_published.get_future();
    auto consumed = first_copied.get_future();
    std::thread producer([&] {
        for (unsigned i = 1; i <= 50000; ++i) {
            Frame frame;
            frame.sequence = i;
            frame.fields.fill(i);
            store.publish(std::move(frame));
            if (i == 1) {
                first_published.set_value();
                consumed.wait();
            }
        }
        complete = true;
    });
    Frame received;
    ready.wait();
    while (!store.try_copy(received)) std::this_thread::yield();
    bool torn = received.sequence != 1;
    for (const auto value : received.fields) {
        if (value != 1) torn = true;
    }
    unsigned previous = received.sequence, copies = 1, intermediate = 1;
    first_copied.set_value();
    do {
        if (store.try_copy(received)) {
            ++copies;
            if (received.sequence > 0 && received.sequence < 50000) ++intermediate;
            if (received.sequence < previous) torn = true;
            for (const auto value : received.fields) {
                if (value != received.sequence) torn = true;
            }
            previous = received.sequence;
        }
    } while (!complete.load());
    producer.join();
    require(store.try_copy(received) && received.sequence == 50000, "Final publication missing");
    for (const auto value : received.fields) {
        if (value != received.sequence) torn = true;
    }
    require(!torn, "Observed torn or reversed frame");
    std::printf("PASS concurrency: 50000 publications, %u copies, %u intermediate, consistent fields\n",
                copies, intermediate);
}

int main() {
    try {
        contention();
        consistency();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
