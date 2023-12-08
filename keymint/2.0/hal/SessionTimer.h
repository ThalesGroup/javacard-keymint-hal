#include <iostream>
#include <thread>
#include <functional>
#include <chrono>
#include <android-base/logging.h>
#include <android-base/properties.h>

class Timer {
public:
    Timer() : is_running(false) {}

    ~Timer() {
        stop();
    }

    // Start the timer with the specified timeout and call closeChannel if timeout is reached
    void start(int timeout_ms, void* ptr) {
        if (!is_running) {
            is_running = true;
            timer_thread = std::thread([this, timeout_ms, ptr]() {
                keymint::javacard::OmapiTransport *obj = (keymint::javacard::OmapiTransport*)ptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
                if (is_running && obj != nullptr) {
                    obj->closeConnection();
                }
            });
        }
    }

    // Stop the timer
    void stop() {
        if (is_running) {
            is_running = false;
            if (timer_thread.joinable()) {
                timer_thread.detach();
            }
        }
    }

private:
    bool is_running;
    std::thread timer_thread;
};
