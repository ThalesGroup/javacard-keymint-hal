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
#define LOG_TAG "SeTransport"
#include "SeTransport.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <iomanip>
#include <future>

#include <android-base/logging.h>
#include <android-base/properties.h>

#define ENABLE_SESSION_TIMEOUT
#include "SessionTimer.h"
#include <stdlib.h>
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>

#define MAX_INIT_COUNT 15
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

#ifdef ENABLE_SESSION_TIMEOUT
Timer sessionTimer;
#endif

class MySecureElementCallback : public BnSecureElementCallback {
  public:
    ScopedAStatus onStateChange(bool state, const std::string& debugReason) override {
        return ScopedAStatus::ok();
    };
};

std::shared_ptr<ISecureElement> secure_element_;
std::shared_ptr<MySecureElementCallback> secure_element_callback_;

std::shared_ptr<ISecureElement> getSecureElementService() {
    SpAIBinder binder = SpAIBinder(AServiceManager_waitForService("android.hardware.secure_element.ISecureElement/eSE1"));

    secure_element_ = ISecureElement::fromBinder(binder);
    if(secure_element_ == nullptr) return nullptr;

    secure_element_callback_ = SharedRefBase::make<MySecureElementCallback>();
    if(secure_element_callback_ == nullptr) return nullptr;

    secure_element_->init(secure_element_callback_);

    return secure_element_;
}

static uint8_t initCounter = 0;
keymaster_error_t SeTransport::initialize() {
    std::vector<std::string> readers = {};
    LOG(DEBUG) << "Initialize the secure element connection";
    initCounter = 0;

    sessionTimer.count = 0;
    LOG(INFO) << "Initialize the secure element connection";

    if(android::base::GetProperty(PROP_KEYMINT_VENDOR, "") != "Google") {
        LOG(DEBUG) << "Initialize the AID to be Thales";
        KEYMINT_APPLET_AID = SELECTABLE_AID_THALES;
        AID_SIZE = 13;
    } else {
        LOG(DEBUG) << "Initialize the AID to be Google";
        KEYMINT_APPLET_AID = SELECTABLE_AID_GOOGLE;
        AID_SIZE = 9;
    }

    do {
        secure_element = getSecureElementService();
        if (secure_element == nullptr) {
            LOG(ERROR) << "Failed to start SEHAL service null";
            if(initCounter < MAX_INIT_COUNT) {
                initCounter++;
                usleep(INIT_RETRY_DELAY * 1000);
                continue;
            }
            return static_cast<keymaster_error_t>(KM_ERROR_HARDWARE_NOT_YET_AVAILABLE);
        }
    } while (initCounter > 0 && initCounter < MAX_INIT_COUNT+1);

    int size = AID_SIZE;

    bool isSecureElementPresent = false;
    auto res = secure_element->isCardPresent(&isSecureElementPresent);

    if (!res.isOk()) {
        LOG(ERROR) << "isSecureElementPresent error: " << res.getMessage();
        return static_cast<keymaster_error_t>(KM_ERROR_HARDWARE_TYPE_UNAVAILABLE);
    }
    if (!isSecureElementPresent) {
        LOG(ERROR) << "secure element not found";
        return static_cast<keymaster_error_t>(KM_ERROR_HARDWARE_TYPE_UNAVAILABLE);
    }

    return KM_ERROR_OK;
}

bool SeTransport::internalTransmitApdu(
    std::shared_ptr<ISecureElement> secure_element_,
    std::vector<uint8_t> apdu, std::vector<uint8_t>& transmitResponse) {

    std::future<bool> result = std::async(std::launch::async, [this, secure_element_, apdu, &transmitResponse]() {
        std::lock_guard<std::mutex> lock(connectionMutex);

        std::vector<uint8_t> cmd = apdu;
        LogicalChannelResponse logical_channel_response;
        LOG(DEBUG) << "internalTransmitApdu: trasmitting data to secure element";

#ifdef ENABLE_SESSION_TIMEOUT
        // Stop the timer
        LOG(DEBUG) << "Stop timeout if any.";
        sessionTimer.stop();
#endif

        if (secure_element_ == nullptr) {
            LOG(ERROR) << "eSE is null";
            return false;
        }

        auto res = ndk::ScopedAStatus::ok();

        int size = AID_SIZE;
        std::vector<uint8_t> aid(KEYMINT_APPLET_AID, KEYMINT_APPLET_AID + size);
        if (!channelOpenned) {
            res = secure_element_->openLogicalChannel(aid, 0x00, &logical_channel_response);
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

        cmd[0] |= channel_number;

        res = secure_element_->transmit(cmd, &transmitResponse);

        LOG(INFO) << "STATUS OF TRNSMIT: " << res.getExceptionCode()
                  << " Message: " << res.getMessage();
        if (!res.isOk()) {
            LOG(ERROR) << "transmit error: " << res.getMessage();
            return false;
        }

#ifdef ENABLE_SESSION_TIMEOUT
        LOG(DEBUG) << "Start timeout before closing channels ";
        if ( apdu.size() > 2 && apdu.at(1) == 0x30 ) {
            sessionTimer.count++;
        } else if ( apdu.size() > 2 && ( apdu.at(1) == 0x32 || apdu.at(1) == 0x33) ) {
            sessionTimer.count--;
        }
        if ( sessionTimer.count > 0 ) {
            sessionTimer.start(SESSION_TIMEOUT_300S, this);
        } else {
            sessionTimer.start(SESSION_TIMEOUT_20S, this);
        }
#endif

        return true;
    });
    return result.get();
}

keymaster_error_t SeTransport::openConnection() {

    // if already conection setup done, no need to initialise it again.
    if (isConnected()) {
        return KM_ERROR_OK;
    }
    return initialize();
}

keymaster_error_t SeTransport::sendData(const vector<uint8_t>& inData, vector<uint8_t>& output) {

    std::vector<uint8_t> cmd = inData;
    if (!isConnected()) {
        // Try to initialize connection to eSE
        LOG(INFO) << "Failed to send data, try to initialize connection SE connection";
        auto res = initialize();
        if (res != KM_ERROR_OK) {
            LOG(ERROR) << "Failed to send data, initialization not completed";
            closeConnection();
            return res;
        }
    }

    if (secure_element != nullptr) {
        LOG(DEBUG) << "Sending apdu data to secure element: " << ESE_READER_PREFIX;
        if(internalTransmitApdu(secure_element, cmd, output)) {
            if(android::base::GetBoolProperty(PROP_KEYMINT_CLOSE_CHANNEL, false))
                closeConnection();
            return KM_ERROR_OK;
        } else {
            closeConnection();
            return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
        }
    } else {
        LOG(ERROR) << "secure element reader " << ESE_READER_PREFIX << " not found";
        return KM_ERROR_SECURE_HW_COMMUNICATION_FAILED;
    }
}

keymaster_error_t SeTransport::closeConnection() {
    std::future<keymaster_error_t> result = std::async(std::launch::async, [this]() {
        std::lock_guard<std::mutex> lock(connectionMutex);
        LOG(DEBUG) << "Closing all connections";
        if (channel_number != 0) {
            secure_element->closeChannel(channel_number);
            channel_number = 0;
            channelOpenned = false;
        }
        if (secure_element != nullptr) secure_element = nullptr;
        return KM_ERROR_OK;
    });
    return result.get();
}

bool SeTransport::isConnected() {
    // Check already initialization completed or not
    std::future<bool> result = std::async(std::launch::async, [this]() {
        std::lock_guard<std::mutex> lock(connectionMutex);
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
