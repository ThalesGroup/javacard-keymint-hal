#define LOG_TAG "javacard.strongbox.keymint.operation-impl"
#include "JavacardSharedSecret.h"

#include <android-base/logging.h>

#include <KeyMintUtils.h>

namespace aidl::android::hardware::security::sharedsecret {
using ::keymint::javacard::Instruction;

ScopedAStatus JavacardSharedSecret::getSharedSecretParameters(SharedSecretParameters* params) {
    auto error = card_->initializeJavacard();
    if (error != KM_ERROR_OK) {
        LOG(ERROR) << "Error in initializing javacard.";
        return keymint::km_utils::kmError2ScopedAStatus(error);
    }
    auto [item, err] = card_->sendRequest(Instruction::INS_GET_SHARED_SECRET_PARAM_CMD);
    if (err != KM_ERROR_OK) {
        LOG(ERROR) << "Error in sending in getSharedSecretParameters.";
        return keymint::km_utils::kmError2ScopedAStatus(err);
    }
    auto optSSParams = cbor_.getSharedSecretParameters(item, 1);
    if (!optSSParams) {
        LOG(ERROR) << "Error in sending in getSharedSecretParameters.";
        return keymint::km_utils::kmError2ScopedAStatus(KM_ERROR_UNKNOWN_ERROR);
    }
    *params = std::move(optSSParams.value());
    return ScopedAStatus::ok();
}

ScopedAStatus
JavacardSharedSecret::computeSharedSecret(const std::vector<SharedSecretParameters>& params,
                                          std::vector<uint8_t>* secret) {
    card_->sendPendingEvents();
    auto error = card_->initializeJavacard();
    if (error != KM_ERROR_OK) {
        LOG(ERROR) << "Error in initializing javacard.";
        return keymint::km_utils::kmError2ScopedAStatus(error);
    }
    cppbor::Array request;
    cbor_.addSharedSecretParameters(request, params);
    auto [item, err] = card_->sendRequest(Instruction::INS_COMPUTE_SHARED_SECRET_CMD, request);
    if (err != KM_ERROR_OK) {
        LOG(ERROR) << "Error in sending in computeSharedSecret.";
        return keymint::km_utils::kmError2ScopedAStatus(err);
    }
    auto optSecret = cbor_.getByteArrayVec(item, 1);
    if (!optSecret) {
        LOG(ERROR) << "Error in decoding the response in computeSharedSecret.";
        return keymint::km_utils::kmError2ScopedAStatus(KM_ERROR_UNKNOWN_ERROR);
    }
    *secret = std::move(optSecret.value());
    return ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::security::sharedsecret
