/*
 **
 ** Copyright 2020, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */
#define LOG_TAG "Keymint-SeTransport"
#include "SeTransport.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <iomanip>
#include <future>
#include <chrono>
#include <thread>

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <stdlib.h>
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>


#define MAX_SEND_COUNT 5
#define INIT_RETRY_DELAY 1000 //ms
#include <aidl/android/hardware/secure_element/BnSecureElementCallback.h>


using aidl::android::hardware::secure_element::BnSecureElementCallback;

using aidl::android::hardware::secure_element::LogicalChannelResponse;
using ndk::ScopedAStatus;
using ndk::SharedRefBase;
using ndk::SpAIBinder;

using namespace android;

#define PROP_KEYMINT_CLOSE_CHANNEL "vendor.keymint.closechannel"
#define PROP_KEYMINT_VENDOR "persist.vendor.keymint.applet"

namespace keymint::javacard {

uint8_t SELECTABLE_AID_THALES[] = {0xA0, 0x00, 0x00, 0x00, 0x18, 0x43, 0x43, 0x43, 0x43, 0x43, 0x42, 0x41, 0x01};
uint8_t SELECTABLE_AID_GOOGLE[] = {0xA0, 0x00, 0x00, 0x08, 0x44, 0x00, 0x00, 0xAA, 0x01};
uint8_t *KEYMINT_APPLET_AID;
uint8_t AID_SIZE;
//constexpr uint8_t KEYMINT_APPLET_AID[] = {0xA0, 0x00, 0x00, 0x00, 0x62, 0x03,
//                                          0x02, 0x0C, 0x01, 0x01, 0x01};
std::string const ESE_READER_PREFIX = "eSE";
constexpr const char omapiServiceName[] =
        "android.se.omapi.ISecureElementService/default";

class SEListener : public ::aidl::android::se::omapi::BnSecureElementListener {};

class MySecureElementCallback : public BnSecureElementCallback {
  public:
    ScopedAStatus onStateChange(bool state, const std::string& debugReason) override {
        return ScopedAStatus::ok();
    };
};

std::shared_ptr<ISecureElement> getSecureElementService(
        std::shared_ptr<BnSecureElementCallback>& out_callback) {
    SpAIBinder binder = SpAIBinder(AServiceManager_checkService("android.hardware.secure_element.ISecureElement/eSE1"));
    if(binder.get() == nullptr){
      LOG(ERROR) << "Failed to get secure element service binder";
      return nullptr;
    }

    auto se = ISecureElement::fromBinder(binder);
    if(se == nullptr) return nullptr;

    out_callback = SharedRefBase::make<MySecureElementCallback>();
    if(out_callback == nullptr) return nullptr;

    auto res = se->init(out_callback);
    if (!res.isOk()) {
        LOG(ERROR) << "Failed to init secure element: " << res.getMessage();
        return nullptr;
    }

    return se;
}

SeTransport::SeTransport() : secure_element(nullptr), channel_number(0) , channelOpenned(false){
    LOG(DEBUG) << "SeTransport: constructor called";

#ifdef ENABLE_SESSION_TIMEOUT
    std::lock_guard<std::mutex> timerLock(timerMutex);
    sessionTimer.init(this);
#endif
}

SeTransport::~SeTransport() {
    LOG(DEBUG) << "SeTransport: destructor called";
#ifdef ENABLE_SESSION_TIMEOUT
    {
        std::lock_guard<std::mutex> timerLock(timerMutex);
        sessionTimer.stop(); // disarms timer and wait end of timer callback if started
        sessionTimer.release();
    }
#endif

    std::lock_guard<std::mutex> lock(connectionMutex);
    _closeConnection();
}

static uint8_t initCounter = 0;
keymaster_error_t SeTransport::initialize() {
    std::vector<std::string> readers = {};
    LOG(DEBUG) << "Initialize the secure element connection";
    
    #ifdef ENABLE_SESSION_TIMEOUT
      sessionCount = 0;
    #endif // ENABLE_SESSION_TIMEOUT


    if(android::base::GetProperty(PROP_KEYMINT_VENDOR, "") != "Google") {
        LOG(DEBUG) << "Initialize the AID to be Thales";
        KEYMINT_APPLET_AID = SELECTABLE_AID_THALES;
        AID_SIZE = 13;
    } else {
        LOG(DEBUG) << "Initialize the AID to be Google";
        KEYMINT_APPLET_AID = SELECTABLE_AID_GOOGLE;
        AID_SIZE = 9;
    }

        secure_element = getSecureElementService(secure_element_callback_);

        if (secure_element == nullptr) {
        LOG(ERROR) << "Failed to start SEHAL service null";
        return static_cast<keymaster_error_t>(KM_ERROR_HARDWARE_NOT_YET_AVAILABLE);
    }
    
    int size = AID_SIZE;
    bool isSecureElementPresent = false;
    auto res = secure_element->isCardPresent(&isSecureElementPresent);

    if (!res.isOk()) {
        LOG(ERROR) << "isSecureElementPresent error: " << res.getMessage();
        secure_element = nullptr;
        return static_cast<keymaster_error_t>(KM_ERROR_HARDWARE_TYPE_UNAVAILABLE);
    }
    if (!isSecureElementPresent) {
        LOG(ERROR) << "secure element not found";
        secure_element = nullptr;
        return static_cast<keymaster_error_t>(KM_ERROR_HARDWARE_TYPE_UNAVAILABLE);
    }

    return KM_ERROR_OK;
}

bool SeTransport::internalTransmitApdu(
    std::shared_ptr<ISecureElement> se,
    std::vector<uint8_t> apdu, std::vector<uint8_t>& transmitResponse) {

    std::future<bool> result = std::async(std::launch::async, [this, se, apdu, &transmitResponse]() {

    std::vector<uint8_t> cmd = apdu;
    LogicalChannelResponse logical_channel_response;
    LOG(DEBUG) << "internalTransmitApdu: transmitting data to secure element";

#ifdef ENABLE_SESSION_TIMEOUT
        {
            std::lock_guard<std::mutex> timerLock(timerMutex);
            LOG(DEBUG) << "Stop timeout if any.";
            sessionTimer.stop();
        }
#endif

        if (secure_element == nullptr) {
            LOG(ERROR) << "eSE is null";
            return false;
        }

        if( cmd.empty() ) {
            LOG(ERROR) << "APDU command is empty";
            return false;
        }

        auto res = ndk::ScopedAStatus::ok();

        int size = AID_SIZE;
        std::vector<uint8_t> aid(KEYMINT_APPLET_AID, KEYMINT_APPLET_AID + size);
        if (!channelOpenned) {
            res = se->openLogicalChannel(aid, 0x00, &logical_channel_response);
            if (!res.isOk()) {
                LOG(ERROR) << "openLogicalChannel error: " << res.getMessage();
                return false;
            }
            if (logical_channel_response.channelNumber == 0) {
                LOG(ERROR) << "Could not open channel null";
                return false;
            }
            channel_number = logical_channel_response.channelNumber;
            channelOpenned = true;
            LOG(DEBUG) << "openLogicalChannel: channel=" << channel_number;

            if (logical_channel_response.selectResponse.empty()) {
                LOG(ERROR) << "logical_channel_response.selectResponse == nullptr ";
                return false;
            }

            if ((logical_channel_response.selectResponse.size() < 2)
                || ((logical_channel_response.selectResponse[logical_channel_response.selectResponse.size() -1] & 0xFF) != 0x00)
                || ((logical_channel_response.selectResponse[logical_channel_response.selectResponse.size() -2] & 0xFF) != 0x90))
            {
                LOG(ERROR) << "Failed to select the Applet.";
                return false;
            }
        }

        // Rewrite CLA byte per ISO 7816-4: clear channel bits first, then encode.
        // Mirrors OMAPI Channel.java setChannelToClassByte logic.
        if (channel_number < 4) {
            // First interindustry class byte: clear bits[1:0] and bit6, preserve SM bits.
            cmd[0] = (cmd[0] & 0xBC) | (uint8_t)channel_number;
        } else {
            // Further interindustry class byte: clear bits[3:0] and bit6, set bit6 + offset.
            cmd[0] = (cmd[0] & 0xB0) | 0x40 | (uint8_t)(channel_number - 4);
        }

        res = se->transmit(cmd, &transmitResponse);

        LOG(INFO) << "STATUS OF TRANSMIT: " << res.getExceptionCode()
                  << " Message: " << res.getMessage();

#ifdef ENABLE_SESSION_TIMEOUT
        {
            LOG(DEBUG) << "Start timeout before closing channels ";
            if ( apdu.size() > 2 && apdu.at(1) == 0x30 ) {
                sessionCount ++;
                LOG(DEBUG) << "Session opened, count=" << sessionCount;
            } else if ( apdu.size() > 2 && ( apdu.at(1) == 0x32 || apdu.at(1) == 0x33) ) {
                if( sessionCount >= 1)
                    sessionCount--;
                LOG(DEBUG) << "Session closed, count=" << sessionCount;
            }
            if ( sessionCount > 0 ) {
                LOG(DEBUG) << "Active sessions, using 300s timeout";

                std::lock_guard<std::mutex> timerLock(timerMutex);
                sessionTimer.start(SESSION_TIMEOUT_300S);
            } else {
                LOG(DEBUG) << "No active sessions, using 20s timeout";

                std::lock_guard<std::mutex> timerLock(timerMutex);
                sessionTimer.start(SESSION_TIMEOUT_20S);
            }
        }
#endif

        if (!res.isOk()) {
            LOG(ERROR) << "transmit error: " << res.getMessage();
            return false;
        }

        return true;
    });
    return result.get();
}

keymaster_error_t SeTransport::openConnection() {
    std::lock_guard<std::mutex> lock(connectionMutex);
    return _openConnection();
}

keymaster_error_t SeTransport::_openConnection() {
    LOG(DEBUG) << "SeTransport: openConnection";
    // if already conection setup done, no need to initialise it again.
    if (_isConnected()) {
        return KM_ERROR_OK;
    }
    return initialize();
}

keymaster_error_t SeTransport::sendData(const vector<uint8_t>& inData, vector<uint8_t>& output) {
    std::lock_guard<std::mutex> lock(connectionMutex);
    return _sendData(inData, output);
}

keymaster_error_t SeTransport::_sendData(const vector<uint8_t>& inData, vector<uint8_t>& output) {

    std::vector<uint8_t> cmd = inData;

    if( inData.size() < 4 ) { // Minimal size 4 bytes (CLA/INS/P1/P2)
        LOG(ERROR) << "Invalid data to send (min size)";
        return KM_ERROR_INVALID_ARGUMENT;
    }

    if (!_isConnected()) {
        // Try to initialize connection to eSE
        LOG(INFO) << "Failed to send data, try to initialize connection SE connection";
        auto res = initialize();
        if (res != KM_ERROR_OK) {
            LOG(ERROR) << "Failed to send data, initialization not completed";
            _closeConnection();
            return res;
        }
    }

    if (secure_element != nullptr) {
        LOG(DEBUG) << "Sending apdu data to secure element: " << ESE_READER_PREFIX;
        if(internalTransmitApdu(secure_element, cmd, output)) {
            if(android::base::GetBoolProperty(PROP_KEYMINT_CLOSE_CHANNEL, false))
                _closeConnection();
            return KM_ERROR_OK;
        } else {
            _closeConnection();
            return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
        }
    } else {
        LOG(ERROR) << "secure element reader " << ESE_READER_PREFIX << " not found";
        return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
    }
}

keymaster_error_t SeTransport::closeConnection() {

#ifdef ENABLE_SESSION_TIMEOUT
    {
        // Stop the timer before closing connection
        std::lock_guard<std::mutex> timerLock(timerMutex);
        sessionTimer.stop(); // disarms timer and wait end of timer callback if started
    }
#endif

    std::lock_guard<std::mutex> lock(connectionMutex);
    return _closeConnection();
}

bool SeTransport::tryCloseConnection() { 
    std::unique_lock<std::mutex> lock(connectionMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        // Unable to lock mutex -> do not close connection
        LOG(ERROR) << "Unable to try to close connection (mutex already locked)";
        return false;
    }

    return _closeConnection() == KM_ERROR_OK;
}

keymaster_error_t SeTransport::_closeConnection() {
    std::future<keymaster_error_t> result = std::async(std::launch::async, [this]() {
        LOG(DEBUG) << "Closing all connections";
        if (secure_element != nullptr) {
            if (channel_number != 0) {
                LOG(DEBUG) << "closeChannel: channel=" << channel_number;
                if( !secure_element->closeChannel(channel_number).isOk() )
                    LOG(WARNING) << "Unable to close channel!";

                channel_number = 0;
                channelOpenned = false;
            }
            // Release SE Callback first: SE HAL dead detection removes its reference to this callback
            secure_element_callback_ = nullptr;
            // Then release SE Service
            secure_element = nullptr;
        }

        return KM_ERROR_OK;
    });
    return result.get();
}

bool SeTransport::isConnected() {
    // Check already initialization completed or not
    std::lock_guard<std::mutex> lock(connectionMutex);
    return _isConnected();
}

bool SeTransport::_isConnected() {
    // Check already initialization completed or not
    std::future<bool> result = std::async(std::launch::async, [this]() {
        if (secure_element != nullptr) {
            LOG(INFO) << "Connection initialization already completed";
            return true;
        }

        LOG(DEBUG) << "Connection initialization not completed";
        return false;
    });
    return result.get();
}

}
