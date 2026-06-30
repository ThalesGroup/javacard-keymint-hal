#define LOG_TAG "javacard.strongbox.keymint.operation-impl"
#include "JavacardSharedSecret.h"

#include <android-base/logging.h>

#include <KeyMintUtils.h>
#define MAX_GET_SHARED_PARAM_RETRIES 60

namespace aidl::android::hardware::security::sharedsecret {
using ::keymint::javacard::Instruction;

static uint8_t sharedSecretParamCounter = 0;

ScopedAStatus JavacardSharedSecret::getSharedSecretParameters(SharedSecretParameters* params) {
    std::vector<uint8_t> opt_seed;
    std::vector<uint8_t> opt_nonce(32, 0);

    if (sharedSecretParamCounter >= MAX_GET_SHARED_PARAM_RETRIES) {
        LOG(ERROR) << "Maximum retry attempts reached for getSharedSecretParameters.";
        params->seed.clear();
        params->nonce.assign(opt_nonce.begin(), opt_nonce.end());
        sharedSecretParamCounter = 0;
		return ScopedAStatus::ok();
    }

    auto error = card_->initializeJavacard();
    if (error != KM_ERROR_OK) {
        LOG(ERROR) << "Error in initializing javacard.";
        sharedSecretParamCounter++;
        return keymint::km_utils::kmError2ScopedAStatus(error);
    }
    auto [item, err] = card_->sendRequest(Instruction::INS_GET_SHARED_SECRET_PARAM_CMD);
    if (err != KM_ERROR_OK) {
        LOG(ERROR) << "Error in sending in getSharedSecretParameters.";
        sharedSecretParamCounter++;
        return keymint::km_utils::kmError2ScopedAStatus(err);
    }
    auto optSSParams = cbor_.getSharedSecretParameters(item, 1);
    if (!optSSParams) {
        LOG(ERROR) << "Error in sending in getSharedSecretParameters.";
        sharedSecretParamCounter++;
        return keymint::km_utils::kmError2ScopedAStatus(KM_ERROR_UNKNOWN_ERROR);
    }

    sharedSecretParamCounter = 0;
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
