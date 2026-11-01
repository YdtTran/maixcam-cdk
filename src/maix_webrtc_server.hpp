#pragma once

#include <atomic>
#include "g4c4_opus_rx.hpp"
#include "maix_webrtc_audio_dsp.hpp"
#include "rtc/rtc.hpp"
#include "nlohmann/json.hpp"
#include "maix_webrtc_signaling_server.hpp"
#include "maix_webrtc_http_server.hpp"
#include <string>
#include <memory>
#include <iostream>

#ifdef NDEBUG
#define MAIX_WEBRTC_LOG(...)
#else
#define MAIX_WEBRTC_LOG(...) printf(__VA_ARGS__)
#endif

#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <sstream>
#include <vector>

using json = nlohmann::json;

enum class VideoCodec {
    H264,
    H265
};

class MaixWebRTCServerBuilder;

class MaixWebRTCServer
{
    friend class MaixWebRTCServerBuilder;

private:
    int *clients;
    std::shared_ptr<rtc::PeerConnection> _peer_connection;
    rtc::Configuration _peer_config;
    std::shared_ptr<rtc::DataChannel> _data_channel;
    std::shared_ptr<rtc::Track> _video_track;
    std::shared_ptr<rtc::Track> _audio_track;
    std::shared_ptr<G4c4OpusRx> _audio_rx;
    std::shared_ptr<G4c4BtPlayback> _shared_playback;
    std::shared_ptr<maix::webrtc::MaixAudioUplinkDSP> _audio_dsp;
    std::shared_ptr<rtc::RtpPacketizationConfig> _video_rtp_config;
    std::shared_ptr<rtc::RtpPacketizationConfig> _audio_rtp_config;

    // Uplink audio metrics (PLAN-07)
    std::atomic<uint64_t> _audio_tx_sent{0};
    std::atomic<uint64_t> _audio_tx_failed{0};
    std::atomic<uint64_t> _audio_tx_bytes{0};

    // Protect tracks, peer handle and RTP configs during reconnection/sending.
    std::mutex _media_mutex;
    std::mutex _reset_mutex;
    bool _has_audio;
    VideoCodec _video_codec;

    std::mutex _frame_mutex;
    std::condition_variable _frame_cv;

    bool _is_running;
    std::thread *_send_thread;

    // Signaling WebSocket
    std::shared_ptr<rtc::WebSocket> _signaling_ws;
    std::string _client_id;
    bool _offer_sent;
    std::function<void()> _on_connected;
    std::function<void()> _on_disconnected;

    // Built-in signaling server
    std::shared_ptr<MaixWebRTCSignalingServer> _builtin_signaling_server;
    bool _use_builtin_signaling;

    // Built-in HTTP server
    std::shared_ptr<MaixWebRTCHttpServer> _http_server;
    bool _use_http_server;

public:
    MaixWebRTCServer(int *clients, std::shared_ptr<rtc::PeerConnection> peer_connection, bool has_audio, VideoCodec video_codec = VideoCodec::H264)
        : _shared_playback(std::make_shared<G4c4BtPlayback>()),
          clients(clients), _peer_connection(peer_connection), _has_audio(has_audio), _video_codec(video_codec), _is_running(false), _send_thread(nullptr),
          _client_id("server"), _offer_sent(false), _use_builtin_signaling(false), _use_http_server(false)
    {
        if (clients != nullptr) {
            *clients = 0;
        }

        _peer_connection->onStateChange([this](rtc::PeerConnection::State state) {
            MAIX_WEBRTC_LOG("PeerConnection state: %d\n", (int)state);
        });

        _peer_connection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            MAIX_WEBRTC_LOG("PeerConnection gathering state: %d\n", (int)state);
        });

        _peer_connection->onLocalCandidate([this](rtc::Candidate candidate) {
            std::string cand_str = candidate.candidate();
            if (cand_str.find("::") != std::string::npos ||
                (cand_str.find(':') != std::string::npos &&
                 std::count(cand_str.begin(), cand_str.end(), ':') > 2)) {
                MAIX_WEBRTC_LOG("Filtered IPv6 candidate: %s\n", cand_str.c_str());
                return;
            }
            MAIX_WEBRTC_LOG("New local candidate: %s\n", cand_str.c_str());
        });

        _peer_connection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            MAIX_WEBRTC_LOG("New data channel: %s\n", dc->label().c_str());
            {
                std::lock_guard<std::mutex> lock(_media_mutex);
                _data_channel = dc;
            }

            dc->onOpen([this]() {
                MAIX_WEBRTC_LOG("Data channel opened\n");
            });

            dc->onClosed([this]() {
                MAIX_WEBRTC_LOG("Data channel closed\n");
            });

            dc->onMessage([this](rtc::message_variant data) {
                if (std::holds_alternative<rtc::binary>(data)) {
                    MAIX_WEBRTC_LOG("Received binary message\n");
                } else {
                    const std::string &str = std::get<std::string>(data);
                    MAIX_WEBRTC_LOG("Received string message: %s\n", str.c_str());
                    if (_audio_rx) {
                        if (str.find("talk-stop") != std::string::npos ||
                            str.find("\"talk\":false") != std::string::npos ||
                            str.find("talk-off") != std::string::npos ||
                            str == "talk:0") {
                            _audio_rx->set_talk_state(false);
                        } else if (str.find("talk-start") != std::string::npos ||
                                   str.find("\"talk\":true") != std::string::npos ||
                                   str.find("talk-on") != std::string::npos ||
                                   str == "talk:1") {
                            _audio_rx->set_talk_state(true);
                        }
                    }
                }
            });
        });
    }

    ~MaixWebRTCServer() {
        stop();

        if (_http_server) {
            _http_server->stop();
        }

        if (_builtin_signaling_server) {
            _builtin_signaling_server->stop();
        }

        if (_signaling_ws && _signaling_ws->isOpen()) {
            _signaling_ws->close();
        }

        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            peer = std::move(_peer_connection);
            _video_track.reset();
            _audio_track.reset();
            _audio_rx.reset();
            _video_rtp_config.reset();
            _audio_rtp_config.reset();
        }
        if (peer) peer->close();

        if (clients != nullptr) {
            free(clients);
            clients = nullptr;
        }
    }

    int get_clients() {
        int value = 0;
        if (clients != nullptr) {
            value = *clients;
        }
        return value;
    }

    void reset_peer_connection() {
        // Serialize concurrent disconnect callbacks and stop sending before closing.
        std::lock_guard<std::mutex> reset_lock(_reset_mutex);
        MAIX_WEBRTC_LOG("[WebRTC] Resetting PeerConnection...\n");
        std::shared_ptr<rtc::PeerConnection> old_peer;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            old_peer = std::move(_peer_connection);
            _video_track.reset();
            _audio_track.reset();
            _audio_rx.reset();
            _data_channel.reset();
            _video_rtp_config.reset();
            _audio_rtp_config.reset();
        }
        if (old_peer) {
            try {
                old_peer->close();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } catch (const std::exception &e) {
                MAIX_WEBRTC_LOG("[WebRTC] Error closing old PeerConnection: %s\n", e.what());
            }
        }

        try {
            auto new_peer = std::make_shared<rtc::PeerConnection>(_peer_config);
            {
                std::lock_guard<std::mutex> lock(_media_mutex);
                _peer_connection = new_peer;
            }

            new_peer->onStateChange([this](rtc::PeerConnection::State state) {
                MAIX_WEBRTC_LOG("PeerConnection state: %d\n", (int)state);
            });

            new_peer->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
                MAIX_WEBRTC_LOG("PeerConnection gathering state: %d\n", (int)state);
            });

            new_peer->onLocalCandidate([this](rtc::Candidate candidate) {
                std::string cand_str = candidate.candidate();
                if (cand_str.find("::") != std::string::npos ||
                    (cand_str.find(':') != std::string::npos &&
                     std::count(cand_str.begin(), cand_str.end(), ':') > 2)) {
                    MAIX_WEBRTC_LOG("Filtered IPv6 candidate: %s\n", cand_str.c_str());
                    return;
                }
                MAIX_WEBRTC_LOG("New local candidate: %s\n", cand_str.c_str());
            });

            new_peer->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
                MAIX_WEBRTC_LOG("New data channel: %s\n", dc->label().c_str());
                {
                    std::lock_guard<std::mutex> lock(_media_mutex);
                    _data_channel = dc;
                }

                dc->onOpen([this]() {
                    MAIX_WEBRTC_LOG("Data channel opened\n");
                });

                dc->onClosed([this]() {
                    MAIX_WEBRTC_LOG("Data channel closed\n");
                });

                dc->onMessage([this](rtc::message_variant data) {
                    if (std::holds_alternative<rtc::binary>(data)) {
                        auto bin = std::get<rtc::binary>(data);
                        MAIX_WEBRTC_LOG("Data channel received binary data: %zu bytes\n", bin.size());
                    } else {
                        auto str = std::get<std::string>(data);
                        MAIX_WEBRTC_LOG("Data channel received: %s\n", str.c_str());
                        if (_audio_rx) {
                            if (str.find("talk-stop") != std::string::npos ||
                                str.find("\"talk\":false") != std::string::npos ||
                                str.find("talk-off") != std::string::npos ||
                                str == "talk:0") {
                                _audio_rx->set_talk_state(false);
                            } else if (str.find("talk-start") != std::string::npos ||
                                       str.find("\"talk\":true") != std::string::npos ||
                                       str.find("talk-on") != std::string::npos ||
                                       str == "talk:1") {
                                _audio_rx->set_talk_state(true);
                            }
                        }
                    }
                });
            });

            add_video_track();
            if (_has_audio) {
                add_audio_track();
            }

            try {
                new_peer->setLocalDescription();
            } catch (const std::exception &e) {
                MAIX_WEBRTC_LOG("[WebRTC] Error setting local description after reset: %s\n", e.what());
            }

            MAIX_WEBRTC_LOG("[WebRTC] PeerConnection reset successfully\n");
        } catch (const std::exception &e) {
            MAIX_WEBRTC_LOG("[WebRTC] Failed to reset PeerConnection: %s\n", e.what());
        }
    }

    void add_video_track() {
        std::lock_guard<std::mutex> lock(_media_mutex);
        if (!_peer_connection) return;

        const rtc::SSRC ssrc = 1;
        auto video = rtc::Description::Video("video");

        if (_video_codec == VideoCodec::H265) {
            video.addH265Codec(96, "profile-id=1;level-id=153");
            video.addAttribute("rtcp-fb:96 nack");
            video.addAttribute("rtcp-fb:96 nack pli");
            video.addAttribute("rtcp-fb:96 goog-remb");
            video.addAttribute("extmap:1 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay");
            video.addSSRC(ssrc, "video-stream", "stream1", "video-stream");
            _video_track = _peer_connection->addTrack(video);

            if (_video_track) {
                _video_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "video-stream", 96, rtc::H265RtpPacketizer::ClockRate);
                auto packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, _video_rtp_config);
                auto srReporter = std::make_shared<rtc::RtcpSrReporter>(_video_rtp_config);
                packetizer->addToChain(srReporter);

                auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
                packetizer->addToChain(nackResponder);

                _video_track->setMediaHandler(packetizer);

                MAIX_WEBRTC_LOG("Video track added: H.265/HEVC Main Profile, NACK/PLI/REMB enabled\n");
            }
        } else {
            video.addH264Codec(96, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4D0034");
            video.addAttribute("rtcp-fb:96 nack");
            video.addAttribute("rtcp-fb:96 nack pli");
            video.addAttribute("rtcp-fb:96 goog-remb");
            video.addAttribute("extmap:1 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay");
            video.addSSRC(ssrc, "video-stream", "stream1", "video-stream");
            _video_track = _peer_connection->addTrack(video);

            if (_video_track) {
                _video_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "video-stream", 96, rtc::H264RtpPacketizer::ClockRate);
                auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, _video_rtp_config);
                auto srReporter = std::make_shared<rtc::RtcpSrReporter>(_video_rtp_config);
                packetizer->addToChain(srReporter);

                auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
                packetizer->addToChain(nackResponder);

                _video_track->setMediaHandler(packetizer);

                MAIX_WEBRTC_LOG("Video track added: H.264 Main Profile (4D0034), packetization-mode=1, NACK/PLI/REMB enabled\n");
            }
        }
    }

    void add_audio_track() {
        std::lock_guard<std::mutex> lock(_media_mutex);
        if (!_peer_connection || !_has_audio) return;

        constexpr rtc::SSRC audio_ssrc = 2;  // video SSRC is 1
        constexpr uint8_t opus_pt = 97;
        auto audio = rtc::Description::Audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(opus_pt);
        audio.addSSRC(audio_ssrc, "audio-stream", "stream1", "audio-stream");
        _audio_track = _peer_connection->addTrack(audio);
        if (_audio_track) {
            _audio_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
                audio_ssrc, "audio-stream", opus_pt, rtc::OpusRtpPacketizer::DefaultClockRate);
            auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(_audio_rtp_config);
            packetizer->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
            packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
            packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(_audio_rtp_config));
            _audio_track->setMediaHandler(packetizer);

            auto rx = std::make_shared<G4c4OpusRx>(_shared_playback);
            _audio_rx = rx;
            // A standalone session starts muted, including after disconnect.
            // The existing decoder defaults to ON for legacy integration tests.
            if (std::getenv("MAIX_WEBRTC_STANDALONE")) rx->set_talk_state(false);

            _audio_track->onFrame(
                [weak_rx = std::weak_ptr<G4c4OpusRx>(rx)]
                (rtc::binary data, rtc::FrameInfo info) {
                    auto worker = weak_rx.lock();
                    if (!worker || data.empty()) return;

                    worker->enqueue(
                        data.data(),
                        data.size(),
                        info.timestamp,
                        info.payloadType
                    );
                });

            MAIX_WEBRTC_LOG("Audio track added: Opus 48 kHz, PT=97, SSRC=2\n");
        }
    }

    int video_frame_push(uint8_t *frame, size_t frame_size, uint64_t timestamp) {
        std::lock_guard<std::mutex> lock(_media_mutex);
        if (!_video_track || !_video_track->isOpen() || !frame || frame_size == 0) {
            return -1;
        }

        try {
            if (_video_rtp_config) {
                _video_rtp_config->timestamp = static_cast<uint32_t>(timestamp);
            }

            rtc::binary video_frame;
            video_frame.reserve(frame_size);
            for (size_t i = 0; i < frame_size; ++i) {
                video_frame.push_back(static_cast<std::byte>(frame[i]));
            }
            _video_track->send(video_frame);
        } catch (const std::exception &e) {
            MAIX_WEBRTC_LOG("Error sending video frame: %s\n", e.what());
            return -1;
        }

        return 0;
    }

    int audio_frame_push(uint8_t *frame, size_t frame_size, uint64_t timestamp) {
        std::lock_guard<std::mutex> lock(_media_mutex);
        if (!_audio_track || !_audio_track->isOpen() || !_has_audio || !frame || frame_size == 0) {
            _audio_tx_failed.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }

        try {
            if (!_audio_rtp_config) {
                _audio_tx_failed.fetch_add(1, std::memory_order_relaxed);
                return -1;
            }
            // Caller supplies elapsed 48 kHz audio samples, not wall-clock milliseconds.
            _audio_rtp_config->timestamp =
                _audio_rtp_config->startTimestamp + static_cast<uint32_t>(timestamp);
            rtc::binary audio_frame;
            audio_frame.reserve(frame_size);
            for (size_t i = 0; i < frame_size; ++i) {
                audio_frame.push_back(static_cast<std::byte>(frame[i]));
            }
            _audio_track->send(audio_frame);
            _audio_tx_sent.fetch_add(1, std::memory_order_relaxed);
            _audio_tx_bytes.fetch_add(frame_size, std::memory_order_relaxed);
        } catch (const std::exception &e) {
            _audio_tx_failed.fetch_add(1, std::memory_order_relaxed);
            MAIX_WEBRTC_LOG("Error sending audio frame: %s\n", e.what());
            return -1;
        }

        return 0;
    }

    void start() {
        if (_is_running) return;

        _is_running = true;

        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            peer = _peer_connection;
        }
        try {
            if (peer) peer->setLocalDescription();
        } catch (const std::exception &e) {
            MAIX_WEBRTC_LOG("Error setting local description: %s\n", e.what());
        }
    }    void stop() {
        _is_running = false;
    }

    bool is_video_track_open() {
        std::lock_guard<std::mutex> lock(_media_mutex);
        return _video_track && _video_track->isOpen();
    }

    bool is_audio_track_open() {
        std::lock_guard<std::mutex> lock(_media_mutex);
        return _audio_track && _audio_track->isOpen();
    }

    void set_talk_state(bool active) {
        std::lock_guard<std::mutex> lock(_media_mutex);
        if (_audio_rx) {
            _audio_rx->set_talk_state(active);
        }
    }

    bool is_talk_active() {
        std::lock_guard<std::mutex> lock(_media_mutex);
        return _audio_rx ? _audio_rx->is_talk_active() : false;
    }

    std::string get_audio_metrics_json() {
        json root;
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        root["timestamp_ms"] = now_ms;

        // Uplink stats
        json up;
        up["packets_sent"] = _audio_tx_sent.load(std::memory_order_relaxed);
        up["packets_failed"] = _audio_tx_failed.load(std::memory_order_relaxed);
        up["bytes_sent"] = _audio_tx_bytes.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            up["is_track_open"] = _audio_track && _audio_track->isOpen();
        }
        root["uplink"] = up;

        std::shared_ptr<maix::webrtc::MaixAudioUplinkDSP> dsp;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            dsp = _audio_dsp;
        }
        if (dsp) {
            try {
                root["uplink_dsp"] = json::parse(dsp->get_metrics_json());
            } catch (...) {
                root["uplink_dsp"] = dsp->get_metrics_json();
            }
        }

        // Downlink stats
        std::shared_ptr<G4c4OpusRx> rx;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            rx = _audio_rx;
        }

        if (rx) {
            auto s = rx->get_stats();
            json down;
            down["talk_active"] = rx->is_talk_active();
            down["talk_on_events"] = s.talk_on_events;
            down["talk_off_events"] = s.talk_off_events;
            down["talk_off_dropped_packets"] = s.talk_off_dropped_packets;
            down["packets_received"] = s.packets_received;
            down["packets_enqueued"] = s.packets_enqueued;
            down["dropped_busy"] = s.dropped_busy;
            down["dropped_overflow"] = s.dropped_overflow;
            down["dropped_inactive"] = s.dropped_inactive;
            down["packets_decoded"] = s.packets_decoded;
            down["decode_errors"] = s.decode_errors;
            down["samples_decoded"] = s.samples_decoded;
            down["decoder_resets"] = s.decoder_resets;
            down["current_queue_depth"] = s.current_queue_depth;
            down["peak_queue_depth"] = s.peak_queue_depth;

            json res;
            res["frames_in"] = s.resampler.frames_in;
            res["frames_out"] = s.resampler.frames_out;
            res["process_calls"] = s.resampler.process_calls;
            res["clips"] = s.resampler.clips;
            res["resets"] = s.resampler.resets;
            down["resampler"] = res;

            json pb;
            pb["frames_written"] = s.playback.frames_written;
            pb["dropped_overflow"] = s.playback.dropped_overflow;
            pb["dropped_busy"] = s.playback.dropped_busy;
            pb["alsa_errors"] = s.playback.alsa_errors;
            pb["underruns"] = s.playback.underruns;
            pb["talk_on_events"] = s.playback.talk_on_events;
            pb["talk_off_events"] = s.playback.talk_off_events;
            pb["talk_off_dropped_chunks"] = s.playback.talk_off_dropped_chunks;
            pb["talk_off_pcm_drops"] = s.playback.talk_off_pcm_drops;
            pb["current_queue_depth"] = s.playback.current_queue_depth;
            pb["peak_queue_depth"] = s.playback.peak_queue_depth;
            pb["device_open_count"] = s.playback.device_open_count;
            pb["device_close_count"] = s.playback.device_close_count;
            pb["last_write_us"] = s.playback.last_write_us;
            pb["is_device_open"] = s.playback.is_device_open;
            down["playback"] = pb;

            root["downlink"] = down;
        } else if (_shared_playback) {
            auto pb_stats = _shared_playback->get_stats();
            json down;
            down["talk_active"] = _shared_playback->is_talk_active();
            down["talk_on_events"] = pb_stats.talk_on_events;
            down["talk_off_events"] = pb_stats.talk_off_events;
            down["talk_off_dropped_packets"] = 0;
            down["packets_received"] = 0;
            down["packets_enqueued"] = 0;
            down["dropped_busy"] = pb_stats.dropped_busy;
            down["dropped_overflow"] = pb_stats.dropped_overflow;
            down["dropped_inactive"] = 0;
            down["packets_decoded"] = 0;
            down["decode_errors"] = 0;
            down["samples_decoded"] = 0;
            down["decoder_resets"] = 0;
            down["current_queue_depth"] = 0;
            down["peak_queue_depth"] = 0;

            json res;
            res["frames_in"] = 0;
            res["frames_out"] = 0;
            res["process_calls"] = 0;
            res["clips"] = 0;
            res["resets"] = 0;
            down["resampler"] = res;

            json pb;
            pb["frames_written"] = pb_stats.frames_written;
            pb["dropped_overflow"] = pb_stats.dropped_overflow;
            pb["dropped_busy"] = pb_stats.dropped_busy;
            pb["alsa_errors"] = pb_stats.alsa_errors;
            pb["underruns"] = pb_stats.underruns;
            pb["talk_on_events"] = pb_stats.talk_on_events;
            pb["talk_off_events"] = pb_stats.talk_off_events;
            pb["talk_off_dropped_chunks"] = pb_stats.talk_off_dropped_chunks;
            pb["talk_off_pcm_drops"] = pb_stats.talk_off_pcm_drops;
            pb["current_queue_depth"] = pb_stats.current_queue_depth;
            pb["peak_queue_depth"] = pb_stats.peak_queue_depth;
            pb["device_open_count"] = pb_stats.device_open_count;
            pb["device_close_count"] = pb_stats.device_close_count;
            pb["last_write_us"] = pb_stats.last_write_us;
            pb["is_device_open"] = pb_stats.is_device_open;
            down["playback"] = pb;

            root["downlink"] = down;
        } else {
            root["downlink"] = nullptr;
        }

        return root.dump();
    }

    void set_audio_dsp(std::shared_ptr<maix::webrtc::MaixAudioUplinkDSP> dsp) {
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            _audio_dsp = dsp;
        }
        if (_http_server) {
            _http_server->set_audio_dsp(dsp);
        }
    }

    std::shared_ptr<maix::webrtc::MaixAudioUplinkDSP> get_audio_dsp() {
        std::lock_guard<std::mutex> lock(_media_mutex);
        return _audio_dsp;
    }

    std::string get_local_description() {
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            peer = _peer_connection;
        }
        if (!peer) return "";
        auto description = peer->localDescription();
        if (description) {
            std::string sdp = std::string(*description);
            std::istringstream iss(sdp);
            std::ostringstream oss;
            std::string line;

            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.find("a=candidate:") == 0) {
                    if (line.find("::") != std::string::npos ||
                        std::count(line.begin(), line.end(), ':') > 5) {
                        MAIX_WEBRTC_LOG("Filtered IPv6 from SDP: %s\n", line.c_str());
                        continue;
                    }
                }
                oss << line << "\r\n";
            }

            return oss.str();
        }
        return "";
    }

    bool connect_signaling(const std::string &ip, int port, const std::string &client_id = "server") {
        _client_id = client_id;

        if (ip.empty()) {
            MAIX_WEBRTC_LOG("[WebRTC] Starting built-in signaling server on 0.0.0.0:%d\n", port);
            _builtin_signaling_server = std::make_shared<MaixWebRTCSignalingServer>("0.0.0.0", port);

            _builtin_signaling_server->set_on_request([this](const std::string &client_id, const json &message) {
                MAIX_WEBRTC_LOG("[WebRTC] Received request from %s, sending offer\n", client_id.c_str());

                std::string local_desc;
                for (int i = 0; i < 10; i++) {
                    local_desc = get_local_description();
                    if (!local_desc.empty()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                if (!local_desc.empty()) {
                    json offer_message = {
                        {"id", client_id},
                        {"type", "offer"},
                        {"sdp", local_desc}
                    };
                    _builtin_signaling_server->send_to_client(client_id, offer_message);
                    MAIX_WEBRTC_LOG("[WebRTC] Sent offer to %s\n", client_id.c_str());
                } else {
                    MAIX_WEBRTC_LOG("[WebRTC] Local description is empty after waiting\n");
                }
            });

            _builtin_signaling_server->set_on_message([this](const std::string &client_id, const json &message) {
                std::string msg_type = message.value("type", "");
                MAIX_WEBRTC_LOG("[WebRTC] Received %s from %s\n", msg_type.c_str(), client_id.c_str());

                if (msg_type == "answer") {
                    std::string sdp = message.value("sdp", "");
                    if (!sdp.empty()) {
                        set_remote_description(sdp);
                        MAIX_WEBRTC_LOG("[WebRTC] Set remote description from answer\n");
                    }
                }
            });

            _builtin_signaling_server->set_on_client_disconnected([this](const std::string &client_id) {
                MAIX_WEBRTC_LOG("[WebRTC] Client %s disconnected, resetting connection...\n", client_id.c_str());
                set_talk_state(false);
                reset_peer_connection();
            });

            if (!_builtin_signaling_server->start()) {
                MAIX_WEBRTC_LOG("[WebRTC] Failed to start built-in signaling server\n");
                return false;
            }
            _use_builtin_signaling = true;
            MAIX_WEBRTC_LOG("[WebRTC] Built-in signaling server started successfully\n");
            return true;
        }

        _signaling_ws = std::make_shared<rtc::WebSocket>();

        _signaling_ws->onOpen([this]() {
            MAIX_WEBRTC_LOG("WebSocket connected to signaling server\n");
            if (_on_connected) {
                _on_connected();
            }
        });

        _signaling_ws->onClosed([this]() {
            MAIX_WEBRTC_LOG("WebSocket closed\n");
            if (_on_disconnected) {
                _on_disconnected();
            }
        });

        _signaling_ws->onError([](const std::string &error) {
            std::cerr << "WebSocket error: " << error << std::endl;
        });

        _signaling_ws->onMessage([this](std::variant<rtc::binary, std::string> data) {
            if (!std::holds_alternative<std::string>(data))
                return;

            try {
                json message = json::parse(std::get<std::string>(data));
                std::string type = message["type"];

                std::cout << "Received signaling message: " << type << std::endl;

                if (type == "request") {
                    std::cout << "Client requested stream, sending offer" << std::endl;

                    std::string client_id = message["id"];

                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    std::string local_desc = get_local_description();
                    if (!local_desc.empty()) {
                        json offer_message = {
                            {"id", client_id},
                            {"type", "offer"},
                            {"sdp", local_desc}
                        };
                        _signaling_ws->send(offer_message.dump());
                        _offer_sent = true;
                        std::cout << "Sent offer to client: " << client_id << std::endl;
                    } else {
                        std::cout << "Local description is empty, waiting..." << std::endl;
                    }

                } else if (type == "answer") {
                    std::string sdp = message["sdp"];
                    std::cout << "Received answer SDP" << std::endl;
                    set_remote_description(sdp);
                }
            } catch (const std::exception &e) {
                std::cerr << "Error parsing signaling message: " << e.what() << std::endl;
            }
        });

        std::string signaling_url = "ws://" + ip + ":" + std::to_string(port) + "/" + _client_id;
        std::cout << "Connecting to signaling server: " << signaling_url << std::endl;

        try {
            _signaling_ws->open(signaling_url);
        } catch (const std::exception &e) {
            std::cerr << "Failed to connect to signaling server: " << e.what() << std::endl;
            return false;
        }

        std::cout << "Waiting for signaling connection..." << std::endl;
        int wait_count = 0;
        while (!_signaling_ws->isOpen() && !_signaling_ws->isClosed() && wait_count < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }

        if (!_signaling_ws->isOpen()) {
            std::cerr << "Failed to connect to signaling server" << std::endl;
            return false;
        }

        std::cout << "Connected to signaling server" << std::endl;
        return true;
    }

    bool is_signaling_connected() {
        return _signaling_ws && _signaling_ws->isOpen();
    }

    bool is_offer_sent() {
        return _offer_sent;
    }

    void set_on_connected(std::function<void()> callback) {
        _on_connected = callback;
    }

    void set_on_disconnected(std::function<void()> callback) {
        _on_disconnected = callback;
    }

    void set_remote_description(const std::string &description) {
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            peer = _peer_connection;
        }
        if (!peer) return;
        try {
            // Both signaling handlers call this only for a browser SDP answer.
            peer->setRemoteDescription(rtc::Description(description, "answer"));
        } catch (const std::exception &e) {
            std::cerr << "Error setting remote description: " << e.what() << std::endl;
        }
    }

    void add_remote_candidate(const std::string &candidate, const std::string &mid = "") {
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(_media_mutex);
            peer = _peer_connection;
        }
        if (!peer) return;
        try {
            peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
        } catch (const std::exception &e) {
            std::cerr << "Error adding remote candidate: " << e.what() << std::endl;
        }
    }

};

class MaixWebRTCServerBuilder {
    std::string _ice_server = "";
    bool _has_audio = false;
    int _audio_sample_rate = 48000;
    int _audio_channels = 1;
    VideoCodec _video_codec = VideoCodec::H264;
    bool _enable_http_server = false;
    std::string _http_host = "0.0.0.0";
    uint16_t _http_port = 8080;
    std::string _stun_server = "stun:stun.l.google.com:19302";
    uint16_t _ws_port = 8001;

public:
    MaixWebRTCServerBuilder() {
    }

    ~MaixWebRTCServerBuilder() {
    }

    MaixWebRTCServerBuilder &set_ice_server(std::string ice_server) {
        _ice_server = ice_server;
        return *this;
    }

    MaixWebRTCServerBuilder &set_audio(bool en) {
        _has_audio = en;
        return *this;
    }

    MaixWebRTCServerBuilder &set_audio_sample_rate(int sample_rate) {
        _audio_sample_rate = sample_rate;
        return *this;
    }

    MaixWebRTCServerBuilder &set_audio_channels(int channels) {
        _audio_channels = channels;
        return *this;
    }

    MaixWebRTCServerBuilder &set_video_codec(VideoCodec codec) {
        _video_codec = codec;
        return *this;
    }

    MaixWebRTCServerBuilder &set_http_server(bool enable, const std::string &host = "0.0.0.0",
                                             uint16_t port = 8080) {
        _enable_http_server = enable;
        _http_host = host;
        _http_port = port;
        return *this;
    }

    MaixWebRTCServerBuilder &set_stun_server(const std::string &stun_server) {
        _stun_server = stun_server;
        return *this;
    }

    MaixWebRTCServerBuilder &set_ws_port(uint16_t ws_port) {
        _ws_port = ws_port;
        return *this;
    }

    MaixWebRTCServer *build() {
        rtc::Configuration config;

        if (!_ice_server.empty()) {
            config.iceServers.emplace_back(_ice_server);
        }

        std::shared_ptr<rtc::PeerConnection> peer_connection;
        try {
            peer_connection = std::make_shared<rtc::PeerConnection>(config);
        } catch (const std::exception &e) {
            std::cerr << "Failed to create PeerConnection: " << e.what() << std::endl;
            throw "webrtc peer connection creation failed";
        }

        int *clients = (int *)malloc(sizeof(int));
        if (clients == nullptr) {
            throw "alloc memory failed";
        }
        *clients = 0;

        MaixWebRTCServer *new_server = new MaixWebRTCServer(clients, peer_connection, _has_audio, _video_codec);

        new_server->_peer_config = config;

        new_server->add_video_track();
        if (_has_audio) {
            new_server->add_audio_track();
        }

        // Start HTTP server if enabled
        if (_enable_http_server) {
            new_server->_http_server = std::make_shared<MaixWebRTCHttpServer>(_http_host, _http_port, _stun_server, _ws_port);
            if (new_server->_audio_dsp) {
                new_server->_http_server->set_audio_dsp(new_server->_audio_dsp);
            }
            new_server->_http_server->set_metrics_callback([new_server]() {
                return new_server->get_audio_metrics_json();
            });
            new_server->_http_server->set_talk_callback([new_server](bool active) {
                new_server->set_talk_state(active);
            });
            if (new_server->_http_server->start()) {
                new_server->_use_http_server = true;
                MAIX_WEBRTC_LOG("[HttpServer] Access WebRTC client at %s\n", new_server->_http_server->get_url().c_str());
            } else {
                MAIX_WEBRTC_LOG("[HttpServer] Failed to start HTTP server\n");
            }
        }

        return new_server;
    }
};
