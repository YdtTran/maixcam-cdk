#pragma once

#include "rtc/rtc.hpp"
#include "nlohmann/json.hpp"
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
#include <map>
#include <functional>
#include <cstdlib>
#include <stdexcept>
#include <atomic>
#include <condition_variable>

using json = nlohmann::json;

class MaixWebRTCSignalingServer {
private:
    std::string _host;
    uint16_t _port;
    std::shared_ptr<rtc::WebSocketServer> _ws_server;
    std::map<std::string, std::shared_ptr<rtc::WebSocket>> _clients;
    std::map<std::string, std::shared_ptr<rtc::WebSocket>> _pending;
    std::mutex _clients_mutex;
    std::atomic<bool> _is_running;
    std::thread _watchdog;
    std::condition_variable _watch_cv;
    std::chrono::steady_clock::time_point _last_activity;
    std::function<void(const std::string &, const json &)> _on_request;
    std::function<void(const std::string &, const json &)> _on_message;
    std::function<void(const std::string &)> _on_client_disconnected;

public:
    MaixWebRTCSignalingServer(const std::string &host = "0.0.0.0", uint16_t port = 8001)
        : _host(host), _port(port), _is_running(false) {
        // Start only after callbacks have been installed.
    }

    ~MaixWebRTCSignalingServer() {
        stop();
    }

    bool start() {
        if (_is_running) return true;

        try {
            rtc::WebSocketServerConfiguration config;
            config.port = _port;
            config.bindAddress = _host;
            config.connectionTimeout = std::chrono::seconds(15);
            config.maxMessageSize = 65536;
            const char *cert = std::getenv("MAIX_WEBRTC_TLS_CERT");
            const char *key = std::getenv("MAIX_WEBRTC_TLS_KEY");
            if (cert || key) {
                if (!cert || !key) throw std::runtime_error("TLS requires certificate and key");
                config.enableTls = true;
                config.certificatePemFile = cert;
                config.keyPemFile = key;
            }

            _ws_server = std::make_shared<rtc::WebSocketServer>(config);

            _ws_server->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
                handle_client(ws);
            });

            _is_running = true;
            _watchdog = std::thread([this]() {
                std::unique_lock<std::mutex> lock(_clients_mutex);
                while (_is_running) {
                    _watch_cv.wait_for(lock, std::chrono::seconds(5));
                    if (!_is_running) break;
                    if (!_clients.empty() && std::chrono::steady_clock::now() - _last_activity > std::chrono::seconds(30)) {
                        auto socket = _clients.begin()->second;
                        lock.unlock();
                        socket->forceClose();
                        lock.lock();
                    }
                }
            });
            MAIX_WEBRTC_LOG("[SignalingServer] Started on ws://%s:%d\n", _host.c_str(), _port);
            return true;
        } catch (const std::exception &e) {
            MAIX_WEBRTC_LOG("[SignalingServer] Failed to start: %s\n", e.what());
            _is_running = false;
            return false;
        }
    }

    void stop() {
        _is_running = false;
        _watch_cv.notify_all();
        if (_watchdog.joinable()) _watchdog.join();
        if (_ws_server) {
            _ws_server->stop();
            _ws_server = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(_clients_mutex);
            _clients.clear();
            _pending.clear();
        }
    }

    bool is_running() const {
        return _is_running;
    }

    uint16_t get_port() const {
        if (_ws_server) {
            return _ws_server->port();
        }
        return _port;
    }

    void set_on_request(std::function<void(const std::string &, const json &)> callback) {
        _on_request = callback;
    }

    void set_on_message(std::function<void(const std::string &, const json &)> callback) {
        _on_message = callback;
    }

    void set_on_client_disconnected(std::function<void(const std::string &)> callback) {
        _on_client_disconnected = callback;
    }

    void send_to_client(const std::string &client_id, const json &message) {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        auto it = _clients.find(client_id);
        if (it != _clients.end() && it->second) {
            try {
                it->second->send(message.dump());
                MAIX_WEBRTC_LOG("[SignalingServer] Sent message to %s\n", client_id.c_str());
            } catch (const std::exception &e) {
                MAIX_WEBRTC_LOG("[SignalingServer] Failed to send to %s: %s\n", client_id.c_str(), e.what());
            }
        }
    }

private:
    void handle_client(std::shared_ptr<rtc::WebSocket> ws) {
        auto client_id_ptr = std::make_shared<std::string>(
            "client_" + std::to_string(reinterpret_cast<uintptr_t>(ws.get())));
        auto admitted = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(_clients_mutex);
            if (_pending.size() >= 4) { ws->close(); return; }
            // Retain the public socket while TLS opens; callbacks hold weak
            // references so they do not create a self-owning reference cycle.
            _pending[*client_id_ptr] = ws;
        }
        std::weak_ptr<rtc::WebSocket> weak_ws = ws;
        // onClient precedes the TLS/HTTP handshake. The URL is available only
        // in onOpen; checking it earlier would abort valid TLS handshakes.
        ws->onOpen([this, weak_ws, client_id_ptr, admitted]() {
            auto socket = weak_ws.lock();
            if (!socket) return;
            const char *token = std::getenv("MAIX_WEBRTC_SIGNAL_TOKEN");
            if (token) {
                const auto path = socket->path();
                const std::string prefix = "/" + std::string(token) + "/";
                if (!path || path->compare(0, prefix.size(), prefix) != 0) {
                    socket->close();
                    return;
                }
            }
            bool busy;
            {
                std::lock_guard<std::mutex> lock(_clients_mutex);
                _pending.erase(*client_id_ptr);
                busy = !_clients.empty();
                if (!busy) {
                    _clients[*client_id_ptr] = socket;
                    _last_activity = std::chrono::steady_clock::now();
                    *admitted = true;
                }
            }
            if (busy) {
                socket->send(std::string("{\"type\":\"busy\"}"));
                socket->close();
            }
        });
        ws->onMessage([this, weak_ws, client_id_ptr, admitted](std::variant<rtc::binary, std::string> data) {
            if (!*admitted || !std::holds_alternative<std::string>(data)) return;
            try {
                {
                    std::lock_guard<std::mutex> lock(_clients_mutex);
                    _last_activity = std::chrono::steady_clock::now();
                }
                auto message = json::parse(std::get<std::string>(data));
                const std::string type = message.value("type", "");
                if (type == "ping") {
                    if (auto socket = weak_ws.lock()) socket->send(std::string("{\"type\":\"pong\"}"));
                } else if (type == "request" && _on_request) {
                    _on_request(*client_id_ptr, message);
                } else if (type == "answer" && _on_message) {
                    _on_message(*client_id_ptr, message);
                }
            } catch (const std::exception &e) {
                MAIX_WEBRTC_LOG("[SignalingServer] Invalid message: %s\n", e.what());
            }
        });
        ws->onClosed([this, client_id_ptr, admitted]() {
            {
                std::lock_guard<std::mutex> lock(_clients_mutex);
                _pending.erase(*client_id_ptr);
            }
            if (!admitted->exchange(false)) return;
            // Keep admission closed until the previous peer has been reset.
            if (_on_client_disconnected) _on_client_disconnected(*client_id_ptr);
            std::lock_guard<std::mutex> lock(_clients_mutex);
            _clients.erase(*client_id_ptr);
        });

        ws->onError([](const std::string &error) {
            MAIX_WEBRTC_LOG("[SignalingServer] WebSocket error: %s\n", error.c_str());
        });
    }
};
