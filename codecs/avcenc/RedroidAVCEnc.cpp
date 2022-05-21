/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "RedroidAVCEnc"
#include <utils/Log.h>
#include <utils/misc.h>

#include "OMX_Video.h"

#include <media/hardware/HardwareAPI.h>
#include <media/hardware/MetadataBufferType.h>
#include <media/stagefright/foundation/ADebug.h>
#include <OMX_IndexExt.h>
#include <OMX_VideoExt.h>

#include <nativebase/nativebase.h>
#include <dlfcn.h>

#include "RedroidAVCEnc.h"

#define d(...) ALOGD(__VA_ARGS__)
#define i(...) ALOGI(__VA_ARGS__)
#define e(...) ALOGE(__VA_ARGS__)

namespace android {


    static ANativeWindowBuffer *getANWBuffer(void *src, size_t srcSize) {
        MetadataBufferType bufferType = *(MetadataBufferType *)src;
        bool usingANWBuffer = bufferType == kMetadataBufferTypeANWBuffer;
        if (!usingANWBuffer && bufferType != kMetadataBufferTypeGrallocSource) {
            ALOGE("Unsupported metadata type (%d)", bufferType);
            return NULL;
        }

        if (usingANWBuffer) {
            if (srcSize < sizeof(VideoNativeMetadata)) {
                ALOGE("Metadata is too small (%zu vs %zu)", srcSize, sizeof(VideoNativeMetadata));
                return NULL;
            }

            VideoNativeMetadata &nativeMeta = *(VideoNativeMetadata *)src;
            return nativeMeta.pBuffer;
        }

        return nullptr;
    }

    static const CodecProfileLevel kProfileLevels[] = {
        { OMX_VIDEO_AVCProfileConstrainedBaseline, OMX_VIDEO_AVCLevel41 },

        { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel41 },

        { OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel41 },
    };

    RedroidAVCEnc::RedroidAVCEnc(
            const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component)
        : RedroidVideoEncoderOMXComponent(
                name, "video_encoder.avc", OMX_VIDEO_CodingAVC,
                kProfileLevels, NELEM(kProfileLevels),
                176 /* width */, 144 /* height */,
                callbacks, appData, component),
        mUpdateFlag(0),
        mStarted(false),
        mCodecLibHandle(nullptr),
        mCodec(nullptr),
        mCodecCtx(nullptr),
        mSawInputEOS(false),
        mSawOutputEOS(false),
        mSignalledError(false) {

            initPorts(kNumBuffers, kNumBuffers, ((mWidth * mHeight * 3) >> 1),
                    "video/avc", 2);

            // If dump is enabled, then open create an empty file
            GENERATE_FILE_NAMES();
            CREATE_DUMP_FILE(mInFile);
            CREATE_DUMP_FILE(mOutFile);
            memset(mConversionBuffers, 0, sizeof(mConversionBuffers));
            memset(mInputBufferInfo, 0, sizeof(mInputBufferInfo));

        }

    RedroidAVCEnc::~RedroidAVCEnc() {
        d(__func__);
        releaseEncoder();
        List<BufferInfo *> &outQueue = getPortQueue(1);
        List<BufferInfo *> &inQueue = getPortQueue(0);
        CHECK(outQueue.empty());
        CHECK(inQueue.empty());
    }

    OMX_ERRORTYPE RedroidAVCEnc::setBitRate() {
        ALOGW("TODO unsupported setBitRate()");
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE RedroidAVCEnc::initEncoder() {
        mCodecLibHandle = dlopen("libmedia_codec.so", RTLD_NOW|RTLD_NODELETE);
        CHECK(mCodecLibHandle);

        // extern "C" media_codec_t *get_media_codec()
        typedef media_codec_t *(*get_media_codec_func)(); 
        get_media_codec_func func = (get_media_codec_func) dlsym(mCodecLibHandle, "get_media_codec");
        CHECK(func);

        mCodec = func();
        CHECK(mCodec);

        mCodecCtx = mCodec->codec_alloc(H264_ENCODE, nullptr);
        CHECK(mCodecCtx);
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE RedroidAVCEnc::releaseEncoder() {
        d(__func__);
        if (mCodecCtx) {
            mCodec->codec_free(mCodecCtx);
            mCodecCtx = nullptr;
        }

        if (mCodecLibHandle) {
            dlclose(mCodecLibHandle);
            mCodecLibHandle = nullptr;
            mCodec = nullptr;
        }
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE RedroidAVCEnc::internalGetParameter(OMX_INDEXTYPE index, OMX_PTR params) {
        switch (index) {
            case OMX_IndexParamVideoBitrate:
                {
                    OMX_VIDEO_PARAM_BITRATETYPE *bitRate =
                        (OMX_VIDEO_PARAM_BITRATETYPE *)params;

                    if (!isValidOMXParam(bitRate)) {
                        return OMX_ErrorBadParameter;
                    }

                    if (bitRate->nPortIndex != 1) {
                        return OMX_ErrorUndefined;
                    }

                    bitRate->eControlRate = OMX_Video_ControlRateVariable;
                    bitRate->nTargetBitrate = mBitrate;
                    return OMX_ErrorNone;
                }

            case OMX_IndexParamVideoAvc:
                {
                    OMX_VIDEO_PARAM_AVCTYPE *avcParams = (OMX_VIDEO_PARAM_AVCTYPE *)params;

                    if (!isValidOMXParam(avcParams)) {
                        return OMX_ErrorBadParameter;
                    }

                    if (avcParams->nPortIndex != 1) {
                        return OMX_ErrorUndefined;
                    }

                    // TODO: maintain profile
                    avcParams->eProfile = (OMX_VIDEO_AVCPROFILETYPE)OMX_VIDEO_AVCProfileConstrainedBaseline;
                    avcParams->eLevel = OMX_VIDEO_AVCLevel41;
                    avcParams->nRefFrames = 1;
                    avcParams->bUseHadamard = OMX_TRUE;
                    avcParams->nAllowedPictureTypes = (OMX_VIDEO_PictureTypeI
                            | OMX_VIDEO_PictureTypeP | OMX_VIDEO_PictureTypeB);
                    avcParams->nRefIdx10ActiveMinus1 = 0;
                    avcParams->nRefIdx11ActiveMinus1 = 0;
                    avcParams->bWeightedPPrediction = OMX_FALSE;
                    avcParams->bconstIpred = OMX_FALSE;
                    avcParams->bDirect8x8Inference = OMX_FALSE;
                    avcParams->bDirectSpatialTemporal = OMX_FALSE;
                    avcParams->nCabacInitIdc = 0;
                    return OMX_ErrorNone;
                }

            default:
                return RedroidVideoEncoderOMXComponent::internalGetParameter(index, params);
        }
    }

    OMX_ERRORTYPE RedroidAVCEnc::internalSetParameter(OMX_INDEXTYPE index, const OMX_PTR params) {
        int32_t indexFull = index;

        switch (indexFull) {
            case OMX_IndexParamVideoBitrate:
                {
                    OMX_VIDEO_PARAM_BITRATETYPE *bitRate =
                        (OMX_VIDEO_PARAM_BITRATETYPE *)params;

                    if (!isValidOMXParam(bitRate)) {
                        return OMX_ErrorBadParameter;
                    }

                    return internalSetBitrateParams(bitRate);
                }

            case OMX_IndexParamVideoAvc:
                {
                    ALOGW("TODO unsupported settings [OMX_IndexParamVideoAvc]");
                    return OMX_ErrorNone;
                }

            default:
                return RedroidVideoEncoderOMXComponent::internalSetParameter(index, params);
        }
    }

    OMX_ERRORTYPE RedroidAVCEnc::getConfig(
            OMX_INDEXTYPE index, OMX_PTR _params) {
        switch ((int)index) {
            case OMX_IndexConfigAndroidIntraRefresh:
                {
                    OMX_VIDEO_CONFIG_ANDROID_INTRAREFRESHTYPE *intraRefreshParams =
                        (OMX_VIDEO_CONFIG_ANDROID_INTRAREFRESHTYPE *)_params;

                    if (!isValidOMXParam(intraRefreshParams)) {
                        return OMX_ErrorBadParameter;
                    }

                    if (intraRefreshParams->nPortIndex != kOutputPortIndex) {
                        return OMX_ErrorUndefined;
                    }

                    intraRefreshParams->nRefreshPeriod = 0;
                    return OMX_ErrorNone;
                }

            default:
                return RedroidVideoEncoderOMXComponent::getConfig(index, _params);
        }
    }

    OMX_ERRORTYPE RedroidAVCEnc::internalSetConfig(
            OMX_INDEXTYPE index, const OMX_PTR _params, bool *frameConfig) {
        switch ((int)index) {
            case OMX_IndexConfigVideoIntraVOPRefresh:
                {
                    OMX_CONFIG_INTRAREFRESHVOPTYPE *params =
                        (OMX_CONFIG_INTRAREFRESHVOPTYPE *)_params;

                    if (!isValidOMXParam(params)) {
                        return OMX_ErrorBadParameter;
                    }

                    if (params->nPortIndex != kOutputPortIndex) {
                        return OMX_ErrorBadPortIndex;
                    }

                    if (params->IntraRefreshVOP) {
                        mUpdateFlag |= kRequestKeyFrame;
                    }
                    return OMX_ErrorNone;
                }

            case OMX_IndexConfigVideoBitrate:
                {
                    OMX_VIDEO_CONFIG_BITRATETYPE *params =
                        (OMX_VIDEO_CONFIG_BITRATETYPE *)_params;

                    if (!isValidOMXParam(params)) {
                        return OMX_ErrorBadParameter;
                    }

                    if (params->nPortIndex != kOutputPortIndex) {
                        return OMX_ErrorBadPortIndex;
                    }

                    if (mBitrate != params->nEncodeBitrate) {
                        mBitrate = params->nEncodeBitrate;
                        mUpdateFlag |= kUpdateBitrate;
                    }
                    return OMX_ErrorNone;
                }

            case OMX_IndexConfigAndroidIntraRefresh:
                {
                    const OMX_VIDEO_CONFIG_ANDROID_INTRAREFRESHTYPE *intraRefreshParams =
                        (const OMX_VIDEO_CONFIG_ANDROID_INTRAREFRESHTYPE *)_params;

                    if (!isValidOMXParam(intraRefreshParams)) {
                        return OMX_ErrorBadParameter;
                    }

                    if (intraRefreshParams->nPortIndex != kOutputPortIndex) {
                        return OMX_ErrorUndefined;
                    }

                    return OMX_ErrorNone;
                }

            default:
                return SimpleRedroidOMXComponent::internalSetConfig(index, _params, frameConfig);
        }
    }

    OMX_ERRORTYPE RedroidAVCEnc::internalSetBitrateParams(
            const OMX_VIDEO_PARAM_BITRATETYPE *bitrate) {
        if (bitrate->nPortIndex != kOutputPortIndex) {
            return OMX_ErrorUnsupportedIndex;
        }

        mBitrate = bitrate->nTargetBitrate;
        mUpdateFlag |= kUpdateBitrate;

        return OMX_ErrorNone;
    }

    void RedroidAVCEnc::onQueueFilled(OMX_U32 portIndex) {
        UNUSED(portIndex);

        if (mSignalledError) {
            return;
        }

        if (!mCodecCtx) initEncoder();

        List<BufferInfo *> &inQueue = getPortQueue(0);
        List<BufferInfo *> &outQueue = getPortQueue(1);

        if (outQueue.empty()) {
            ALOGW("outQueue is empty");
            return;
        }

        BufferInfo *outputBufferInfo = *outQueue.begin();
        OMX_BUFFERHEADERTYPE *outputBufferHeader = outputBufferInfo->mHeader;

        BufferInfo *inputBufferInfo;
        OMX_BUFFERHEADERTYPE *inputBufferHeader;

        if (mSawInputEOS) {
            inputBufferHeader = NULL;
            inputBufferInfo = NULL;
        } else if (!inQueue.empty()) {
            inputBufferInfo = *inQueue.begin();
            inputBufferHeader = inputBufferInfo->mHeader;
        } else {
            return;
        }

        outputBufferHeader->nTimeStamp = 0;
        outputBufferHeader->nFlags = 0;
        outputBufferHeader->nOffset = 0;
        outputBufferHeader->nFilledLen = 0;
        outputBufferHeader->nOffset = 0;

        if (inputBufferHeader != NULL) {
            outputBufferHeader->nFlags = inputBufferHeader->nFlags;
        }


        if (mUpdateFlag) {
            if (mUpdateFlag & kUpdateBitrate) {
                setBitRate();
            }
            if (mUpdateFlag & kRequestKeyFrame) {
                mCodec->request_key_frame(mCodecCtx);
            }
            mUpdateFlag = 0;
        }

        if ((inputBufferHeader != NULL)
                && (inputBufferHeader->nFlags & OMX_BUFFERFLAG_EOS)) {
            mSawInputEOS = true;
        }

        if ((inputBufferHeader != NULL) && inputBufferHeader->nFilledLen) {
            OMX_ERRORTYPE error = validateInputBuffer(inputBufferHeader);
            if (error != OMX_ErrorNone) {
                ALOGE("b/69065651");
                android_errorWriteLog(0x534e4554, "69065651");
                return;
            }

            if (mInputDataIsMeta) {
                ANativeWindowBuffer *buffer = getANWBuffer(inputBufferHeader->pBuffer + inputBufferHeader->nOffset,
                        inputBufferHeader->nFilledLen);
                if (!buffer) {
                    ALOGE("ANativeWindowBuffer null!");
                    return;
                }

                buffer_handle_t handle = buffer->handle;
                if (!handle) {
                    ALOGE("buffer_handle_t handle is null!");
                    return;
                } 
                ALOGV("before encode");
                void *out_buf = outputBufferHeader->pBuffer + outputBufferHeader->nFilledLen;
                int out_size = 0;
                mCodec->encode_frame(mCodecCtx, (void *) handle, out_buf, &out_size);
                outputBufferHeader->nFilledLen += out_size;
                ALOGV("after encode");
            }

            //DUMP_TO_FILE();
        }

        if (inputBufferHeader != NULL) {
            inQueue.erase(inQueue.begin());

            /* If in meta data, call EBD on input */
            /* In case of normal mode, EBD will be done once encoder
               releases the input buffer */
            if (mInputDataIsMeta) {
                inputBufferInfo->mOwnedByUs = false;
                notifyEmptyBufferDone(inputBufferHeader);
            }
        }

        if (false) { // TODO
            outputBufferHeader->nFlags |= OMX_BUFFERFLAG_EOS;
            mSawOutputEOS = true;
        } else {
            outputBufferHeader->nFlags &= ~OMX_BUFFERFLAG_EOS;
        }

        if (outputBufferHeader->nFilledLen) {
            if (inputBufferHeader) outputBufferHeader->nTimeStamp = inputBufferHeader->nTimeStamp;

            outputBufferInfo->mOwnedByUs = false;
            outQueue.erase(outQueue.begin());
            DUMP_TO_FILE(mOutFile, outputBufferHeader->pBuffer,
                    outputBufferHeader->nFilledLen);
            notifyFillBufferDone(outputBufferHeader);
        }
    }

    void RedroidAVCEnc::onReset() {
        d(__func__);
        RedroidVideoEncoderOMXComponent::onReset();

        if (releaseEncoder() != OMX_ErrorNone) {
            ALOGW("releaseEncoder failed");
        }
    }
}  // namespace android

extern "C"
android::RedroidOMXComponent *createRedroidOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::RedroidAVCEnc(name, callbacks, appData, component);
}
