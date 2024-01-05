#include <iostream>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <atomic>

class Timer {
public:
    Timer() : timer_id(0), is_running(false) {}

    ~Timer() {
        stop();
    }

    // Start the timer with the specified timeout and call closeChannel if timeout is reached
    void start(int timeout_ms, void* ptr) {
        if (!is_running) {
            is_running = true;

            // Set up the timer
            struct sigevent sev;
            sev.sigev_notify = SIGEV_THREAD;
            sev.sigev_value.sival_ptr = ptr;
            sev.sigev_notify_function = &timerCallback;
            sev.sigev_notify_attributes = nullptr;

            struct itimerspec its;
            its.it_value.tv_sec = timeout_ms / 1000;
            its.it_value.tv_nsec = (timeout_ms % 1000) * 1000000;
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 0;

            if (timer_create(CLOCK_REALTIME, &sev, &timer_id) == -1) {
                std::cerr << "Failed to create timer" << std::endl;
                is_running = false;
                return;
            }

            if (timer_settime(timer_id, 0, &its, nullptr) == -1) {
                std::cerr << "Failed to set timer" << std::endl;
                timer_delete(timer_id);
                is_running = false;
                return;
            }
        }
    }

    // Stop the timer
    void stop() {
        if (is_running) {
            is_running = false;
            timer_delete(timer_id);
        }
    }

private:
    timer_t timer_id;
    std::atomic<bool> is_running;

    // Static callback function required by timer_create
    static void timerCallback(sigval sv) {
        keymint::javacard::OmapiTransport *transport = (keymint::javacard::OmapiTransport*)sv.sival_ptr;
        if (transport != nullptr) {
            transport->closeConnection();
        }
    }
};
