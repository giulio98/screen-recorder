#include "decoder.h"

#include <stdexcept>

static void throw_error(const std::string &msg) { throw std::runtime_error("Decoder: " + msg); }

Decoder::Decoder(const AVCodecParameters *params) : codec_(nullptr) {
    codec_ = avcodec_find_decoder(params->codec_id);
    if (!codec_) throw_error("cannot find codec");

    codec_ctx_ = av::CodecContextUPtr(avcodec_alloc_context3(codec_));
    if (!codec_ctx_) throw_error("failed to allocated memory for AVCodecContext");

    if (avcodec_parameters_to_context(codec_ctx_.get(), params) < 0)
        throw_error("failed to copy codec params to codec context");

    if (avcodec_open2(codec_ctx_.get(), codec_, nullptr) < 0) throw_error("unable to open the av codec");
}

bool Decoder::sendPacket(const AVPacket *packet) const {
    int ret = avcodec_send_packet(codec_ctx_.get(), packet);
    if (ret == AVERROR(EAGAIN)) return false;
    if (ret == AVERROR_EOF) throw_error("has already been flushed");
    if (ret < 0) throw_error("failed to send packet to decoder");
    return true;
}

av::FrameUPtr Decoder::getFrame() const {
    av::FrameUPtr frame(av_frame_alloc());
    if (!frame) throw_error("failed to allocate frame");

    int ret = avcodec_receive_frame(codec_ctx_.get(), frame.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return nullptr;
    if (ret < 0) throw_error("failed to receive frame from decoder");
    return frame;
}

const AVCodecContext *Decoder::getCodecContext() const { return codec_ctx_.get(); }