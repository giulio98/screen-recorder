#pragma once

#include <memory>

#include "common.h"
#include "converter.h"

/**
 * @details convert video frames to the output pix format and crop them
 */
class VideoConverter : public Converter {
    int in_width_;
    int in_height_;
    int out_width_;
    int out_height_;
    int offset_x_;
    int offset_y_;
    AVPixelFormat out_pix_fmt_;
    av::SwsContextUPtr scale_ctx_;
    av::FilterGraphUPtr filter_graph_;
    AVFilterContext *buffersrc_ctx_;
    AVFilterContext *buffersink_ctx_;

public:
    /**
     * Create a new video converter
     * @param in_codec_ctx the codec context containing the input params (from the decoder)
     * @param out_codec_ctx the codec context containing the output params (from the encoder)
     * @param offset_x the horizontal offset to use when performing the cropping of the frames
     * @param offset_y the vertical offset to use when performing the cropping of the frames
     */
    VideoConverter(const AVCodecContext *in_codec_ctx, const AVCodecContext *out_codec_ctx, int offset_x, int offset_y);

    /**
     * Send a frame to convert
     * @return always true (the return type is there just for compatibility reasons)
     */
    bool sendFrame(const AVFrame *frame) const override;

    /**
     * Get a converted frame
     * @param frame_number the frame's sequence number to use to compute the PTS
     * @return a new converted frame if it was possible to build it, nullptr otherwise
     */
    [[nodiscard]] av::FrameUPtr getFrame(int64_t frame_number = 0) const override;
};