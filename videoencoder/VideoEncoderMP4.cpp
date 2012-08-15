/*
 INTEL CONFIDENTIAL
 Copyright 2011 Intel Corporation All Rights Reserved.
 The source code contained or described herein and all documents related to the source code ("Material") are owned by Intel Corporation or its suppliers or licensors. Title to the Material remains with Intel Corporation or its suppliers and licensors. The Material contains trade secrets and proprietary and confidential information of Intel or its suppliers and licensors. The Material is protected by worldwide copyright and trade secret laws and treaty provisions. No part of the Material may be used, copied, reproduced, modified, published, uploaded, posted, transmitted, distributed, or disclosed in any way without Intel’s prior express written permission.

 No license under any patent, copyright, trade secret or other intellectual property right is granted to or conferred upon you by disclosure or delivery of the Materials, either expressly, by implication, inducement, estoppel or otherwise. Any license under such intellectual property rights must be express and approved by Intel in writing.
 */

#include <string.h>
#include <stdlib.h>

#include "VideoEncoderLog.h"
#include "VideoEncoderMP4.h"
#include <va/va_tpi.h>

VideoEncoderMP4::VideoEncoderMP4()
    :mProfileLevelIndication(3)
    ,mFixedVOPTimeIncrement(0) {
    mComParams.profile = (VAProfile)PROFILE_MPEG4SIMPLE;
}

Encode_Status VideoEncoderMP4::getHeaderPos(
        uint8_t *inBuffer, uint32_t bufSize, uint32_t *headerSize) {

    uint8_t *buf = inBuffer;
    uint32_t bytesLeft = bufSize;
    Encode_Status ret = ENCODE_SUCCESS;

    *headerSize = 0;
    CHECK_NULL_RETURN_IFFAIL(inBuffer);

    if (bufSize < 4) {
        //bufSize shoule not < 4
        LOG_E("Buffer size too small\n");
        return ENCODE_FAIL;
    }

    while (bytesLeft > 4  &&
            (memcmp("\x00\x00\x01\xB6", &inBuffer[bufSize - bytesLeft], 4) &&
             memcmp("\x00\x00\x01\xB3", &inBuffer[bufSize - bytesLeft], 4))) {
        --bytesLeft;
    }

    if (bytesLeft <= 4) {
        LOG_E("NO header found\n");
        *headerSize = 0; //
    } else {
        *headerSize = bufSize - bytesLeft;
    }

    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderMP4::outputConfigData(
        VideoEncOutputBuffer *outBuffer) {

    Encode_Status ret = ENCODE_SUCCESS;
    uint32_t headerSize = 0;

    ret = getHeaderPos((uint8_t *)mCurSegment->buf + mOffsetInSeg,
            mCurSegment->size - mOffsetInSeg, &headerSize);
    CHECK_ENCODE_STATUS_RETURN("getHeaderPos");
    if (headerSize == 0) {
        outBuffer->dataSize = 0;
        mCurSegment = NULL;
        return ENCODE_NO_REQUEST_DATA;
    }

    if (headerSize <= outBuffer->bufferSize) {
        memcpy(outBuffer->data, (uint8_t *)mCurSegment->buf + mOffsetInSeg, headerSize);
        mTotalSizeCopied += headerSize;
        mOffsetInSeg += headerSize;
        outBuffer->dataSize = headerSize;
        outBuffer->remainingSize = 0;
        outBuffer->flag |= ENCODE_BUFFERFLAG_ENDOFFRAME;
        outBuffer->flag |= ENCODE_BUFFERFLAG_CODECCONFIG;
        outBuffer->flag |= ENCODE_BUFFERFLAG_SYNCFRAME;
    } else {
        // we need a big enough buffer, otherwise we won't output anything
        outBuffer->dataSize = 0;
        outBuffer->remainingSize = headerSize;
        outBuffer->flag |= ENCODE_BUFFERFLAG_DATAINVALID;
        LOG_E("Buffer size too small\n");
        return ENCODE_BUFFER_TOO_SMALL;
    }

    return ret;
}


Encode_Status VideoEncoderMP4::getOutput(VideoEncOutputBuffer *outBuffer) {

    Encode_Status ret = ENCODE_SUCCESS;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    bool useLocalBuffer = false;

    LOG_V("Begin\n");
    CHECK_NULL_RETURN_IFFAIL(outBuffer);

     setKeyFrame(mComParams.intraPeriod);

    // prepare for output, map the coded buffer
    ret = VideoEncoderBase::prepareForOutput(outBuffer, &useLocalBuffer);
    CHECK_ENCODE_STATUS_CLEANUP("prepareForOutput");

    switch (outBuffer->format) {
        case OUTPUT_EVERYTHING:
        case OUTPUT_FRAME_DATA: {
            // Output whatever we have
            ret = VideoEncoderBase::outputAllData(outBuffer);
            CHECK_ENCODE_STATUS_CLEANUP("outputAllData");
            break;
        }
        case OUTPUT_CODEC_DATA: {
            // Output the codec config data
            ret = outputConfigData(outBuffer);
            CHECK_ENCODE_STATUS_CLEANUP("outputCodecData");
            break;
        }
        default:
            LOG_E("Invalid buffer mode for MPEG-4:2\n");
            ret = ENCODE_FAIL;
            break;
    }

    LOG_I("out size is = %d\n", outBuffer->dataSize);

    // cleanup, unmap the coded buffer if all
    // data has been copied out
    ret = VideoEncoderBase::cleanupForOutput();

CLEAN_UP:

    if (ret < ENCODE_SUCCESS) {
        if (outBuffer->data && (useLocalBuffer == true)) {
            delete[] outBuffer->data;
            outBuffer->data = NULL;
            useLocalBuffer = false;
        }

        // error happens, unmap the buffer
        if (mCodedBufferMapped) {
            vaStatus = vaUnmapBuffer(mVADisplay, mOutCodedBuffer);
            mCodedBufferMapped = false;
            mCurSegment = NULL;
        }
    }
    LOG_V("End\n");
    return ret;
}


Encode_Status VideoEncoderMP4::renderSequenceParams() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAEncSequenceParameterBufferMPEG4 mp4SequenceParams;

    uint32_t frameRateNum = mComParams.frameRate.frameRateNum;
    uint32_t frameRateDenom = mComParams.frameRate.frameRateDenom;

    LOG_V( "Begin\n\n");

    // set up the sequence params for HW
    mp4SequenceParams.profile_and_level_indication = mProfileLevelIndication;
    mp4SequenceParams.video_object_layer_width= mComParams.resolution.width;
    mp4SequenceParams.video_object_layer_height= mComParams.resolution.height;
    mp4SequenceParams.vop_time_increment_resolution =
            (unsigned int) (frameRateNum + frameRateDenom /2) / frameRateDenom;
    mp4SequenceParams.fixed_vop_time_increment= mFixedVOPTimeIncrement;
    mp4SequenceParams.bits_per_second= mComParams.rcParams.bitRate;
    mp4SequenceParams.frame_rate =
            (unsigned int) (frameRateNum + frameRateDenom /2) / frameRateDenom;
    mp4SequenceParams.initial_qp = mComParams.rcParams.initQP;
    mp4SequenceParams.min_qp = mComParams.rcParams.minQP;
    mp4SequenceParams.intra_period = mComParams.intraPeriod;
    //mpeg4_seq_param.fixed_vop_rate = 30;

    LOG_V("===mpeg4 sequence params===\n");
    LOG_I("profile_and_level_indication = %d\n", (uint32_t)mp4SequenceParams.profile_and_level_indication);
    LOG_I("intra_period = %d\n", mp4SequenceParams.intra_period);
    LOG_I("video_object_layer_width = %d\n", mp4SequenceParams.video_object_layer_width);
    LOG_I("video_object_layer_height = %d\n", mp4SequenceParams.video_object_layer_height);
    LOG_I("vop_time_increment_resolution = %d\n", mp4SequenceParams.vop_time_increment_resolution);
    LOG_I("fixed_vop_rate = %d\n", mp4SequenceParams.fixed_vop_rate);
    LOG_I("fixed_vop_time_increment = %d\n", mp4SequenceParams.fixed_vop_time_increment);
    LOG_I("bitrate = %d\n", mp4SequenceParams.bits_per_second);
    LOG_I("frame_rate = %d\n", mp4SequenceParams.frame_rate);
    LOG_I("initial_qp = %d\n", mp4SequenceParams.initial_qp);
    LOG_I("min_qp = %d\n", mp4SequenceParams.min_qp);
    LOG_I("intra_period = %d\n\n", mp4SequenceParams.intra_period);

    vaStatus = vaCreateBuffer(
            mVADisplay, mVAContext,
            VAEncSequenceParameterBufferType,
            sizeof(mp4SequenceParams),
            1, &mp4SequenceParams,
            &mSeqParamBuf);
    CHECK_VA_STATUS_RETURN("vaCreateBuffer");

    vaStatus = vaRenderPicture(mVADisplay, mVAContext, &mSeqParamBuf, 1);
    CHECK_VA_STATUS_RETURN("vaRenderPicture");

    LOG_V( "end\n");
    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderMP4::renderPictureParams() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAEncPictureParameterBufferMPEG4 mpeg4_pic_param;
    LOG_V( "Begin\n\n");

    // set picture params for HW
    mpeg4_pic_param.reference_picture = mRefSurface;
    mpeg4_pic_param.reconstructed_picture = mRecSurface;
    mpeg4_pic_param.coded_buf = mVACodedBuffer[mCodedBufIndex];
    mpeg4_pic_param.picture_width = mComParams.resolution.width;
    mpeg4_pic_param.picture_height = mComParams.resolution.height;
    mpeg4_pic_param.vop_time_increment= mFrameNum;
    mpeg4_pic_param.picture_type = mIsIntra ? VAEncPictureTypeIntra : VAEncPictureTypePredictive;

    LOG_V("======mpeg4 picture params======\n");
    LOG_I("reference_picture = 0x%08x\n", mpeg4_pic_param.reference_picture);
    LOG_I("reconstructed_picture = 0x%08x\n", mpeg4_pic_param.reconstructed_picture);
    LOG_I("coded_buf = 0x%08x\n", mpeg4_pic_param.coded_buf);
    LOG_I("coded_buf_index = %d\n", mCodedBufIndex);
    LOG_I("picture_width = %d\n", mpeg4_pic_param.picture_width);
    LOG_I("picture_height = %d\n", mpeg4_pic_param.picture_height);
    LOG_I("vop_time_increment = %d\n", mpeg4_pic_param.vop_time_increment);
    LOG_I("picture_type = %d\n\n", mpeg4_pic_param.picture_type);

    vaStatus = vaCreateBuffer(
            mVADisplay, mVAContext,
            VAEncPictureParameterBufferType,
            sizeof(mpeg4_pic_param),
            1,&mpeg4_pic_param,
            &mPicParamBuf);
    CHECK_VA_STATUS_RETURN("vaCreateBuffer");

    vaStatus = vaRenderPicture(mVADisplay, mVAContext, &mPicParamBuf, 1);
    CHECK_VA_STATUS_RETURN("vaRenderPicture");

    return ENCODE_SUCCESS;
}


Encode_Status VideoEncoderMP4::renderSliceParams() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    uint32_t sliceHeight;
    uint32_t sliceHeightInMB;

    VAEncSliceParameterBuffer sliceParams;

    LOG_V( "Begin\n\n");

    sliceHeight = mComParams.resolution.height;
    sliceHeight += 15;
    sliceHeight &= (~15);
    sliceHeightInMB = sliceHeight / 16;

    sliceParams.start_row_number = 0;
    sliceParams.slice_height = sliceHeightInMB;
    sliceParams.slice_flags.bits.is_intra = mIsIntra;
    sliceParams.slice_flags.bits.disable_deblocking_filter_idc = 0;

    LOG_V("======mpeg4 slice params======\n");
    LOG_I( "start_row_number = %d\n", (int) sliceParams.start_row_number);
    LOG_I( "sliceHeightInMB = %d\n", (int) sliceParams.slice_height);
    LOG_I( "is_intra = %d\n", (int) sliceParams.slice_flags.bits.is_intra);

    vaStatus = vaCreateBuffer(
            mVADisplay, mVAContext,
            VAEncSliceParameterBufferType,
            sizeof(VAEncSliceParameterBuffer),
            1, &sliceParams,
            &mSliceParamBuf);
    CHECK_VA_STATUS_RETURN("vaCreateBuffer");

    vaStatus = vaRenderPicture(mVADisplay, mVAContext, &mSliceParamBuf, 1);
    CHECK_VA_STATUS_RETURN("vaRenderPicture");

    LOG_V( "end\n");
    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderMP4::sendEncodeCommand(void) {
    Encode_Status ret = ENCODE_SUCCESS;
    LOG_V( "Begin\n");

    if (mFrameNum == 0) {
        ret = renderSequenceParams();
        CHECK_ENCODE_STATUS_RETURN("renderSequenceParams");
    }

    ret = renderPictureParams();
    CHECK_ENCODE_STATUS_RETURN("renderPictureParams");

    ret = renderSliceParams();
    CHECK_ENCODE_STATUS_RETURN("renderPictureParams");

    LOG_V( "End\n");
    return ENCODE_SUCCESS;
}
