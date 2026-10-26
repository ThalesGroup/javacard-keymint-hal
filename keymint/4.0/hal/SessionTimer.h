#pragma once

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <thread>
#include "ITransport.h"

#define SESSION_TIMEOUT_20S (20000)  // 20 s
#define SESSION_TIMEOUT_300S (300000)  // 300 s

class Timer {

public:
    Timer() : is_running_(false), transport_ptr_(nullptr), callback_executing_(false) {
        LOG(DEBUG) << "[SessionTimer] Timer constructor";
    }

    // Timer is not copiable and not moveable (callback)
    Timer(const Timer&)            = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&)                 = delete;
    Timer& operator=(Timer&&)      = delete;

    ~Timer() {
        LOG(DEBUG) << "[SessionTimer] Timer destructor";

        stop();

        std::lock_guard<std::mutex> lock(timer_mutex_);
        {
            release_();
        }
    }

    // Initialize Transport
    // Do nothing if timer already created
    // Calling release() to delete timer 
    void init(keymint::javacard::ITransport* transport_ptr )
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);

        if( !timer_created_)
        {
            transport_ptr_ = transport_ptr;
            LOG(DEBUG) << "[SessionTimer] Transport registered";
        }
        else
            LOG(WARNING) << "[SessionTimer] init() ignored: timer already created";        
    }

    // delete the timer created by init()
    void release()
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        release_();
    }

    // Start the timer with the specified timeout and call closeChannel if timeout is reached
    void start(int timeout_ms) {
        std::lock_guard<std::mutex> lock(timer_mutex_);

        if (timeout_ms <= 0) {
            LOG(ERROR) << "[SessionTimer] Invalid timeout=" << timeout_ms;
            return;
        }

        if (!is_running_) {
            LOG(DEBUG) << "[SessionTimer] Starting timer with timeout=" << timeout_ms << " ms";

            // create timer only if not yet exist
            if( !timer_created_ ) {
                LOG(DEBUG) << "[SessionTimer] Creating timer";
                struct sigevent sev {};
                sev.sigev_notify = SIGEV_THREAD;
                sev.sigev_value.sival_ptr = this;
                sev.sigev_notify_function = &timerCallback;
                sev.sigev_notify_attributes = nullptr;

                if( timer_create(CLOCK_BOOTTIME_ALARM, &sev, &timer_id_) == -1) {
                    LOG(ERROR) << "[SessionTimer] Failed to create timer (errno=" << errno << ")";
                    return;
                }

                LOG(DEBUG) << "[SessionTimer] Timer created successfully";
                timer_created_ = true;
            }

            LOG(DEBUG) << "[SessionTimer] Setting timer to " << timeout_ms << "ms";

            struct itimerspec its {};
            its.it_value.tv_sec = timeout_ms / 1000;
            its.it_value.tv_nsec = (timeout_ms % 1000) * 1000000;
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 0;
            if (timer_settime(timer_id_, 0, &its, nullptr) == -1) {
                LOG(ERROR) << "[SessionTimer] Failed to set timer";
                return;
            }

            is_running_ = true;
        }
        else
            LOG(ERROR) << "[SessionTimer] Already running";
    }

    // Stop the timer
    void stop() {
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            if (is_running_) {
                LOG(DEBUG) << "[SessionTimer] Stopping timer";

                itimerspec spec{};

                if( timer_settime(timer_id_, 0, &spec, nullptr) == -1 )
                    LOG(ERROR) << "[SessionTimer] Unable to stop Timer";
                else
                    LOG(DEBUG) << "[SessionTimer] Timer stopped";
                
                // In all case, timer disarmed or not
                is_running_ = false;
            }
        }

        // Wait for callback to complete outside the lock
        while (callback_executing_.load()) {
            std::this_thread::yield();
        }
    }

private:
    std::mutex timer_mutex_; // Used to protect timer_created_, timer_id_, transport_ptr_ and is_running_

    bool is_running_;
    keymint::javacard::ITransport* transport_ptr_; // Updated by init() / Clear by release()
    std::atomic<bool> callback_executing_;
    timer_t timer_id_{};
    bool timer_created_ = false;

    void release_()
    {
        if( transport_ptr_ != nullptr)
        {
            transport_ptr_ = nullptr;
            LOG(DEBUG) << "[SessionTimer] Transport unregistered";
        }

        // Free timer resource
        if (timer_created_) {
            LOG(DEBUG) << "[SessionTimer] Releasing timer";
            timer_delete(timer_id_); // If timer armed, it is disarmed before being deleted 
            timer_created_ = false;
        }         
    }


    // Release timerMutex before calling closeConnection() to avoid deadlock:
    // internalTransmitApdu holds connectionMutex and calls stop() (needs timerMutex),
    // so the callback must never hold timerMutex while acquiring connectionMutex.
    static void timerCallback(sigval sv) {

        LOG(DEBUG) << "[SessionTimer] Timer callback execution";
        Timer* self = static_cast<Timer*>(sv.sival_ptr);
        if (self == nullptr)
            return;

        keymint::javacard::ITransport* transport_ptr = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->timer_mutex_);


            // Check if timer was destroyed
            if( !self->timer_created_)
            {
                LOG(DEBUG) << "[SessionTimer] Timer callback: timer was destroyed, skipping";
                return;
            }

            // Check if timer was stopped
            if (!self->is_running_) {
                LOG(DEBUG) << "[SessionTimer] Timer callback: timer was stopped, skipping";
                return;
            }

            if( self->transport_ptr_ == nullptr) {
                LOG(DEBUG) << "[SessionTimer] Timer callback: transport was cleared, skipping";
                return;
            }            

            self->is_running_ = false;
            transport_ptr = self->transport_ptr_;
            self->callback_executing_ = true;
        }

        if (transport_ptr != nullptr) {
            LOG(DEBUG) << "[SessionTimer] Closing connection";

            if( !transport_ptr->tryCloseConnection() )
                LOG(ERROR) << "[SessionTimer] Unable to close connection";
        }

        LOG(DEBUG) << "[SessionTimer] Timer callback terminated";
        self->callback_executing_ = false;
    }
};
