#pragma once

#include <map>
#include <memory>
#include <vector>


#include <aidl/android/se/omapi/BnSecureElementListener.h>
#include <aidl/android/se/omapi/ISecureElementChannel.h>
#include <aidl/android/se/omapi/ISecureElementListener.h>
#include <aidl/android/se/omapi/ISecureElementReader.h>
#include <aidl/android/se/omapi/ISecureElementService.h>
#include <aidl/android/se/omapi/ISecureElementSession.h>
#include <mutex>

#include <android/binder_manager.h>

#include "ITransport.h"

// Session timeout
#define SESSION_TIMEOUT_20S (20000)  // 20 s
#define SESSION_TIMEOUT_300S (300000) // 300s
#include <aidl/android/hardware/secure_element/ISecureElement.h>
using aidl::android::hardware::secure_element::ISecureElement;

namespace keymint::javacard {
using std::vector;

/**
 * SeTransport is derived from ITransport. This class gets the OMAPI service binder instance and
 * uses IPC to communicate with OMAPI service. OMAPI inturn communicates with hardware via
 * ISecureElement.
 */
class SeTransport : public ITransport {

  public:
    SeTransport() : secure_element(nullptr), channel_number(0) , channelOpenned(false){
    }
    /**
     * Gets the binder instance of ISEService, gets te reader corresponding to secure element,
     * establishes a session and opens a basic channel.
     */
    keymaster_error_t openConnection() override;
    /**
     * Transmists the data over the opened basic channel and receives the data back.
     */
    keymaster_error_t sendData(const vector<uint8_t>& inData, vector<uint8_t>& output) override;

    /**
     * Closes the connection.
     */
    keymaster_error_t closeConnection() override;
    /**
     * Returns the state of the connection status. Returns true if the connection is active, false
     * if connection is broken.
     */
    bool isConnected() override;

  private:
    std::shared_ptr<ISecureElement> secure_element;
    int channel_number;
    bool channelOpenned;
    keymaster_error_t initialize();
    bool
    internalTransmitApdu(std::shared_ptr<ISecureElement> secure_element_,
                         std::vector<uint8_t> apdu, std::vector<uint8_t>& transmitResponse);
    std::mutex connectionMutex;
};

}
