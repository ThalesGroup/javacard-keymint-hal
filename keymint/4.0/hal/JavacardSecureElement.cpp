/*
 * Copyright 2020, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "javacard.keymint.device.strongbox-impl"
#include "JavacardSecureElement.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <memory>
#include <regex.h>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <keymaster/android_keymaster_messages.h>

#include "keymint_utils.h"

namespace keymint::javacard {

bool initialized = false;

keymaster_error_t JavacardSecureElement::initializeJavacard() {
    Array request;
    request.add(Uint(getOsVersion()));
    request.add(Uint(getOsPatchlevel()));
    request.add(Uint(getVendorPatchlevel()));
    if(initialized)
        return KM_ERROR_OK;
    auto [item, err] = sendRequest(Instruction::INS_INIT_STRONGBOX_CMD, request);
    if(err == KM_ERROR_OK)
        initialized = true;
    if(err == -10001) {
        initialized = true;
        return KM_ERROR_OK;
    }
    return err;
}

void JavacardSecureElement::setDeleteAllKeysPending() {
    isDeleteAllKeysPending = true;
}

void JavacardSecureElement::setEarlyBootEndedPending() {
    isEarlyBootEndedPending = true;
}

void JavacardSecureElement::sendPendingEvents() {
    if (isDeleteAllKeysPending) {
        auto [_, err] = sendRequest(Instruction::INS_DELETE_ALL_KEYS_CMD);
        if (err == KM_ERROR_OK) {
            isDeleteAllKeysPending = false;
        } else {
            LOG(ERROR) << "Error in sending deleteAllKeys.";
        }
    }

    if (isEarlyBootEndedPending) {
        auto [_, err] = sendRequest(Instruction::INS_EARLY_BOOT_ENDED_CMD);
        if (err == KM_ERROR_OK) {
            isEarlyBootEndedPending = false;
        } else {
            LOG(ERROR) << "Error in sending earlyBootEnded.";
        }
    }
}

keymaster_error_t JavacardSecureElement::constructApduMessage(Instruction& ins,
                                                              uint8_t p2,
                                                              std::vector<uint8_t>& inputData,
                                                              std::vector<uint8_t>& apduOut) {
    apduOut.push_back(static_cast<uint8_t>(APDU_CLS));  // CLS
    apduOut.push_back(static_cast<uint8_t>(ins));       // INS
    apduOut.push_back(static_cast<uint8_t>(APDU_P1));   // P1
    apduOut.push_back(static_cast<uint8_t>(p2));   // P2


    if (USHRT_MAX >= inputData.size()) {
		// Send extended length APDU always as response size is not known to HAL.
		// Case 1: Lc > 0  CLS | INS | P1 | P2 | 00 | 2 bytes of Lc | CommandData | 2 bytes of Le
		// all set to 00. Case 2: Lc = 0  CLS | INS | P1 | P2 | 3 bytes of Le all set to 00.
		// Extended length 3 bytes, starts with 0x00
		apduOut.push_back(static_cast<uint8_t>(0x00));
		if (inputData.size() > 0) {
			apduOut.push_back(static_cast<uint8_t>(inputData.size() >> 8));
			apduOut.push_back(static_cast<uint8_t>(inputData.size() & 0xFF));
			// Data
			apduOut.insert(apduOut.end(), inputData.begin(), inputData.end());
		}
		// Expected length of output.
		// Accepting complete length of output every time.
		apduOut.push_back(static_cast<uint8_t>(0x00));
		apduOut.push_back(static_cast<uint8_t>(0x00));

    } else {
        LOG(ERROR) << "Error in constructApduMessage.";
        return (KM_ERROR_INVALID_INPUT_LENGTH);
    }
    return (KM_ERROR_OK);  // success
}

keymaster_error_t JavacardSecureElement::sendData(Instruction ins, std::vector<uint8_t>& inData,
                                                  std::vector<uint8_t>& response) {
    keymaster_error_t ret = KM_ERROR_UNKNOWN_ERROR;
    std::vector<uint8_t> apdu, temp_response;
    const size_t apduChuckSize = 255;
    size_t currentIndex = 0;
    uint8_t p2 = 0x00;

    do {
        size_t currentChunkSize = std::min(apduChuckSize, inData.size() - currentIndex);
        std::vector<uint8_t> apduData(inData.begin() + currentIndex, inData.begin() + currentIndex + currentChunkSize);

        if((inData.size() - currentIndex <= apduChuckSize) && p2 != 0x00) {
            p2 |= 0x80;
            p2++;
        } else if(inData.size() - currentIndex > apduChuckSize) {
            p2++;
        }

        currentIndex += currentChunkSize;
        LOG(DEBUG) << "inData.size() =  " << inData.size();
        LOG(DEBUG) << "currentIndex =  " << currentIndex;
        LOG(DEBUG) << "currentChunkSize =  " << currentChunkSize;

        LOG(DEBUG) << "p2 =  " << std::hex << p2;

        apdu.clear();
        ret = constructApduMessage(ins, p2, apduData, apdu);

        LOG(DEBUG) << "apdu.size() =  " << apdu.size();
        LOG(DEBUG) << "ret =  " << ret;
        if (ret != KM_ERROR_OK) {
            return ret;
        }

        temp_response.clear();
        ret = transport_->sendData(apdu, temp_response);
        if (ret != KM_ERROR_OK) {
            LOG(ERROR) << "Error in sending data in sendData. " << ret;
            return ret;
        }

        if (getApduStatus(temp_response) != APDU_RESP_STATUS_OK) {
            LOG(ERROR) << "Response of the sendData is wrong: temp_response size = apdu status = " << getApduStatus(temp_response);
            return (KM_ERROR_UNKNOWN_ERROR);
        }
    } while (currentIndex < inData.size());
    // Response size should be greater than 2. Cbor output data followed by two bytes of APDU
    // status.
    if (temp_response.size() <= 2){
        LOG(ERROR) << "Response of the sendData is wrong: temp_response size = " << temp_response.size();
        return (KM_ERROR_UNKNOWN_ERROR);
    }
    // remove the status bytes
    temp_response.pop_back();
    temp_response.pop_back();
    response.insert(response.end(), temp_response.begin(), temp_response.end());

    return (KM_ERROR_OK);  // success
}

std::tuple<std::unique_ptr<Item>, keymaster_error_t>
JavacardSecureElement::sendRequest(Instruction ins, Array& request) {
    vector<uint8_t> response;
    // encode request
    std::vector<uint8_t> command = request.encode();
    auto sendError = sendData(ins, command, response);
    if (sendError != KM_ERROR_OK) {
        return {unique_ptr<Item>(nullptr), sendError};
    }
    // decode the response and send that back
    return cbor_.decodeData(response);
}

std::tuple<std::unique_ptr<Item>, keymaster_error_t>
JavacardSecureElement::sendRequest(Instruction ins, std::vector<uint8_t>& command) {
    vector<uint8_t> response;
    auto sendError = sendData(ins, command, response);
    if (sendError != KM_ERROR_OK) {
        return {unique_ptr<Item>(nullptr), sendError};
    }
    // decode the response and send that back
    return cbor_.decodeData(response);
}

std::tuple<std::unique_ptr<Item>, keymaster_error_t>
JavacardSecureElement::sendRequest(Instruction ins) {
    vector<uint8_t> response;
    vector<uint8_t> emptyRequest;
    auto sendError = sendData(ins, emptyRequest, response);
    if (sendError != KM_ERROR_OK) {
        return {unique_ptr<Item>(nullptr), sendError};
    }
    // decode the response and send that back
    return cbor_.decodeData(response);
}

}  // namespace keymint::javacard
