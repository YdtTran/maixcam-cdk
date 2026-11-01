#include <cstdlib>
#include <cstring>
#include <memory>
/**
 * @author 916BGAI@sipeed
 * @copyright Sipeed Ltd 2023-
 * @license Apache 2.0
 * @update 2025.12.08: Add maixcam2 webrtc framework.
 */

#include "maix_webrtc.hpp"
#include "maix_err.hpp"
#include "maix_basic.hpp"
#include "maix_camera.hpp"
#include "maix_video.hpp"
#include "maix_audio.hpp"
#include "sophgo_middleware.hpp"
#include "maix_webrtc_server.hpp"
#include "maix_webrtc_audio_dsp.hpp"
#include "nlohmann/json.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#ifdef MAIX_WEBRTC_G4A_TONE
#include <opus.h>
#include <array>
#include <cmath>
#endif
#include <ifaddrs.h>
#include <netdb.h>

namespace maix::webrtc
{
    Region::Region(int x, int y, int width, int height, image::Format format, camera::Camera *camera)
    {
        if (format != image::Format::FMT_BGRA8888) {
            err::check_raise(err::ERR_RUNTIME, "region support FMT_BGRA8888 only!");
        }

        if (camera == NULL) {
            err::check_raise(err::ERR_RUNTIME, "region bind a NULL camera!");
        }

        int rgn_id = mmf_get_region_unused_channel();
        if (rgn_id < 0) {
            err::check_raise(err::ERR_RUNTIME, "no more region id!");
        }

        int flip = true;
        int mirror = true;
        auto configs = sys::device_configs(true);
        for (auto &item : configs) {
            log::info("device:%s value:%s", item.first.c_str(), item.second.c_str());
        }
        auto mirror_string = configs.find("cam_flip");
        auto flip_string = configs.find("cam_mirror");
        if (mirror_string != configs.end()) {
            mirror = !atoi(mirror_string->second.c_str());
        }

        if (flip_string != configs.end()) {
            flip = !atoi(flip_string->second.c_str());
        }

        int x2 = flip ? camera->width() - width - x : x;
        int y2 = mirror ? camera->height() - height - y : y;

        int vi_vpss = 0;
        int vi_vpss_chn = camera->get_channel();
        if (0 != mmf_add_region_channel_v2(rgn_id, 0, 6, vi_vpss, vi_vpss_chn, x2, y2, width, height, mmf_invert_format_to_mmf(format))) {
            err::check_raise(err::ERR_RUNTIME, "mmf_add_region_channel_v2 failed!");
        }
        this->_id = rgn_id;
        this->_width = width;
        this->_height = height;
        this->_x = x;
        this->_y = y;
        this->_format = format;
        this->_camera = camera;
        this->_flip = flip;
        this->_mirror = mirror;
    }

    Region::~Region() {
        if (mmf_del_region_channel(this->_id) < 0) {
            err::check_raise(err::ERR_RUNTIME, "mmf_del_region_unused_channel failed!");
        }
    }

    image::Image *Region::get_canvas() {
        void *data;
        if (0 != mmf_region_get_canvas(this->_id, &data, NULL, NULL, NULL)) {
            err::check_raise(err::ERR_RUNTIME, "mmf_region_get_canvas failed!");
        }

        image::Image *img = NULL;
        switch (this->_format) {
        case image::Format::FMT_BGRA8888:
            img = new image::Image(this->_width, this->_height, this->_format, (uint8_t *)data, this->_width * this->_height * 4, false);
            if (img == NULL) {
                mmf_del_region_channel(this->_id);
                err::check_raise(err::ERR_RUNTIME, "malloc failed!");
            }
            memset(img->data(), 0, img->data_size());
        break;
        default:err::check_raise(err::ERR_RUNTIME, "region format not support!");break;
        }

        this->_image = img;

        return img;
    }

    err::Err Region::update_canvas() {
        image::Image *img = this->_image;
        if (img->format() == image::Format::FMT_BGRA8888) {
            uint32_t *data_u32 = (uint32_t *)img->data();
            int width = img->width();
            int height = img->height();

            if (this->_flip) {
                for (int h = 0; h < height; h ++) {
                    for (int w = 0; w < width / 2; w ++) {
                        int left_idx = h * width + w;
                        int right_idx = h * width + (width - 1 - w);
                        uint32_t tmp = data_u32[left_idx];
                        data_u32[left_idx] = data_u32[right_idx];
                        data_u32[right_idx] = tmp;
                    }
                }
            }

            if (this->_mirror) {
                for (int h = 0; h < height / 2; h ++) {
                    for (int w = 0; w < width; w ++) {
                        int left_idx = h * width + w;
                        int right_idx = (height - 1 - h) * width + w;
                        uint32_t tmp = data_u32[left_idx];
                        data_u32[left_idx] = data_u32[right_idx];
                        data_u32[right_idx] = tmp;
                    }
                }
            }
        } else {
            log::error("support FMT_BGRA888 only!\r\n");
            return err::ERR_RUNTIME;
        }

        if (0 != mmf_region_update_canvas(this->_id)) {
            log::error("mmf_region_update_canvas failed!\r\n");
            return err::ERR_RUNTIME;
        }
        return err::ERR_NONE;
    }

    enum WebRTCStatus
    {
        WEBRTC_IDLE = 0,
        WEBRTC_RUNNING,
        WEBRTC_STOP,
    };

    typedef struct
    {
        WebRTC *self;

        std::atomic<WebRTCStatus> status;

        camera::Camera *camera;
        video::Encoder *encoder;
        audio::Recorder *audio_recorder;

        bool bind_camera;
        bool bind_audio_recorder;

        int encoder_bitrate;
        int gop;

        MaixWebRTCServer *webrtc_server;
        std::shared_ptr<maix::webrtc::MaixAudioUplinkDSP> audio_dsp;
#ifdef MAIX_WEBRTC_G4A_TONE
        std::thread *tone_thread;
#endif

    } webrtc_param_t;

    static uint64_t get_video_timestamp() {
        return time::ticks_ms() * 90;
    }

    static void _camera_push_thread(void *args)
    {
        webrtc_param_t *param = (webrtc_param_t *)args;

        int vi_ch = param->camera->get_channel();
        size_t video_pts = 0;
        bool found_first_pps_sps = false;

        while (param->status == WEBRTC_RUNNING && !app::need_exit()) {
            bool found_camera_frame = false;
            bool found_venc_stream = false;
            void *frame = NULL;
            int enc_ch = 1;
            mmf_frame_info_t f_info;
            mmf_stream_t venc_stream = {0};

            if (param->webrtc_server == nullptr || !param->webrtc_server->is_video_track_open()) {
                time::sleep_ms(10);
                continue;
            }

            if (0 == mmf_vi_frame_pop2(vi_ch, &frame, &f_info) && frame) {
                found_camera_frame = true;
            }

            if (0 == mmf_venc_pop(enc_ch, &venc_stream) && venc_stream.count > 0) {
                found_venc_stream = true;
            }

            if (found_venc_stream) {
                if (!found_first_pps_sps) {
                    if (venc_stream.count > 1) {
                        found_first_pps_sps = true;
                        video_pts = 0;
                    }
                }

                if (found_first_pps_sps) {
                    int venc_output_data_size = 0;
                    uint8_t *venc_output_data = nullptr;
                    for (int i = 0; i < venc_stream.count; i ++) {
                        venc_output_data_size += venc_stream.data_size[i];
                    }

                    venc_output_data = (uint8_t *)malloc(venc_output_data_size);
                    if (venc_output_data) {
                        int curr_data_size = 0;
                        for (int i = 0; i < venc_stream.count; i ++) {
                            memcpy(venc_output_data + curr_data_size, venc_stream.data[i], venc_stream.data_size[i]);
                            curr_data_size += venc_stream.data_size[i];
                        }

                        video_pts = get_video_timestamp();

                        if (param->webrtc_server && param->webrtc_server->is_video_track_open()) {
                            param->webrtc_server->video_frame_push(venc_output_data, venc_output_data_size, video_pts);
                        }

                        free(venc_output_data);
                    }
                }
            }

            if (found_venc_stream) {
                if (0 != mmf_venc_free(enc_ch)) {
                    log::error("mmf_venc_free failed!\r\n");
                }
            }

            if (found_camera_frame) {
                if (0 != mmf_venc_push2(enc_ch, frame)) {
                    log::error("mmf_venc_push2 failed!\r\n");
                }

                mmf_vi_frame_free2(vi_ch, &frame);
            }
        }

        // Lifecycle/status is owned by WebRTC::stop().
    }

#ifdef MAIX_WEBRTC_G4A_TONE
    // Test-only synthetic audio: one 20 ms Opus frame per 960 mono PCM samples.
    static void _opus_tone_thread(webrtc_param_t *param)
    {
        constexpr int rate = 48000;
        constexpr int frame_samples = 960;
        constexpr double phase_step = 6.28318530717958647692 * 440.0 / rate;
        int error = OPUS_OK;
        OpusEncoder *encoder = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &error);
        if (!encoder || error != OPUS_OK) {
            log::error("G4a Opus encoder creation failed: %d", error);
            if (encoder) opus_encoder_destroy(encoder);
            return;
        }
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
        std::array<opus_int16, frame_samples> pcm{};
        std::array<unsigned char, 1275> encoded{};
        double phase = 0.0;
        uint64_t sample_cursor = 0;
        auto next = std::chrono::steady_clock::now();
        unsigned sent = 0, failed = 0;

        while (param->status == WEBRTC_RUNNING && !app::need_exit()) {
            if (param->webrtc_server && param->webrtc_server->is_audio_track_open()) {
                for (int i = 0; i < frame_samples; ++i) {
                    pcm[i] = static_cast<opus_int16>(std::sin(phase) * 6000.0);
                    phase += phase_step;
                    if (phase >= 6.28318530717958647692)
                        phase -= 6.28318530717958647692;
                }
                int size = opus_encode(encoder, pcm.data(), frame_samples,
                                       encoded.data(), static_cast<opus_int32>(encoded.size()));
                if (size > 0 && param->webrtc_server->audio_frame_push(
                                    encoded.data(), static_cast<size_t>(size), sample_cursor) == 0)
                    ++sent;
                else
                    ++failed;
                if ((sent + failed) % 250 == 0)
                    log::info("G4a audio frames sent=%u failed=%u", sent, failed);
            }
            sample_cursor += frame_samples;
            next += std::chrono::milliseconds(20);
            // Skip missed send opportunities: do not produce a burst of stale audio.
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;
            std::this_thread::sleep_until(next);
        }
        opus_encoder_destroy(encoder);
        log::info("G4a tone stopped, sent=%u failed=%u", sent, failed);
    }

    // G4b experimental onboard microphone source.
    // Captures continuously, including when no peer is connected,
    // to avoid sending stale PCM following a reconnect.
    static void _opus_mic_thread(webrtc_param_t *param)
    {
        constexpr int rate = 48000;
        constexpr int frame_samples = 960;
        constexpr size_t frame_bytes = frame_samples * sizeof(opus_int16);

        std::unique_ptr<audio::Recorder> recorder;

        try {
            recorder.reset(new audio::Recorder(
                "", rate, audio::Format::FMT_S16_LE, 1, false
            ));

            if (recorder->frame_size(frame_samples) !=
                static_cast<int>(frame_bytes)) {
                log::error("G4b unexpected microphone PCM format");
                return;
            }

            recorder->reset(true);
        } catch (const std::exception &e) {
            log::error("G4b microphone initialization: %s", e.what());
            return;
        }

        int error = OPUS_OK;
        OpusEncoder *raw = opus_encoder_create(
            rate, 1, OPUS_APPLICATION_VOIP, &error
        );

        std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>
            encoder(raw, &opus_encoder_destroy);

        if (!encoder || error != OPUS_OK) {
            log::error("G4b Opus encoder creation failed: %d", error);
            return;
        }

        opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(24000));
        opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(5));

        auto dsp = param->audio_dsp;
        if (!dsp) {
            dsp = std::make_shared<maix::webrtc::MaixAudioUplinkDSP>();
            param->audio_dsp = dsp;
            if (param->webrtc_server) {
                param->webrtc_server->set_audio_dsp(dsp);
            }
        }

        std::array<opus_int16, frame_samples> pcm{};
        std::array<unsigned char, 1275> encoded{};

        uint64_t sample_cursor = 0;

        unsigned sent = 0;
        unsigned failed = 0;
        unsigned dropped = 0;

        bool previous_open = false;
        auto last_capture = std::chrono::steady_clock::now();
        unsigned recovery_attempts = 0;
        auto recover_capture = [&]() -> bool {
            if (app::need_exit() || param->status != WEBRTC_RUNNING)
                return false;
            if (++recovery_attempts > 3) {
                log::error("G4c PCM recovery exhausted");
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto gap_us = std::chrono::duration_cast<
                std::chrono::microseconds>(now - last_capture).count();
            const uint64_t lost = static_cast<uint64_t>(gap_us) * rate / 1000000
                                + (dsp ? dsp->queued_bytes() : 0) / sizeof(opus_int16);
            sample_cursor += ((lost + frame_samples - 1) / frame_samples) * frame_samples;
            if (dsp) dsp->reset();
            log::warn("G4c resetting stalled onboard PCM, attempt=%u", recovery_attempts);
            try {
                recorder->reset(true); // Same capture handle; no competing device owner.
                opus_encoder_ctl(encoder.get(), OPUS_RESET_STATE);
                last_capture = std::chrono::steady_clock::now();
                return true;
            } catch (const std::exception &e) {
                log::error("G4c PCM recovery failed: %s", e.what());
                return false;
            }
        };


        log::info("G4b microphone capture started: 48000 Hz mono (DSP active)");

        while (param->status == WEBRTC_RUNNING && !app::need_exit()) {
            const bool track_open =
                param->webrtc_server &&
                param->webrtc_server->is_audio_track_open();

            // Do not preserve partial frames across connection changes.
            if (track_open != previous_open) {
                if (dsp) dsp->reset();
                previous_open = track_open;
            }

            const size_t wanted = frame_bytes;
            std::unique_ptr<maix::Bytes> chunk;

            try {
                chunk.reset(recorder->record_bytes(
                    static_cast<int>(wanted)
                ));
            } catch (const std::exception &e) {
                log::error("G4b microphone read: %s", e.what());
                if (!recover_capture()) break;
                continue;
            }

            if (app::need_exit() || param->status != WEBRTC_RUNNING)
                break;

            const size_t n = chunk && chunk->data
                ? chunk->data_len : 0;

            if (n == 0) {
                if (std::chrono::steady_clock::now() - last_capture >
                    std::chrono::milliseconds(250)) {
                    if (!recover_capture()) break;
                    continue;
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(2)
                );
                continue;
            }

            if (n % sizeof(opus_int16) != 0) {
                log::error("G4b invalid PCM read size: %zu", n);
                break;
            }

            last_capture = std::chrono::steady_clock::now();
            recovery_attempts = 0;

            dsp->ingest_bytes(chunk->data, n);

            while (dsp->has_frame()) {
                if (!dsp->pop_and_process_frame(pcm.data())) break;

                if (track_open &&
                    param->webrtc_server &&
                    param->webrtc_server->is_audio_track_open()) {

                    int size = opus_encode(
                        encoder.get(),
                        pcm.data(),
                        frame_samples,
                        encoded.data(),
                        static_cast<opus_int32>(encoded.size())
                    );

                    if (size > 0 &&
                        param->webrtc_server->audio_frame_push(
                            encoded.data(),
                            static_cast<size_t>(size),
                            sample_cursor
                        ) == 0) {
                        ++sent;
                    } else {
                        ++failed;
                    }
                } else {
                    ++dropped;
                }

                // RTP audio timestamp advances by captured sample count.
                sample_cursor += frame_samples;

                if ((sent + failed) != 0 &&
                    (sent + failed) % 250 == 0) {
                    log::info(
                        "G4b mic sent=%u failed=%u dropped=%u",
                        sent, failed, dropped
                    );
                }
            }
        }

        log::info(
            "G4b microphone stopped: sent=%u failed=%u dropped=%u",
            sent, failed, dropped
        );
    }
#endif

    static video::VideoType get_video_type( maix::webrtc::WebRTCStreamType stream, maix::webrtc::WebRTCRCType rc)
    {
        using stream_type = maix::webrtc::WebRTCStreamType;
        using rc_type = maix::webrtc::WebRTCRCType;

        if (stream == stream_type::WEBRTC_STREAM_H264) {
            if (rc == rc_type::WEBRTC_RC_CBR) { return video::VIDEO_H264_CBR; }
            if (rc == rc_type::WEBRTC_RC_VBR) { return video::VIDEO_H264_VBR; }
        }

        if (stream == stream_type::WEBRTC_STREAM_H265) {
            if (rc == rc_type::WEBRTC_RC_CBR) { return video::VIDEO_H265_CBR; }
            if (rc == rc_type::WEBRTC_RC_VBR) { return video::VIDEO_H265_VBR; }
        }

        log::error("Unsupported video type stream=%d rc=%d", (int)stream, (int)rc);
        return video::VIDEO_H264_CBR;
    }

    WebRTC::WebRTC(std::string ip, int port, webrtc::WebRTCStreamType stream_type,
                   webrtc::WebRTCRCType rc_type, int bitrate, int gop, std::string signaling_ip,
                   int signaling_port, const std::string &stun_server, bool http_server)
    {
        this->_ip = ip.size() ? ip : "0.0.0.0";
        this->_port = port;
        this->_signaling_ip = signaling_ip;
        this->_signaling_port = signaling_port;
        this->_stun_server = stun_server;
        this->_gop = gop;
        this->_stream_type = stream_type;
        this->_rc_type = rc_type;
        this->_is_start = false;
        this->_video_thread = nullptr;
        this->_http_server = http_server;
        this->_timestamp = 0;
        this->_last_ms = 0;
        this->_region_max_number = 16;
        for (int i = 0; i < this->_region_max_number; i ++) {
            this->_region_list.push_back(NULL);
            this->_region_type_list.push_back(0);
            this->_region_used_list.push_back(false);
        }

        auto *param = new webrtc_param_t();
        param->self = this;
        param->status = WEBRTC_IDLE;
        param->camera = nullptr;
        param->encoder = nullptr;
        param->audio_recorder = nullptr;
        param->bind_camera = false;
        param->bind_audio_recorder = false;
        param->encoder_bitrate = bitrate;
        param->gop = gop;

        param->webrtc_server = nullptr;
        param->audio_dsp = std::make_shared<maix::webrtc::MaixAudioUplinkDSP>();
#ifdef MAIX_WEBRTC_G4A_TONE
        param->tone_thread = nullptr;
#endif

        _param = (void *)param;
    }

    WebRTC::~WebRTC()
    {
        webrtc_param_t *param = (webrtc_param_t *)_param;
        if (param) {
            stop();

            if (param->encoder) {
                delete param->encoder;
                param->encoder = nullptr;
            }

            if (param->webrtc_server) {
                delete param->webrtc_server;
                param->webrtc_server = nullptr;
            }

            delete param;
            _param = nullptr;
        }

        for (auto &region : this->_region_list) {
            delete region;
        }
    }

    err::Err WebRTC::start()
    {
        webrtc_param_t *param = (webrtc_param_t *)_param;
        if (!param) {
            return err::ERR_RUNTIME;
        }

        if (param->status != WEBRTC_IDLE) {
            return err::ERR_BUSY;
        }

        if (!param->bind_camera || !param->camera) {
            log::error("You need bind a camera!");
            return err::ERR_RUNTIME;
        }

        if (param->camera->width() % 32 != 0) {
            log::error("camera width must be multiple of 32!\r\n");
            return err::ERR_RUNTIME;
        }

        if (param->encoder) {
            delete param->encoder;
            param->encoder = nullptr;
        }

        video::VideoType video_type = get_video_type(this->_stream_type, this->_rc_type);

        param->encoder = new video::Encoder("", param->camera->width(), param->camera->height(), image::Format::FMT_YVU420SP, video_type, param->camera->fps(), param->gop, param->encoder_bitrate);
        err::check_null_raise(param->encoder, "Create video encoder failed!");

        if (param->bind_audio_recorder) {
            param->audio_recorder->reset(true);
        }

        if (param->webrtc_server) {
            delete param->webrtc_server;
            param->webrtc_server = nullptr;
        }

        MaixWebRTCServerBuilder builder;
        builder.set_ice_server(std::getenv("MAIX_WEBRTC_STANDALONE") ? "" : this->_stun_server);
#ifdef MAIX_WEBRTC_G4A_TONE
        builder.set_audio(true).set_audio_sample_rate(48000).set_audio_channels(1);
        log::info("Experimental Opus audio enabled; set MAIX_WEBRTC_AUDIO_SOURCE=mic for microphone");
#endif

        if (this->_stream_type == maix::webrtc::WebRTCStreamType::WEBRTC_STREAM_H265) {
            builder.set_video_codec(VideoCodec::H265);
            log::info("Using H.265 codec for WebRTC");
        } else {
            builder.set_video_codec(VideoCodec::H264);
            log::info("Using H.264 codec for WebRTC");
        }

        if (this->_http_server) {
            builder.set_http_server(true, this->_ip, this->_port);
            builder.set_ws_port(this->_signaling_port);
            builder.set_stun_server(this->_stun_server);
            log::info("HTTP server will be started on %s:%d", this->_ip.c_str(), this->_port);
        }

        param->webrtc_server = builder.build();
        if (!param->webrtc_server) {
            log::error("Failed to create WebRTC server");
            return err::ERR_RUNTIME;
        }

        if (param->audio_dsp && param->webrtc_server) {
            param->webrtc_server->set_audio_dsp(param->audio_dsp);
        }

        if (!param->webrtc_server->connect_signaling(this->_signaling_ip, this->_signaling_port)) {
            log::error("Failed to connect to signaling server");
            return err::ERR_RUNTIME;
        }

        param->webrtc_server->start();

        param->status = WEBRTC_RUNNING;
        _video_thread = new thread::Thread(_camera_push_thread, param);
        if (!_video_thread) {
            param->status = WEBRTC_IDLE;
            return err::ERR_RUNTIME;
        }

#ifdef MAIX_WEBRTC_G4A_TONE
        try {
            param->tone_thread = new std::thread(
                [](webrtc_param_t *p) {
                    const char *source =
                        std::getenv("MAIX_WEBRTC_AUDIO_SOURCE");

                    if (source && std::strcmp(source, "mic") == 0) {
                        log::info("G4b audio source: onboard microphone");
                        _opus_mic_thread(p);
                    } else {
                        log::info("G4a audio source: synthetic tone");
                        _opus_tone_thread(p);
                    }
                },
                param
            );
        } catch (const std::exception &e) {
            log::error("G4a tone thread creation failed: %s", e.what());
            stop();
            return err::ERR_RUNTIME;
        }
#endif
        _is_start = true;
        return err::ERR_NONE;
    }

    err::Err WebRTC::stop()
    {
        webrtc_param_t *param = (webrtc_param_t *)_param;
        if (!param) return err::ERR_NONE;

        // Stop even after a worker exited unexpectedly: join owned threads.
        param->status = WEBRTC_STOP;
#ifdef MAIX_WEBRTC_G4A_TONE
        if (param->tone_thread) {
            param->tone_thread->join();
            delete param->tone_thread;
            param->tone_thread = nullptr;
        }
#endif
        if (_video_thread) {
            _video_thread->join();
            delete _video_thread;
            _video_thread = nullptr;
        }

        param->status = WEBRTC_IDLE;
        _is_start = false;
        return err::ERR_NONE;
    }

    err::Err WebRTC::bind_camera(camera::Camera *camera)
    {
        webrtc_param_t *param = (webrtc_param_t *)_param;
        if (!param || !camera) {
            return err::ERR_RUNTIME;
        }

        if (camera->format() != image::Format::FMT_YVU420SP) {
            err::check_raise(err::ERR_RUNTIME, "bind camera failed! support FMT_YVU420SP only!\r\n");
            return err::ERR_RUNTIME;
        }

        param->camera = camera;
        param->bind_camera = true;
        return err::ERR_NONE;
    }

    err::Err WebRTC::bind_audio_recorder(audio::Recorder *recorder)
    {
        err::Err err = err::ERR_NOT_IMPL;
        (void)recorder;
        return err;
    }

    err::Err WebRTC::write(video::Frame &frame)
    {
        err::Err err = err::ERR_NONE;
        err::check_raise(err::ERR_NOT_IMPL, "write frame not impl!");
        return err;
    }

    camera::Camera *WebRTC::to_camera()
    {
        webrtc_param_t *param = (webrtc_param_t *)_param;
        if (!param || !param->camera) {
            return nullptr;
        }
        return param->camera;
    }

    std::string WebRTC::get_url()
    {
        return "http://" + _ip + ":" + std::to_string(_port);
    }

    static int get_ip(char *hw, char ip[16])
    {
        struct ifaddrs *ifaddr, *ifa;
        int family, s;
        char host[NI_MAXHOST];

        if (getifaddrs(&ifaddr) == -1) {
            perror("getifaddrs");
            return -1;
        }

        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) {
                continue;
            }

            family = ifa->ifa_addr->sa_family;

            if (family == AF_INET) {
                s = getnameinfo(ifa->ifa_addr, (family == AF_INET) ? sizeof(struct sockaddr_in) :
                                sizeof(struct sockaddr_in6), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
                if (s != 0) {
                    printf("getnameinfo() failed: %s\n", gai_strerror(s));
                    return -1;
                }

                if (!strcmp(ifa->ifa_name, hw)) {
                    strncpy(ip, host, 16);
                    freeifaddrs(ifaddr);
                    return 0;
                }
            }
        }

        freeifaddrs(ifaddr);
        return -1;
    }

    static std::vector<std::string> webrtc_get_server_urls(std::string ip, int port)
    {
        char new_ip[16] = {0};

        std::vector<std::string> ip_list;

        if (!strcmp("0.0.0.0", ip.c_str())) {
            if (!get_ip((char *)"eth0", new_ip)) {
                ip_list.push_back("http://" + std::string(new_ip) + ":" + std::to_string(port));
            }
            if (!get_ip((char *)"usb0", new_ip)) {
                ip_list.push_back("http://" + std::string(new_ip) + ":" + std::to_string(port));
            }
            if (!get_ip((char *)"usb1", new_ip)) {
                ip_list.push_back("http://" + std::string(new_ip) + ":" + std::to_string(port));
            }
            if (!get_ip((char *)"wlan0", new_ip)) {
                ip_list.push_back("http://" + std::string(new_ip) + ":" + std::to_string(port));
            }
            if (!get_ip((char *)"tailscale0", new_ip)) {
                ip_list.push_back("http://" + std::string(new_ip) + ":" + std::to_string(port));
            }
        } else {
            ip_list.push_back("http://" + ip + ":" + std::to_string(port));
        }

        return ip_list;
    }

    std::vector<std::string> WebRTC::get_urls()
    {
        return webrtc_get_server_urls(_ip, _port);
    }

    webrtc::Region *WebRTC::add_region(int x, int y, int width, int height, image::Format format) {
        webrtc_param_t *param = (webrtc_param_t *)_param;
        if (!param) {
            return nullptr;
        }

        if (format != image::Format::FMT_BGRA8888) {
            log::error("region support FMT_BGRA8888 only!\r\n");
            return NULL;
        }

        if (!param->bind_camera) {
            log::error("You must use bind camera firstly!\r\n");
            return NULL;
        }

        // Find unused idx
        int unused_idx = -1;
        for (int i = 0; i < this->_region_max_number; i ++) {
            if (this->_region_used_list[i] == false) {
                unused_idx = i;
                break;
            }
        }
        err::check_bool_raise(unused_idx != -1, "Unused region not found");

        // Create region
        webrtc::Region *region = new webrtc::Region(x, y, width, height, format, param->camera);
        err::check_null_raise(region, "Create region failed!");
        this->_region_list[unused_idx] = region;
        this->_region_used_list[unused_idx] = true;
        this->_region_type_list[unused_idx] = 0;

        return region;
    }

    err::Err WebRTC::update_region(webrtc::Region &region)
    {
        return region.update_canvas();
    }

    err::Err WebRTC::del_region(webrtc::Region *region) {
        err::check_null_raise(region, "The region object is NULL");

        for (int i = 0; i < this->_region_max_number; i ++) {
            if (this->_region_list[i] == region) {
                this->_region_list[i] = NULL;
                this->_region_used_list[i] = false;
                this->_region_type_list[i] = 0;
                delete region;
                return err::ERR_NONE;
            }
        }

        return err::ERR_NONE;
    }

    err::Err WebRTC::draw_rect(int id, int x, int y, int width, int height, image::Color color, int thickness)
    {
        webrtc_param_t *param = (webrtc_param_t *)_param;
        if (!param) {
            return err::ERR_RUNTIME;
        }

        // Check id
        if (id < 0 || id > 3) {
            log::error("region id is invalid! range is [0, 3");
            err::check_raise(err::ERR_RUNTIME, "invalid parameter");
        }

        if (x < 0) {
            width = width + x < 0 ? 0 : width + x;
            x = 0;
        }

        if (y < 0) {
            height = height + y < 0 ? 0 : height + y;
            y = 0;
        }

        if (x > param->camera->width()) {
            x = 0;
            width = 0;
        }

        if (y > param->camera->height()) {
            y = 0;
            height = 0;
        }

        if (x + width > param->camera->width()) {
            width = param->camera->width() - x;
        }

        if (y + height > param->camera->height()) {
            height = param->camera->height() - y;
        }

        // Check if the region [id, id + 4) is used for other functions
        for (size_t i = id; i < this->_region_used_list.size() && (int)i < id + 4; i ++) {
            if (_region_used_list[i] == true && _region_type_list[i] != 2) {
                log::error("In areas %d - %d, %d is used for other functions(%d)", id, id + 4, i, _region_type_list[i]);
                err::check_raise(err::ERR_RUNTIME, "invalid parameter");
            }
        }

        // Delete last region
        for (size_t i = id; i < this->_region_used_list.size() && (int)i < id + 4; i ++) {
            if (_region_used_list[i] == true && _region_type_list[i] == 2) {
                delete _region_list[i];
                _region_list[i] = NULL;
                _region_used_list[i] = false;
                _region_type_list[i] = -1;
            }
        }

        // Create upper region
        int upper_lower_height = 0;
        upper_lower_height = thickness > 0 ? thickness : height;
        upper_lower_height = upper_lower_height > height ? height : upper_lower_height;
        webrtc::Region *region_upper = add_region(x, y, width, upper_lower_height);
        err::check_null_raise(region_upper);

        // Create lower region
        webrtc::Region *region_lower = add_region(x, y + height - upper_lower_height, width, upper_lower_height);
        err::check_null_raise(region_upper);

        // Create left region
        int left_right_width = 0;
        left_right_width = thickness > 0 ? thickness : width;
        left_right_width = left_right_width > width ? width : left_right_width;
        webrtc::Region *region_left = add_region(x, y + upper_lower_height, left_right_width, height - 2 * upper_lower_height);
        err::check_null_raise(region_left);

        // Create right region
        webrtc::Region *region_right = add_region(x + width - left_right_width, y + upper_lower_height, left_right_width, height - 2 * upper_lower_height);
        err::check_null_raise(region_right);

        // Config region list
        _region_list[id] = region_upper;
        _region_list[id + 1] = region_lower;
        _region_list[id + 2] = region_left;
        _region_list[id + 3] = region_right;
        _region_used_list[id] = true;
        _region_used_list[id + 1] = true;
        _region_used_list[id + 2] = true;
        _region_used_list[id + 3] = true;
        _region_type_list[id] = 2;
        _region_type_list[id + 1] = 2;
        _region_type_list[id + 2] = 2;
        _region_type_list[id + 3] = 2;

        // Draw all of region
        uint32_t color_hex = color.hex();
        for (int i = id; i < id + 4; i ++) {
            webrtc::Region *region = _region_list[i];
            image::Image *img = region->get_canvas();
            err::check_null_raise(img, "Get canvas image failed!");
            uint32_t *data = (uint32_t *)img->data();
            int width = img->width();
            int height = img->height();
            for (int i = 0; i < height; i ++) {
                for (int j = 0; j < width; j ++) {
                    data[i * width + j] = color_hex;
                }
            }
            update_region(*region);
        }

        return err::ERR_NONE;
    }

    err::Err WebRTC::draw_string(int id, int x, int y, const char *str, image::Color color, int size, int thickness)
    {
        return err::ERR_NOT_IMPL;
    }

} // namespace maix::webrtc
