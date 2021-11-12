#include "../include/screen_recorder.h"

#include <stdio.h>
#include <stdlib.h>

#include <sstream>
#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#endif

#include "../include/duration_logger.h"

/* initialize the resources*/
ScreenRecorder::ScreenRecorder() {}

/* uninitialize the resources */
ScreenRecorder::~ScreenRecorder() {
    avformat_close_input(&in_fmt_ctx_);
    if (in_fmt_ctx_) {
        std::cout << "\nunable to close the file";
        exit(1);
    }

    avformat_free_context(in_fmt_ctx_);
    if (in_fmt_ctx_) {
        std::cout << "unable to free avformat context" << std::endl;
        exit(1);
    }

    if (recorder_thread_.joinable() == true) {
        recorder_thread_.join();
    }

    /* TO-DO: free all data structures */
}

void ScreenRecorder::Start(const std::string &output_file, bool audio) {
    output_file_ = output_file;
    stop_capture_ = false;
    paused_ = false;
    video_framerate_ = 30;
    out_video_pix_fmt_ = AV_PIX_FMT_YUV420P;
    out_video_codec_id_ = AV_CODEC_ID_H264;
    record_audio_ = audio;

    avdevice_register_all();

    if (SelectArea()) {
        std::cerr << "Error in SelectArea" << std::endl;
        exit(1);
    }

    video_device_options_ = NULL;
    if (SetVideoDeviceOptions()) {
        std::cerr << "Error in setting video options" << std::endl;
        exit(1);
    };

    if (OpenInputDevices()) {
        std::cerr << "Error in OpenInputDevice" << std::endl;
        exit(1);
    }

    if (InitOutputFile()) {
        std::cerr << "Error in InitOutputFile" << std::endl;
        exit(1);
    }

    // auto video_fun = [this]() {
    //     std::cout << "Recording..." << std::endl;
    //     this->CaptureFrames();
    // };

    recorder_thread_ = std::thread([this]() {
        std::cout << "Recording..." << std::endl;
        this->CaptureFrames();
    });

    std::cout << "All required functions have been successfully registered" << std::endl;
}

void ScreenRecorder::Stop() {
    {
        std::unique_lock<std::mutex> ul{mutex_};
        this->stop_capture_ = true;
        this->paused_ = false;
        cv_.notify_all();
    }

    avformat_close_input(&in_fmt_ctx_);
    if (in_fmt_ctx_) {
        std::cerr << "Unable to close the file" << std::endl;
        exit(1);
    }

    avformat_free_context(in_fmt_ctx_);
    if (in_fmt_ctx_) {
        std::cerr << "Unable to freed avformat context" << std::endl;
        exit(1);
    }

#ifdef __linux__
    avformat_close_input(&in_audio_fmt_ctx_);
    if (in_fmt_ctx_) {
        std::cerr << "Unable to close the file" << std::endl;
        exit(1);
    }

    avformat_free_context(in_audio_fmt_ctx_);
    if (in_fmt_ctx_) {
        std::cerr << "Unable to freed avformat context" << std::endl;
        exit(1);
    }
#endif

    av_dict_free(&video_device_options_);

    if (recorder_thread_.joinable() == true) {
        recorder_thread_.join();
    }
}

void ScreenRecorder::Pause() {
    std::unique_lock<std::mutex> ul{mutex_};
    this->paused_ = true;
    std::cout << "Recording paused" << std::endl;
    cv_.notify_all();
}

void ScreenRecorder::Resume() {
    std::unique_lock<std::mutex> ul{mutex_};
    this->paused_ = false;
    std::cout << "Recording resumed" << std::endl;
    cv_.notify_all();
}

int ScreenRecorder::InitDecoder(int audio_video) {
    int ret;
    AVCodec *&codec = audio_video ? in_audio_codec_ : in_video_codec_;
    AVCodecContext *&codec_ctx = audio_video ? in_audio_codec_ctx_ : in_video_codec_ctx_;
    AVCodecParameters *codec_params = audio_video ? in_audio_stream_->codecpar : in_video_stream_->codecpar;

    codec = avcodec_find_decoder(codec_params->codec_id);
    if (codec == NULL) {
        std::cerr << "Unable to find the video decoder" << std::endl;
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Failed to allocated memory for AVCodecContext" << std::endl;
        return -1;
    }

    // Fill the codec context based on the values from the supplied codec parameters
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        std::cerr << "Failed to copy codec params to codec context" << std::endl;
        return -1;
    }

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        std::cerr << "Unable to open the av codec" << std::endl;
        return -1;
    }

    return 0;
}

int ScreenRecorder::SetVideoDeviceOptions() {
    char str[20];
    int ret;

    if (video_device_options_) {
        std::cerr << "video_device_options_ is not NULL (it may be already set)" << std::endl;
        return -1;
    }

    sprintf(str, "%d", video_framerate_);
    ret = av_dict_set(&video_device_options_, "framerate", str, 0);
    if (ret < 0) {
        std::cerr << "Error in setting framerate" << std::endl;
        return -1;
    }

    sprintf(str, "%dx%d", width_, height_);
    ret = av_dict_set(&video_device_options_, "video_size", str, 0);
    if (ret < 0) {
        std::cerr << "Error in setting video_size" << std::endl;
        return -1;
    }

#ifdef __linux__
    ret = av_dict_set(&video_device_options_, "show_region", "1", 0);
    if (ret < 0) {
        std::cerr << "Error in setting show_region" << std::endl;
        return -1;
    }
#else
    ret = av_dict_set(&video_device_options_, "capture_cursor", "1", 0);
    if (ret < 0) {
        std::cerr << "Error in setting capture_cursor" << std::endl;
        return -1;
    }
#endif

    return 0;
}

int ScreenRecorder::InitVideoConverter() {
    video_converter_ctx_ =
        sws_getContext(in_video_codec_ctx_->width, in_video_codec_ctx_->height, in_video_codec_ctx_->pix_fmt,
                       out_video_codec_ctx_->width, out_video_codec_ctx_->height, out_video_codec_ctx_->pix_fmt,
                       SWS_BICUBIC, NULL, NULL, NULL);

    if (!video_converter_ctx_) {
        std::cerr << "Cannot allocate video_converter_ctx_";
        return -1;
    }

    return 0;
}

int ScreenRecorder::InitAudioConverter() {
    int ret;
    int fifo_duration = 2;  // How many seconds of audio to store in the FIFO buffer

    audio_converter_ctx_ = swr_alloc_set_opts(
        nullptr, av_get_default_channel_layout(in_audio_codec_ctx_->channels), out_audio_codec_ctx_->sample_fmt,
        in_audio_codec_ctx_->sample_rate, av_get_default_channel_layout(in_audio_codec_ctx_->channels),
        (AVSampleFormat)in_audio_stream_->codecpar->format, in_audio_stream_->codecpar->sample_rate, 0, nullptr);

    if (!audio_converter_ctx_) {
        std::cerr << "Error allocating audio converter";
        return -1;
    }

    ret = swr_init(audio_converter_ctx_);
    if (ret < 0) {
        std::cerr << "Error initializing audio FIFO buffer";
        return -1;
    }

    audio_fifo_buf_ = av_audio_fifo_alloc(out_audio_codec_ctx_->sample_fmt, in_audio_codec_ctx_->channels,
                                          in_audio_codec_ctx_->sample_rate * fifo_duration);

    if (!audio_converter_ctx_) {
        std::cerr << "Error allocating audio converter";
        return -1;
    }

    return 0;
}

int ScreenRecorder::OpenInputDevice(AVFormatContext *&in_fmt_ctx, AVInputFormat *in_fmt, const std::string &device_name,
                                    AVDictionary **options) {
    int ret;
    in_fmt_ctx = NULL;

    ret = avformat_open_input(&in_fmt_ctx, device_name.c_str(), in_fmt, options);
    if (ret != 0) {
        std::cerr << "\nerror in opening input device";
        exit(1);
    }

    ret = avformat_find_stream_info(in_fmt_ctx, NULL);
    if (ret < 0) {
        std::cerr << "\nunable to find the stream information";
        exit(1);
    }

    for (int i = 0; i < in_fmt_ctx->nb_streams; i++) {
        AVStream *stream = in_fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            in_video_stream_ = stream;
            if (InitDecoder(0)) {
                std::cerr << "Cannot Initialize in_video_codec_ctx";
                exit(1);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            in_audio_stream_ = stream;
            if (InitDecoder(1)) {
                std::cerr << "Cannot Initialize in_audio_codec_ctx";
                exit(1);
            }
        }
    }

    av_dump_format(in_fmt_ctx, 0, device_name.c_str(), 0);

    return 0;
}

int ScreenRecorder::OpenInputDevices() {
    std::stringstream ss;

    in_video_stream_ = NULL;
    in_audio_stream_ = NULL;

#ifdef __linux__
    char video_device_name[20];
    char *display = getenv("DISPLAY");
    sprintf(video_device_name, "%s.0+%d,%d", display, offset_x_, offset_y_);
    OpenInputDevice(in_fmt_ctx_, av_find_input_format("x11grab"), video_device_name, &video_device_options_);
    if (record_audio_) OpenInputDevice(in_audio_fmt_ctx_, av_find_input_format("pulse"), "default", NULL);
#else
    ss << "1:";
    if (record_audio_) ss << "0";
    OpenInputDevice(in_fmt_ctx_, av_find_input_format("avfoundation"), ss.str(), &video_device_options_);
#endif

    if (!in_video_stream_) {
        std::cerr << "Unable to find the video stream index. (-1)" << std::endl;
        exit(1);
    }

    if (record_audio_ && !in_audio_stream_) {
        std::cerr << "Unable to find the audio stream index. (-1)" << std::endl;
        exit(1);
    }

    return 0;
}

int ScreenRecorder::InitVideoEncoder() {
    int ret;
    AVDictionary *encoder_options = NULL;

    /* Use ultrafast preset to avoid loss of frames */
    av_dict_set(&encoder_options, "preset", "ultrafast", 0);

    out_video_stream_ = avformat_new_stream(out_fmt_ctx_, NULL);
    if (!out_video_stream_) {
        std::cout << "\nerror in creating a av format new stream";
        return -1;
    }

    if (!out_fmt_ctx_->nb_streams) {
        std::cout << "\noutput file dose not contain any stream";
        return -1;
    }

    out_video_codec_ = avcodec_find_encoder(out_video_codec_id_);
    if (!out_video_codec_) {
        std::cout << "\nerror in finding the av codecs. try again with correct codec";
        return -1;
    }

    /* set property of the video file */
    out_video_codec_ctx_ = avcodec_alloc_context3(out_video_codec_);
    out_video_codec_ctx_->pix_fmt = out_video_pix_fmt_;
    out_video_codec_ctx_->width = in_video_codec_ctx_->width;
    out_video_codec_ctx_->height = in_video_codec_ctx_->height;
    out_video_codec_ctx_->framerate = (AVRational){video_framerate_, 1};
    out_video_codec_ctx_->time_base.num = 1;
    out_video_codec_ctx_->time_base.den = video_framerate_;

    /*
     * Some container formats (like MP4) require global headers to be present
     * Mark the encoder so that it behaves accordingly.
     */
    if (out_fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        out_video_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(out_video_codec_ctx_, out_video_codec_, &encoder_options);
    if (ret < 0) {
        std::cout << "\nerror in opening the avcodec";
        return -1;
    }

    ret = avcodec_parameters_from_context(out_video_stream_->codecpar, out_video_codec_ctx_);
    if (ret < 0) {
        std::cout << "\nerror in writing video stream parameters";
        return -1;
    }

    av_dict_free(&encoder_options);

    return 0;
}

int ScreenRecorder::InitAudioEncoder() {
    int ret;

    out_audio_stream_ = avformat_new_stream(out_fmt_ctx_, NULL);
    if (!out_video_stream_) {
        std::cout << "\nerror in creating a av format new stream";
        exit(1);
    }

    if (!out_fmt_ctx_->nb_streams) {
        std::cout << "\noutput file dose not contain any stream";
        return -1;
    }

    out_audio_codec_ = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!out_audio_codec_) {
        std::cout << "\nerror in finding the av codecs. try again with correct codec";
        return -1;
    }

    out_audio_codec_ctx_ = avcodec_alloc_context3(out_audio_codec_);
    out_audio_codec_ctx_->channels = in_audio_stream_->codecpar->channels;
    out_audio_codec_ctx_->channel_layout = av_get_default_channel_layout(in_audio_stream_->codecpar->channels);
    out_audio_codec_ctx_->sample_rate = in_audio_stream_->codecpar->sample_rate;
    out_audio_codec_ctx_->sample_fmt = out_audio_codec_->sample_fmts[0];  // for aac there is AV_SAMPLE_FMT_FLTP = 8
    out_audio_codec_ctx_->bit_rate = 96000;
    out_audio_codec_ctx_->time_base.num = 1;
    out_audio_codec_ctx_->time_base.den = out_audio_codec_ctx_->sample_rate;

    /*
     * Some container formats (like MP4) require global headers to be present
     * Mark the encoder so that it behaves accordingly.
     */
    if (out_fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        out_audio_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(out_audio_codec_ctx_, out_audio_codec_, NULL);
    if (ret < 0) {
        std::cout << "\nerror in opening the avcodec";
        return -1;
    }

    ret = avcodec_parameters_from_context(out_audio_stream_->codecpar, out_audio_codec_ctx_);
    if (ret < 0) {
        std::cout << "\nerror in writing video stream parameters";
        return -1;
    }

    return 0;
}

/* initialize the video output file and its properties  */
int ScreenRecorder::InitOutputFile() {
    int ret;
    out_fmt_ctx_ = NULL;

    /* allocate out_fmt_ctx_ */
    ret = avformat_alloc_output_context2(&out_fmt_ctx_, NULL, NULL, output_file_.c_str());
    if (ret < 0) {
        std::cout << "\nerror in allocating av format output context";
        exit(1);
    }

    if (InitVideoEncoder()) exit(1);
    if (record_audio_ && InitAudioEncoder()) exit(1);

    /* create empty video file */
    if (!(out_fmt_ctx_->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx_->pb, output_file_.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cout << "\nerror in creating the output file";
            exit(1);
        }
    }

    /* imp: mp4 container or some advanced container file required header information */
    ret = avformat_write_header(out_fmt_ctx_, NULL);
    if (ret < 0) {
        std::cout << "\nerror in writing the header context";
        exit(1);
    }

    /* show complete information */
    av_dump_format(out_fmt_ctx_, 0, output_file_.c_str(), 1);

    return 0;
}

int ScreenRecorder::WriteAudioFrameToFifo(AVFrame *frame) {
    int ret;
    uint8_t **buf = nullptr;

    ret = av_samples_alloc_array_and_samples(&buf, NULL, out_audio_codec_ctx_->channels, frame->nb_samples,
                                             out_audio_codec_ctx_->sample_fmt, 0);
    if (ret < 0) {
        throw std::runtime_error("Fail to alloc samples by av_samples_alloc_array_and_samples.");
    }

    ret = swr_convert(audio_converter_ctx_, buf, frame->nb_samples, (const uint8_t **)frame->extended_data,
                      frame->nb_samples);
    if (ret < 0) {
        throw std::runtime_error("Fail to swr_convert.");
    }

    if (av_audio_fifo_space(audio_fifo_buf_) < frame->nb_samples)
        throw std::runtime_error("audio buffer is too small.");

    ret = av_audio_fifo_write(audio_fifo_buf_, (void **)buf, frame->nb_samples);
    if (ret < 0) {
        throw std::runtime_error("Fail to write fifo");
    }

    av_freep(&buf[0]);

    return 0;
}

int ScreenRecorder::EncodeWriteFrame(AVFrame *frame, int audio_video) {
    int ret;
    AVPacket *packet;
    AVCodecContext *codec_ctx;
    int stream_index;

    if (audio_video) {
        codec_ctx = out_audio_codec_ctx_;
        stream_index = out_audio_stream_->index;
    } else {
        codec_ctx = out_video_codec_ctx_;
        stream_index = out_video_stream_->index;
    }

    packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "Could not allocate inPacket" << std::endl;
        return -1;
    }

    ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0) {
        std::cerr << "Error sending a frame for encoding" << std::endl;
        return -1;
    }

    while (true) {
        ret = avcodec_receive_packet(codec_ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            std::cerr << "Error during encoding" << std::endl;
            return -1;
        }

        packet->stream_index = stream_index;

        if (av_interleaved_write_frame(out_fmt_ctx_, packet)) {
            std::cerr << "\nerror in writing video frame" << std::endl;
            return -1;
        }
    }

    av_packet_free(&packet);

    return 0;
}

int ScreenRecorder::ProcessVideoPkt(AVPacket *packet) {
    int ret;
    AVFrame *in_frame;
    AVFrame *out_frame;
    DurationLogger dl(" processed in ");

    in_frame = av_frame_alloc();
    if (!in_frame) {
        std::cerr << "\nunable to release the avframe resources" << std::endl;
        return -1;
    }

    out_frame = av_frame_alloc();
    if (!out_frame) {
        std::cerr << "\nunable to release the avframe resources for outframe";
        return -1;
    }

    out_frame->format = out_video_codec_ctx_->pix_fmt;
    out_frame->width = out_video_codec_ctx_->width;
    out_frame->height = out_video_codec_ctx_->height;

    ret = av_image_alloc(out_frame->data, out_frame->linesize, out_frame->width, out_frame->height,
                         out_video_codec_ctx_->pix_fmt, 1);
    if (ret < 0) {
        std::cerr << "Failed to allocate out_frame data";
        return -1;
    }

    ret = avcodec_send_packet(in_video_codec_ctx_, packet);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        return -1;
    }

    while (true) {
        ret = avcodec_receive_frame(in_video_codec_ctx_, in_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        sws_scale(video_converter_ctx_, in_frame->data, in_frame->linesize, 0, out_frame->height, out_frame->data,
                  out_frame->linesize);

        out_frame->pts =
            av_rescale_q(video_frame_counter_++, out_video_codec_ctx_->time_base, out_video_stream_->time_base);

        if (EncodeWriteFrame(out_frame, 0)) return -1;
    }

    av_frame_free(&in_frame);

    av_freep(&out_frame->data[0]);  // needed beacuse of av_image_alloc() (data is not reference-counted)
    av_frame_free(&out_frame);

    return 0;
}

int ScreenRecorder::ProcessAudioPkt(AVPacket *packet) {
    int ret;
    AVFrame *in_frame;
    AVFrame *out_frame;
    DurationLogger dl(" processed in ");

    in_frame = av_frame_alloc();
    if (!in_frame) {
        std::cerr << "\nunable to release the avframe resources" << std::endl;
        return -1;
    }

    ret = avcodec_send_packet(in_audio_codec_ctx_, packet);
    if (ret < 0) {
        throw std::runtime_error("can not send pkt in decoding");
    }

    while (true) {
        ret = avcodec_receive_frame(in_audio_codec_ctx_, in_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            throw std::runtime_error("can not receive frame in decoding");
        }

        ret = WriteAudioFrameToFifo(in_frame);
        if (ret < 0) {
            throw std::runtime_error("can not write in audio FIFO buffer");
        }

        while (av_audio_fifo_size(audio_fifo_buf_) >= out_audio_codec_ctx_->frame_size) {
            out_frame = av_frame_alloc();
            if (!out_frame) {
                std::cerr << "Could not allocate audio out_frame" << std::endl;
                return -1;
            }

            out_frame->nb_samples = out_audio_codec_ctx_->frame_size;
            out_frame->channels = in_audio_codec_ctx_->channels;
            out_frame->channel_layout = av_get_default_channel_layout(in_audio_codec_ctx_->channels);
            out_frame->format = out_audio_codec_ctx_->sample_fmt;
            out_frame->sample_rate = out_audio_codec_ctx_->sample_rate;
            out_frame->pts = av_rescale_q(out_audio_codec_ctx_->frame_size * audio_frame_counter_++,
                                          out_audio_codec_ctx_->time_base, out_audio_stream_->time_base);

            ret = av_frame_get_buffer(out_frame, 0);
            if (ret < 0) {
                std::cerr << "Cannot fill out_frame buffers";
                return -1;
            }

            ret = av_audio_fifo_read(audio_fifo_buf_, (void **)out_frame->data, out_audio_codec_ctx_->frame_size);
            if (ret < 0) {
                std::cerr << "Cannot read from audio FIFO";
                return -1;
            }

            if (EncodeWriteFrame(out_frame, 1)) return -1;

            av_frame_free(&out_frame);
        }
    }

    av_frame_free(&in_frame);

    return 0;
}

int ScreenRecorder::FlushEncoders() {
    if (EncodeWriteFrame(NULL, 0)) return -1;
    if (record_audio_ && EncodeWriteFrame(NULL, 1)) return -1;
    return 0;
}

/* function to capture and store data in frames by allocating required memory and auto deallocating the memory.   */
int ScreenRecorder::CaptureFrames() {
    /*
     * When you decode a single packet, you still don't have information enough to have a frame
     * [depending on the type of codec, some of them you do], when you decode a GROUP of packets
     * that represents a frame, then you have a picture! that's why frame_finished
     * will let you know you decoded enough to have a frame.
     */
    int ret;
    int video_pkt_counter = 0;
    int audio_pkt_counter = 0;
    AVPacket *packet;
#ifdef __linux__
    AVPacket *audio_packet;
    bool video_data_present = false;
    bool audio_data_present = false;
#endif

    if (InitVideoConverter()) exit(1);
    if (record_audio_ && InitAudioConverter()) exit(1);

    /* start counting for PTS */
    video_frame_counter_ = 0;
    if (record_audio_) audio_frame_counter_ = 0;

    packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "Could not allocate packet";
        exit(1);
    }

#ifdef __linux__
    if (record_audio_) {
        audio_packet = av_packet_alloc();
        if (!packet) {
            std::cerr << "Could not allocate in_audio_packet";
            exit(1);
        }
    }
#endif

    while (true) {
        std::unique_lock<std::mutex> ul{mutex_};
        cv_.wait(ul, [this]() { return !paused_; });
        if (stop_capture_) {
            break;
        }

#ifdef __linux__

        ret = av_read_frame(in_fmt_ctx_, packet);
        if (ret == AVERROR(EAGAIN)) {
            video_data_present = false;
        } else if (ret < 0) {
            std::cerr << "ERROR: Cannot read frame" << std::endl;
            exit(1);
        } else {
            video_data_present = true;
        }

        if (record_audio_) {
            ret = av_read_frame(in_audio_fmt_ctx_, audio_packet);
            if (ret == AVERROR(EAGAIN)) {
                audio_data_present = false;
            } else if (ret < 0) {
                std::cerr << "ERROR: Cannot read frame" << std::endl;
                exit(1);
            } else {
                audio_data_present = true;
            }
        }

        if (video_data_present) {
            std::cout << "[V] packet " << video_pkt_counter++;
            if (ProcessVideoPkt(packet)) exit(1);
            av_packet_unref(packet);
        }

        if (audio_data_present) {
            std::cout << std::endl << "[A] packet " << audio_pkt_counter++;
            if (ProcessAudioPkt(audio_packet)) exit(1);
            av_packet_unref(audio_packet);
        }

#else  // macOS

        ret = av_read_frame(in_fmt_ctx_, packet);
        if (ret == AVERROR(EAGAIN)) {
            continue;
        } else if (ret < 0) {
            std::cerr << "ERROR: Cannot read frame" << std::endl;
            exit(1);
        }

        if (packet->stream_index == in_video_stream_->index) {
            std::cout << "[V] packet " << video_pkt_counter++;
            if (ProcessVideoPkt(packet)) exit(1);
        } else if (record_audio_ && (packet->stream_index == in_audio_stream_->index)) {
            std::cout << "[A] packet " << audio_pkt_counter++;
            if (ProcessAudioPkt(packet)) exit(1);
        } else {
            std::cout << " unknown, ignoring...";
        }

        av_packet_unref(packet);

#endif

        std::cout << std::endl;
    }

    av_packet_free(&packet);
#ifdef __linux__
    if (record_audio_) av_packet_free(&audio_packet);
#endif

    if (FlushEncoders()) {
        std::cerr << "ERROR: Could not flush encoders" << std::endl;
        exit(1);
    };

    ret = av_write_trailer(out_fmt_ctx_);
    if (ret < 0) {
        std::cout << "\nerror in writing av trailer";
        exit(1);
    }

    // THIS WAS ADDED LATER
    avio_close(out_fmt_ctx_->pb);

    return 0;
}

int ScreenRecorder::SelectArea() {
#ifdef __linux__
    XEvent ev;
    Display *disp = NULL;
    Screen *scr = NULL;
    Window root = 0;
    Cursor cursor, cursor2;
    XGCValues gcval;
    GC gc;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    int rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
    int btn_pressed = 0, done = 0;
    int threshold = 10;

    std::cout << "Select the area to record (click to select all the display)" << std::endl;

    disp = XOpenDisplay(NULL);
    if (!disp) return EXIT_FAILURE;

    scr = ScreenOfDisplay(disp, DefaultScreen(disp));

    root = RootWindow(disp, XScreenNumberOfScreen(scr));

    cursor = XCreateFontCursor(disp, XC_left_ptr);
    cursor2 = XCreateFontCursor(disp, XC_lr_angle);

    gcval.foreground = XWhitePixel(disp, 0);
    gcval.function = GXxor;
    gcval.background = XBlackPixel(disp, 0);
    gcval.plane_mask = gcval.background ^ gcval.foreground;
    gcval.subwindow_mode = IncludeInferiors;

    gc = XCreateGC(disp, root, GCFunction | GCForeground | GCBackground | GCSubwindowMode, &gcval);

    /* this XGrab* stuff makes XPending true ? */
    if ((XGrabPointer(disp, root, False, ButtonMotionMask | ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                      GrabModeAsync, root, cursor, CurrentTime) != GrabSuccess))
        printf("couldn't grab pointer:");

    if ((XGrabKeyboard(disp, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess))
        printf("couldn't grab keyboard:");

    while (!done) {
        while (!done && XPending(disp)) {
            XNextEvent(disp, &ev);
            switch (ev.type) {
                case MotionNotify:
                    /* this case is purely for drawing rect on screen */
                    if (btn_pressed) {
                        if (rect_w) {
                            /* re-draw the last rect to clear it */
                            // XDrawRectangle(disp, root, gc, rect_x, rect_y, rect_w, rect_h);
                        } else {
                            /* Change the cursor to show we're selecting a region */
                            XChangeActivePointerGrab(disp, ButtonMotionMask | ButtonReleaseMask, cursor2, CurrentTime);
                        }
                        rect_x = rx;
                        rect_y = ry;
                        rect_w = ev.xmotion.x - rect_x;
                        rect_h = ev.xmotion.y - rect_y;

                        if (rect_w < 0) {
                            rect_x += rect_w;
                            rect_w = 0 - rect_w;
                        }
                        if (rect_h < 0) {
                            rect_y += rect_h;
                            rect_h = 0 - rect_h;
                        }
                        /* draw rectangle */
                        // XDrawRectangle(disp, root, gc, rect_x, rect_y, rect_w, rect_h);
                        XFlush(disp);
                    }
                    break;
                case ButtonPress:
                    btn_pressed = 1;
                    rx = ev.xbutton.x;
                    ry = ev.xbutton.y;
                    break;
                case ButtonRelease:
                    done = 1;
                    break;
            }
        }
    }
    /* clear the drawn rectangle */
    if (rect_w) {
        // XDrawRectangle(disp, root, gc, rect_x, rect_y, rect_w, rect_h);
        XFlush(disp);
    }
    rw = ev.xbutton.x - rx;
    rh = ev.xbutton.y - ry;
    /* cursor moves backwards */
    if (rw < 0) {
        rx += rw;
        rw = 0 - rw;
    }
    if (rh < 0) {
        ry += rh;
        rh = 0 - rh;
    }

    if (rw < threshold || rh < threshold) {
        width_ = scr->width;
        height_ = scr->height;
        offset_x_ = 0;
        offset_y_ = 0;
    } else {
        width_ = rw;
        height_ = rh;
        offset_x_ = rx;
        offset_y_ = ry;
    }

    XCloseDisplay(disp);

#else
    width_ = 1920;
    height_ = 1080;
    offset_x_ = offset_y_ = 0;
#endif

    std::cout << "Size: " << width_ << "x" << height_ << std::endl;
    std::cout << "Offset: " << offset_x_ << "," << offset_y_ << std::endl;

    return 0;
}