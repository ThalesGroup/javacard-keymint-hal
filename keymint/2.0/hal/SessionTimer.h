#include <iostream>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <csignal>
#include <unistd.h>
#include <atomic>

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
            transport_ptr = ptr;
            
            if (std::signal(SIGALRM, &timerCallback) == SIG_ERR) {
                LOG(ERROR) << "Error setting up signal handler for SIGALRM. " << std::endl;
                return;
            }
            
            if (alarm(timeout_ms / 1000) != 0) {
                LOG(ERROR) << "Error setting the alarm. " << std::endl;
                return;
            }
        }
    }

    // Stop the timer
    void stop() {
        if (is_running) {
            is_running = false;
            alarm(0);
        }
    }

private:
    std::atomic<bool> is_running;
    static void* transport_ptr;

    // Static callback function required by timer_create
    static void timerCallback(int signal) {
        LOG(DEBUG) << "signal: " << signal;
        keymint::javacard::OmapiTransport *transport = (keymint::javacard::OmapiTransport*)transport_ptr;
        if (transport != nullptr) {
            transport->closeConnection();
        }
    }
};

void* Timer::transport_ptr = nullptr;
