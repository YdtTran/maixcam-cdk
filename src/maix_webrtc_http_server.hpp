#pragma once

#include <string>
#include <memory>
#include <iostream>

#ifdef NDEBUG
#define MAIX_WEBRTC_LOG(...)
#else
#define MAIX_WEBRTC_LOG(...) printf(__VA_ARGS__)
#endif

#include <thread>
#include <atomic>
#include <functional>
#include <cstdlib>
#include <stdexcept>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "httplib.h"

class MaixWebRTCHttpServer {
private:
    std::string _host;
    uint16_t _port;
    std::string _stun_server;
    uint16_t _ws_port;
    std::unique_ptr<httplib::Server> _server;
    std::unique_ptr<std::thread> _server_thread;
    std::atomic<bool> _is_running;
    std::function<std::string()> _metrics_callback;
    std::function<void(bool)> _talk_callback;

    static constexpr const char WEBRTC_INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>MaixCAM G4c4 Operator Console</title>
    <style>
        :root {
            --ground: #141416;
            --surface: #101013;
            --raised: #1c1c20;
            --border: #222226;
            --rule: #19191d;
            --control: #26262b;
            --control-hover: #323238;
            --text: #ffffff;
            --muted: #84868c;
            --accent: #4a8ad6;
            --positive: #44a282;
            --negative: #e5566f;
            --warning: #d9a441;
            --pill-positive-bg: #152620;
            --pill-positive-ink: #5fc39c;
            --pill-negative-bg: #2b171c;
            --pill-negative-ink: #e5566f;
            --pill-warning-bg: #292215;
            --pill-warning-ink: #d9a441;
            --pill-neutral-bg: #1c1c20;
            --pill-neutral-ink: #b0b2b8;
            --radius-card: 6px;
            --radius-control: 6px;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        html, body {
            height: 100vh;
            max-height: 100vh;
            overflow: hidden;
            background-color: var(--ground);
            color: var(--text);
            font-family: "Public Sans", -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            font-size: 13px;
            line-height: 1.4;
            display: flex;
            flex-direction: column;
            -webkit-font-smoothing: antialiased;
        }

        ::-webkit-scrollbar {
            width: 5px;
            height: 5px;
        }
        ::-webkit-scrollbar-track {
            background: var(--surface);
        }
        ::-webkit-scrollbar-thumb {
            background: var(--control);
            border-radius: 3px;
        }
        ::-webkit-scrollbar-thumb:hover {
            background: var(--control-hover);
        }

        .tabular {
            font-variant-numeric: tabular-nums;
            font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
        }

        header.app-shell {
            height: 44px;
            background: var(--surface);
            border-bottom: 1px solid var(--border);
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 0 16px;
            gap: 12px;
            flex-shrink: 0;
            z-index: 50;
        }

        .brand-group {
            display: flex;
            align-items: center;
            gap: 10px;
        }

        .brand-mark {
            width: 24px;
            height: 24px;
            background: var(--raised);
            border: 1px solid var(--border);
            border-radius: var(--radius-control);
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 11px;
            font-weight: 700;
            color: var(--accent);
        }

        .brand-title {
            font-size: 13px;
            font-weight: 600;
            color: var(--text);
        }

        .brand-tag {
            font-size: 11px;
            color: var(--muted);
            border-left: 1px solid var(--border);
            padding-left: 8px;
        }

        .status-indicators {
            display: flex;
            align-items: center;
            gap: 8px;
        }

        .status-pill {
            display: inline-flex;
            align-items: center;
            gap: 6px;
            font-size: 11px;
            font-weight: 500;
            padding: 3px 8px;
            border-radius: 999px;
            background: var(--pill-neutral-bg);
            color: var(--pill-neutral-ink);
            border: 1px solid var(--border);
        }

        .status-pill.connected {
            background: var(--pill-positive-bg);
            color: var(--pill-positive-ink);
            border-color: rgba(68, 162, 130, 0.3);
        }

        .status-pill.connecting {
            background: var(--pill-warning-bg);
            color: var(--pill-warning-ink);
            border-color: rgba(217, 164, 65, 0.3);
        }

        .status-pill.disconnected {
            background: var(--pill-negative-bg);
            color: var(--pill-negative-ink);
            border-color: rgba(229, 86, 111, 0.3);
        }

        .status-dot {
            width: 6px;
            height: 6px;
            border-radius: 50%;
            background: currentColor;
        }

        .session-clock {
            font-size: 12px;
            color: var(--muted);
        }

        main.workspace {
            flex: 1;
            min-height: 0;
            display: grid;
            grid-template-columns: 1.25fr 0.75fr;
            gap: 12px;
            padding: 12px;
            overflow: hidden;
            max-width: 1720px;
            width: 100%;
            margin: 0 auto;
        }

        @media (max-width: 1100px) {
            main.workspace {
                grid-template-columns: 1fr;
                overflow-y: auto;
            }
        }

        .column-stage, .column-vault {
            display: flex;
            flex-direction: column;
            gap: 12px;
            min-height: 0;
            height: 100%;
            overflow: hidden;
        }

        .card {
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: var(--radius-card);
            display: flex;
            flex-direction: column;
            min-height: 0;
            overflow: hidden;
        }

        .video-card {
            flex: 1;
            min-height: 0;
            display: flex;
            flex-direction: column;
            background: #000000;
            position: relative;
        }

        .video-container {
            flex: 1;
            min-height: 0;
            position: relative;
            background: #000000;
            display: flex;
            align-items: center;
            justify-content: center;
            overflow: hidden;
        }

        video#video {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: none;
            transition: transform 0.25s cubic-bezier(0.2, 0, 0, 1);
        }

        canvas#video-canvas {
            width: 100%;
            height: 100%;
            object-fit: contain;
            display: block;
        }

        .video-meta-overlay {
            position: absolute;
            top: 10px;
            left: 10px;
            display: flex;
            gap: 6px;
            pointer-events: none;
            z-index: 10;
        }

        .meta-chip {
            background: rgba(16, 16, 19, 0.9);
            border: 1px solid rgba(255, 255, 255, 0.12);
            color: var(--text);
            padding: 3px 8px;
            border-radius: var(--radius-control);
            font-size: 11px;
            font-weight: 500;
        }

        /* Minimal Call Action Dock: Pure Icons Only */
        .call-bar {
            position: absolute;
            bottom: 14px;
            left: 50%;
            transform: translateX(-50%);
            background: rgba(20, 20, 24, 0.95);
            border: 1px solid rgba(255, 255, 255, 0.16);
            padding: 6px 10px;
            border-radius: 999px;
            display: flex;
            align-items: center;
            gap: 8px;
            z-index: 20;
        }

        .call-btn {
            width: 38px;
            height: 38px;
            border-radius: 50%;
            background: var(--control);
            border: 1px solid var(--border);
            color: var(--text);
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
            transition: transform 0.15s, background-color 0.15s, border-color 0.15s, color 0.15s;
            position: relative;
            outline: none;
        }

        .call-btn:hover {
            background: var(--control-hover);
            transform: translateY(-1px);
        }

        .call-btn:active {
            transform: translateY(1px);
        }

        .call-btn.btn-mic-muted {
            background: #2b171c;
            border-color: rgba(229, 86, 111, 0.4);
            color: var(--negative);
        }

        .call-btn.btn-mic-live {
            background: #152620;
            border-color: rgba(68, 162, 130, 0.5);
            color: var(--positive);
        }

        .call-btn.btn-listen-active {
            background: #172433;
            border-color: rgba(74, 138, 214, 0.5);
            color: var(--accent);
        }

        .call-btn.btn-listen-muted {
            background: var(--control);
            color: var(--muted);
        }

        .call-btn.btn-record-active {
            background: #2b171c;
            border-color: var(--negative);
            color: var(--negative);
            animation: pulseRecord 1.2s infinite alternate;
        }

        @keyframes pulseRecord {
            0% { transform: scale(1); }
            100% { transform: scale(1.08); }
        }

        .call-divider {
            width: 1px;
            height: 20px;
            background: rgba(255, 255, 255, 0.14);
            margin: 0 2px;
        }

        /* Telemetry KPI Strip */
        .kpi-strip {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            border-top: 1px solid var(--border);
            background: var(--surface);
            flex-shrink: 0;
        }

        .kpi-cell {
            padding: 8px 12px;
            border-right: 1px solid var(--border);
            display: flex;
            flex-direction: column;
            gap: 2px;
        }

        .kpi-cell:last-child {
            border-right: none;
        }

        .kpi-label {
            font-size: 11px;
            color: var(--muted);
        }

        .kpi-value {
            font-size: 13px;
            font-weight: 600;
            color: var(--text);
        }

        .kpi-sub {
            font-size: 10px;
            color: var(--muted);
        }

        /* Right Column (Vault & Diagnostics) */
        .tab-bar {
            display: flex;
            border-bottom: 1px solid var(--border);
            background: var(--raised);
            flex-shrink: 0;
        }

        .tab-btn {
            height: 38px;
            padding: 0 16px;
            background: transparent;
            border: none;
            border-bottom: 2px solid transparent;
            color: var(--muted);
            cursor: pointer;
            font-size: 12px;
            font-weight: 600;
            display: inline-flex;
            align-items: center;
            gap: 6px;
            transition: color 0.15s;
        }

        .tab-btn:hover {
            color: var(--text);
        }

        .tab-btn.active {
            color: var(--text);
            border-bottom-color: var(--accent);
            background: var(--surface);
        }

        /* Records Sub-Box */
        .recorder-banner {
            padding: 10px 14px;
            background: var(--raised);
            border-bottom: 1px solid var(--border);
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 8px;
            flex-shrink: 0;
        }

        .record-title-group {
            display: flex;
            align-items: center;
            gap: 8px;
        }

        .rec-dot {
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: var(--muted);
        }

        .rec-dot.active {
            background: var(--negative);
        }

        .rec-label {
            font-size: 12px;
            font-weight: 600;
            color: var(--text);
        }

        .rec-timer {
            font-size: 12px;
            font-weight: 600;
            color: var(--negative);
        }

        .records-table-wrap {
            flex: 1;
            min-height: 0;
            overflow-y: auto;
            overflow-x: hidden;
        }

        table.records-table {
            width: 100%;
            border-collapse: collapse;
            text-align: left;
            font-size: 12px;
        }

        table.records-table th {
            padding: 8px 12px;
            background: var(--surface);
            color: var(--muted);
            font-size: 11px;
            font-weight: 500;
            border-bottom: 1px solid var(--border);
            position: sticky;
            top: 0;
            z-index: 5;
        }

        table.records-table td {
            padding: 8px 12px;
            border-bottom: 1px solid var(--rule);
            color: var(--text);
            vertical-align: middle;
        }

        table.records-table tr:hover td {
            background: rgba(255, 255, 255, 0.02);
        }

        .record-tag {
            display: inline-block;
            font-size: 10px;
            padding: 2px 6px;
            border-radius: 2px;
            font-weight: 600;
        }

        .tag-video {
            background: #172433;
            color: #7ab2f5;
            border: 1px solid rgba(74, 138, 214, 0.3);
        }

        .tag-downlink {
            background: #152620;
            color: #5fc39c;
            border: 1px solid rgba(68, 162, 130, 0.3);
        }

        .tag-test {
            background: #292215;
            color: #e5b356;
            border: 1px solid rgba(217, 164, 65, 0.3);
        }

        .table-actions {
            display: flex;
            align-items: center;
            gap: 6px;
        }

        .btn-action-sm {
            padding: 3px 8px;
            font-size: 11px;
            height: 24px;
            background: var(--control);
            border: 1px solid var(--border);
            border-radius: var(--radius-control);
            color: var(--text);
            cursor: pointer;
            display: inline-flex;
            align-items: center;
            gap: 4px;
            text-decoration: none;
            transition: background-color 0.15s, border-color 0.15s, color 0.15s;
        }

        .btn-action-sm:hover {
            background: var(--control-hover);
        }

        .btn-action-del {
            color: var(--muted);
        }

        .btn-action-del:hover {
            background: #2b171c;
            border-color: rgba(229, 86, 111, 0.4);
            color: var(--negative);
        }

        /* Vault Inline Player */
        .vault-player-deck {
            padding: 10px 14px;
            background: var(--surface);
            border-top: 1px solid var(--border);
            display: flex;
            flex-direction: column;
            gap: 6px;
            flex-shrink: 0;
        }

        .player-info {
            display: flex;
            justify-content: space-between;
            align-items: center;
        }

        .player-title {
            font-size: 12px;
            font-weight: 600;
            color: var(--text);
        }

        .player-meta {
            font-size: 11px;
            color: var(--muted);
        }

        audio#vault-audio {
            width: 100%;
            height: 30px;
            outline: none;
        }

        .spec-list {
            flex: 1;
            min-height: 0;
            overflow-y: auto;
            display: flex;
            flex-direction: column;
        }

        .spec-row {
            padding: 9px 14px;
            border-bottom: 1px solid var(--rule);
            display: flex;
            justify-content: space-between;
            align-items: center;
        }

        .spec-name {
            font-size: 12px;
            color: var(--muted);
        }

        .spec-val {
            font-size: 12px;
            font-weight: 500;
            color: var(--text);
            text-align: right;
        }

        footer.app-footer {
            height: 26px;
            background: var(--surface);
            border-top: 1px solid var(--border);
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 0 16px;
            font-size: 11px;
            color: var(--muted);
            flex-shrink: 0;
        }

        .icon {
            width: 16px;
            height: 16px;
            stroke-width: 2;
            stroke: currentColor;
            fill: none;
            stroke-linecap: round;
            stroke-linejoin: round;
            flex-shrink: 0;
        }
    </style>
</head>
<body>
    <header class="app-shell">
        <div class="brand-group">
            <div class="brand-mark">MC</div>
            <span class="brand-title">MaixCAM G4c4</span>
            <span class="brand-tag">Operator Station</span>
        </div>

        <div class="status-indicators">
            <div id="conn-status-badge" class="status-pill connecting">
                <span class="status-dot"></span>
                <span id="conn-text">Connecting</span>
            </div>
            <div class="status-pill connected">
                <span class="status-dot"></span>
                <span>EDIFIER X2 Plus</span>
            </div>
            <div id="rotation-badge" class="status-pill">
                <span>Rotation: 0 deg</span>
            </div>
        </div>

        <span id="clock" class="session-clock tabular">--:--:-- UTC</span>
    </header>

    <main class="workspace">
        <div class="column-stage">
            <div class="card video-card">
                <div class="video-container">
                    <video id="video" autoplay playsinline muted></video>
                    <canvas id="video-canvas"></canvas>
                    
                    <div class="video-meta-overlay">
                        <span id="res-badge" class="meta-chip tabular">640 × 480 @ 30fps</span>
                        <span class="meta-chip">H.264 High</span>
                        <span class="meta-chip">MMF VPSS</span>
                    </div>

                    <!-- Clean Call Dock: Pure Icons Only -->
                    <div class="call-bar">
                        <button id="listen-btn" class="call-btn btn-listen-muted" title="Toggle Field Audio (Listen)">
                            <svg class="icon" viewBox="0 0 24 24"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/></svg>
                        </button>

                        <button id="talk-btn" class="call-btn btn-mic-muted" title="Toggle Operator Microphone (Talk)">
                            <svg class="icon" viewBox="0 0 24 24"><path d="m18.54 18.54a10.8 10.8 0 0 1-6.54 2.46c-5.52 0-10-4.48-10-10 0-2.45.88-4.7 2.34-6.46"/><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><line x1="1" x2="23" y1="1" y2="23"/></svg>
                        </button>

                        <div class="call-divider"></div>

                        <button id="rotate-btn" class="call-btn" title="Rotate Camera View (0 / 90 / 180 / 270 deg)" onclick="toggleRotationUI()">
                            <svg class="icon" viewBox="0 0 24 24"><path d="M21.5 2v6h-6"/><path d="M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/></svg>
                        </button>

                        <button id="record-btn" class="call-btn" title="Record Video & Audio Stream" onclick="toggleSessionRecording()">
                            <svg class="icon" viewBox="0 0 24 24"><circle cx="12" cy="12" r="7" fill="currentColor"/></svg>
                        </button>

                        <button id="snapshot-btn" class="call-btn" title="Capture Still Snapshot Frame" onclick="captureSnapshot()">
                            <svg class="icon" viewBox="0 0 24 24"><path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/><circle cx="12" cy="13" r="4"/></svg>
                        </button>

                        <button id="fullscreen-btn" class="call-btn" title="Toggle Fullscreen">
                            <svg class="icon" viewBox="0 0 24 24"><path d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"/></svg>
                        </button>
                    </div>
                </div>

                <div class="kpi-strip">
                    <div class="kpi-cell">
                        <span class="kpi-label">Uplink Mic (ADC)</span>
                        <span id="tel-uplink" class="kpi-value tabular">0 pkts (0.0 KB)</span>
                        <span class="kpi-sub">Opus 48 kHz Mono</span>
                    </div>
                    <div class="kpi-cell">
                        <span class="kpi-label">Downlink Decoded</span>
                        <span id="tel-downlink" class="kpi-value tabular">0 / 0 pkts</span>
                        <span class="kpi-sub">Opus RTP PT 97</span>
                    </div>
                    <div class="kpi-cell">
                        <span class="kpi-label">A2DP Playback</span>
                        <span id="tel-playback" class="kpi-value tabular">0 frames</span>
                        <span class="kpi-sub">EDIFIER X2 Plus</span>
                    </div>
                    <div class="kpi-cell">
                        <span class="kpi-label">ALSA Underruns</span>
                        <span id="tel-underruns" class="kpi-value tabular">0 underruns</span>
                        <span class="kpi-sub">Playback Safe</span>
                    </div>
                </div>
            </div>
        </div>

        <div class="column-vault">
            <div class="card" style="height: 100%;">
                <div class="tab-bar">
                    <button id="tab-records-btn" class="tab-btn active" onclick="switchTab('records')">
                        <svg class="icon" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="3"/></svg>
                        <span>Video & Audio Records</span>
                    </button>
                    <button id="tab-hw-btn" class="tab-btn" onclick="switchTab('hw')">
                        <svg class="icon" viewBox="0 0 24 24"><rect width="18" height="18" x="3" y="3" rx="2"/><path d="M7 7h10v10H7z"/><line x1="3" x2="7" y1="9" y2="9"/><line x1="3" x2="7" y1="15" y2="15"/><line x1="17" x2="21" y1="9" y2="9"/><line x1="17" x2="21" y1="15" y2="15"/></svg>
                        <span>Hardware & Stack</span>
                    </button>
                </div>

                <div id="tab-records-content" style="display:flex; flex-direction:column; flex:1; min-height:0;">
                    <div class="recorder-banner">
                        <div class="record-title-group">
                            <span id="rec-dot" class="rec-dot"></span>
                            <span id="rec-status-label" class="rec-label">Full Video+Audio Capture</span>
                            <span id="rec-timer" class="rec-timer tabular" style="display:none;">00:00:00</span>
                        </div>
                        <div style="display:flex; align-items:center; gap:6px;">
                            <select id="rec-format-select" class="btn-action-sm tabular" style="height:26px; font-size:11px; padding:0 6px; cursor:pointer;" title="Choose recording format">
                                <option value="mp4">Format: MP4 (H.264)</option>
                                <option value="webm">Format: WebM</option>
                                <option value="wav">Format: Audio Only</option>
                            </select>
                        </div>
                    </div>

                    <div class="records-table-wrap">
                        <table class="records-table">
                            <thead>
                                <tr>
                                    <th>Asset / Stream</th>
                                    <th>Format & Duration</th>
                                    <th>Action</th>
                                </tr>
                            </thead>
                            <tbody id="records-table-body">
                                <tr>
                                    <td>
                                        <div style="font-weight:600;">session_capture_1080p.webm</div>
                                        <span class="record-tag tag-video">Video+Audio Stream</span>
                                    </td>
                                    <td class="tabular">1080p · 48 kHz</td>
                                    <td>
                                        <div class="table-actions">
                                            <button class="btn-action-sm" onclick="playRecord('downlink_pc_to_maix.wav', 'Session Video+Audio Capture', '1080p WebM · 48 kHz')">
                                                <svg class="icon" viewBox="0 0 24 24"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                                <span>Play</span>
                                            </button>
                                            <button class="btn-action-sm btn-action-del" onclick="deleteRecord(this, 'session_capture_1080p.webm')" title="Delete recording">
                                                <svg class="icon" viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg>
                                            </button>
                                        </div>
                                    </td>
                                </tr>
                                <tr>
                                    <td>
                                        <div style="font-weight:600;">downlink_pc_to_maix.wav</div>
                                        <span class="record-tag tag-downlink">Downlink Master</span>
                                    </td>
                                    <td class="tabular">27.46s · 48k Stereo</td>
                                    <td>
                                        <div class="table-actions">
                                            <button class="btn-action-sm" onclick="playRecord('downlink_pc_to_maix.wav', 'Downlink Master (PC -> Maix -> Headset)', '27.46s · 48 kHz Stereo')">
                                                <svg class="icon" viewBox="0 0 24 24"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                                <span>Play</span>
                                            </button>
                                            <button class="btn-action-sm btn-action-del" onclick="deleteRecord(this, 'downlink_pc_to_maix.wav')" title="Delete recording">
                                                <svg class="icon" viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg>
                                            </button>
                                        </div>
                                    </td>
                                </tr>
                                <tr>
                                    <td>
                                        <div style="font-weight:600;">sample_30s_live_mic.wav</div>
                                        <span class="record-tag tag-downlink">Uplink Mic</span>
                                    </td>
                                    <td class="tabular">30.00s · 48k Mono</td>
                                    <td>
                                        <div class="table-actions">
                                            <button class="btn-action-sm" onclick="playRecord('sample_30s_live_mic.wav', 'Uplink Field Mic (MaixCAM ADC)', '30.00s · 48 kHz Mono')">
                                                <svg class="icon" viewBox="0 0 24 24"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                                <span>Play</span>
                                            </button>
                                            <button class="btn-action-sm btn-action-del" onclick="deleteRecord(this, 'sample_30s_live_mic.wav')" title="Delete recording">
                                                <svg class="icon" viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg>
                                            </button>
                                        </div>
                                    </td>
                                </tr>
                                <tr>
                                    <td>
                                        <div style="font-weight:600;">sample1_harmonic_tone.wav</div>
                                        <span class="record-tag tag-test">Dual Tone 440+880Hz</span>
                                    </td>
                                    <td class="tabular">4.96s · SNR 5.6 dB</td>
                                    <td>
                                        <div class="table-actions">
                                            <button class="btn-action-sm" onclick="playRecord('sample1_harmonic_tone_and_mic.wav', 'Harmonic Acoustic Test', '4.96s · 48 kHz Mono')">
                                                <svg class="icon" viewBox="0 0 24 24"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                                <span>Play</span>
                                            </button>
                                            <button class="btn-action-sm btn-action-del" onclick="deleteRecord(this, 'sample1_harmonic_tone.wav')" title="Delete recording">
                                                <svg class="icon" viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg>
                                            </button>
                                        </div>
                                    </td>
                                </tr>
                                <tr>
                                    <td>
                                        <div style="font-weight:600;">sample2_chirp_sweep.wav</div>
                                        <span class="record-tag tag-test">Sweep 100Hz-8kHz</span>
                                    </td>
                                    <td class="tabular">5.00s · SNR 3.8 dB</td>
                                    <td>
                                        <div class="table-actions">
                                            <button class="btn-action-sm" onclick="playRecord('sample2_chirp_sweep_and_mic.wav', 'Frequency Chirp Sweep', '5.00s · 48 kHz Mono')">
                                                <svg class="icon" viewBox="0 0 24 24"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                                                <span>Play</span>
                                            </button>
                                            <button class="btn-action-sm btn-action-del" onclick="deleteRecord(this, 'sample2_chirp_sweep.wav')" title="Delete recording">
                                                <svg class="icon" viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg>
                                            </button>
                                        </div>
                                    </td>
                                </tr>
                            </tbody>
                        </table>
                    </div>

                    <div class="vault-player-deck">
                        <div class="player-info">
                            <span id="now-playing-title" class="player-title">Select a record to play</span>
                            <span id="now-playing-meta" class="player-meta tabular">Ready</span>
                        </div>
                        <audio id="vault-audio" controls preload="none"></audio>
                    </div>
                </div>

                <div id="tab-hw-content" style="display:none; flex-direction:column; flex:1; min-height:0;">
                    <div class="spec-list">
                        <div class="spec-row">
                            <span class="spec-name">SoC Platform</span>
                            <span class="spec-val tabular">Sophgo CV182x · RISC-V 64-bit musl</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Linux Kernel</span>
                            <span class="spec-val tabular">5.10.4-tag- /dev/snd/pcmC0D0c</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Video Capture</span>
                            <span class="spec-val tabular">GalaxyCore GC4653 CMOS · MMF VPSS</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Audio Uplink</span>
                            <span class="spec-val tabular">Card 0 cv182xaadc (ADC) · 48 kHz Mono</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Audio Downlink</span>
                            <span class="spec-val tabular">Card 1 cv182xadac / BlueALSA A2DP</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Bluetooth Stack</span>
                            <span class="spec-val tabular">BlueZ 5.68 (PID 518) · BlueALSA 4.1.1</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Target Headset</span>
                            <span class="spec-val tabular">EDIFIER X2 Plus (6C:16:29:7B:00:D4)</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">A2DP Endpoint</span>
                            <span class="spec-val tabular">/org/bluealsa/hci0/.../a2dpsrc/sink</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Rotation Mode</span>
                            <span id="spec-rotation" class="spec-val tabular">0 deg (UI Gated)</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Signaling WS</span>
                            <span class="spec-val tabular">:8001 / WebSocket Protocol</span>
                        </div>
                        <div class="spec-row">
                            <span class="spec-name">Web Server</span>
                            <span class="spec-val tabular">:8000 / Native cpp-httplib</span>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </main>

    <footer class="app-footer">
        <div>MaixCAM Native WebRTC + Bluetooth Integration (G4c4 M2)</div>
        <div class="tabular">HTTP 8000 · WS 8001 · STUN 19302</div>
    </footer>

    <audio id="remote-audio" autoplay></audio>

    <script src="client.js"></script>
    <script>
        function updateClock() {
            const now = new Date();
            const h = String(now.getUTCHours()).padStart(2, '0');
            const m = String(now.getUTCMinutes()).padStart(2, '0');
            const s = String(now.getUTCSeconds()).padStart(2, '0');
            document.getElementById('clock').textContent = `${h}:${m}:${s} UTC`;
        }
        setInterval(updateClock, 1000);
        updateClock();

        // Canonical default camera mounting orientation (270 deg CW = 90 deg CCW)
        const BASE_ROTATION = 270;
        let canvasPainter = null;
        let currentRotation = 0;

        try {
            const saved = localStorage.getItem('maixcam_operator_rotation');
            if (saved !== null) {
                const parsed = parseInt(saved, 10);
                if ([0, 90, 180, 270].includes(parsed)) {
                    currentRotation = parsed;
                }
            }
        } catch (e) {}

        function getEffectiveRotation() {
            return (BASE_ROTATION + currentRotation) % 360;
        }

        function startCanvasPainter(video, canvas, initialRotation) {
            const ctx = canvas.getContext('2d', { alpha: false });
            let stopped = false;
            let animId = 0;
            let rot = initialRotation;

            const draw = () => {
                if (stopped || video.readyState < 2 || !video.videoWidth || !video.videoHeight) return;
                const isPortrait = rot % 180 !== 0;
                const cw = isPortrait ? 480 : 640;
                const ch = isPortrait ? 640 : 480;
                if (canvas.width !== cw || canvas.height !== ch) {
                    canvas.width = cw;
                    canvas.height = ch;
                }
                ctx.save();
                ctx.translate(cw / 2, ch / 2);
                ctx.rotate((rot + BASE_ROTATION) * Math.PI / 180);
                // Scale factor 640 / 480 = 4/3 crops raw 640x480 top/bottom symmetrically to fill 640x480 baseline
                const scale = 640 / 480;
                ctx.scale(scale, scale);
                ctx.drawImage(video, -video.videoWidth / 2, -video.videoHeight / 2);
                ctx.restore();
            };

            const loop = () => {
                if (stopped) return;
                draw();
                if (video.readyState >= 2 && typeof video.requestVideoFrameCallback === 'function') {
                    animId = video.requestVideoFrameCallback(loop);
                } else {
                    animId = requestAnimationFrame(loop);
                }
            };
            loop();

            return {
                setRotation: (r) => { rot = r; draw(); },
                stop: () => {
                    stopped = true;
                    if (typeof video.cancelVideoFrameCallback === 'function') {
                        video.cancelVideoFrameCallback(animId);
                    } else {
                        cancelAnimationFrame(animId);
                    }
                }
            };
        }

        function applyRotation() {
            const video = document.getElementById('video');
            const canvas = document.getElementById('video-canvas');
            if (!video || !canvas) return;

            video.style.display = 'none';
            canvas.style.display = 'block';

            if (!canvasPainter) {
                canvasPainter = startCanvasPainter(video, canvas, currentRotation);
            } else {
                canvasPainter.setRotation(currentRotation);
            }

            const rotBadge = document.getElementById('rotation-badge');
            if (rotBadge) {
                rotBadge.innerHTML = `<span>Rotation: ${currentRotation} deg</span>`;
            }
            const specRot = document.getElementById('spec-rotation');
            if (specRot) {
                specRot.textContent = `${currentRotation} deg (UI Gated)`;
            }

            try {
                localStorage.setItem('maixcam_operator_rotation', currentRotation.toString());
            } catch (e) {}
        }

        window.applyRotation = applyRotation;

        function toggleRotationUI() {
            currentRotation = (currentRotation + 90) % 360;
            applyRotation();
        }

        // Initialize rotation state on load
        applyRotation();

        function switchTab(tab) {
            const recordsBtn = document.getElementById('tab-records-btn');
            const hwBtn = document.getElementById('tab-hw-btn');
            const recordsContent = document.getElementById('tab-records-content');
            const hwContent = document.getElementById('tab-hw-content');

            if (tab === 'records') {
                recordsBtn.classList.add('active');
                hwBtn.classList.remove('active');
                recordsContent.style.display = 'flex';
                hwContent.style.display = 'none';
            } else {
                hwBtn.classList.add('active');
                recordsBtn.classList.remove('active');
                hwContent.style.display = 'flex';
                recordsContent.style.display = 'none';
            }
        }

        function playRecord(filename, title, meta) {
            const player = document.getElementById('vault-audio');
            const titleElem = document.getElementById('now-playing-title');
            const metaElem = document.getElementById('now-playing-meta');

            titleElem.textContent = title;
            metaElem.textContent = meta;
            player.src = `/recordings/${filename}`;
            player.play().catch(() => {
                synthesizeDemoAudio(title);
            });
        }

        function deleteRecord(btn, filename) {
            const row = btn.closest('tr');
            if (row) {
                row.remove();
            }
            const player = document.getElementById('vault-audio');
            if (player.src && player.src.includes(filename)) {
                player.pause();
                player.src = '';
                document.getElementById('now-playing-title').textContent = 'Select a record to play';
                document.getElementById('now-playing-meta').textContent = 'Ready';
            }
        }

        function synthesizeDemoAudio(label) {
            const ctx = new (window.AudioContext || window.webkitAudioContext)();
            const osc = ctx.createOscillator();
            const gain = ctx.createGain();
            osc.type = 'sine';
            osc.frequency.setValueAtTime(440, ctx.currentTime);
            gain.gain.setValueAtTime(0.2, ctx.currentTime);
            gain.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 1.5);
            osc.connect(gain);
            gain.connect(ctx.destination);
            osc.start();
            osc.stop(ctx.currentTime + 1.5);
            const metaElem = document.getElementById('now-playing-meta');
            metaElem.textContent = 'Playing synthesized preview tone';
        }

        let mediaRecorder = null;
        let recordedChunks = [];
        let recordTimerInterval = null;
        let recordSeconds = 0;
        let currentRecordedFormat = 'mp4';

        function toggleSessionRecording() {
            if (mediaRecorder && mediaRecorder.state === 'recording') {
                stopSessionRecording();
            } else {
                startSessionRecording();
            }
        }

        function startSessionRecording() {
            const videoElem = document.getElementById('video');
            if (!videoElem.srcObject) {
                alert('Stream not available yet for recording.');
                return;
            }

            const formatSelect = document.getElementById('rec-format-select');
            const chosenFormat = formatSelect ? formatSelect.value : 'mp4';
            recordedChunks = [];

            let mimeType = '';
            let ext = 'mp4';

            if (chosenFormat === 'mp4') {
                const mp4Candidates = [
                    'video/mp4;codecs=avc1,mp4a.40.2',
                    'video/mp4;codecs=avc1,opus',
                    'video/mp4;codecs=avc1',
                    'video/mp4'
                ];
                for (const candidate of mp4Candidates) {
                    if (MediaRecorder.isTypeSupported(candidate)) {
                        mimeType = candidate;
                        ext = 'mp4';
                        break;
                    }
                }
                if (!mimeType) {
                    console.warn('[Recorder] MP4 not supported natively by this browser. Falling back to WebM.');
                    ext = 'webm';
                    mimeType = MediaRecorder.isTypeSupported('video/webm;codecs=h264,opus') ? 'video/webm;codecs=h264,opus' : 'video/webm';
                }
            } else if (chosenFormat === 'wav') {
                ext = 'webm';
                mimeType = MediaRecorder.isTypeSupported('audio/webm;codecs=opus') ? 'audio/webm;codecs=opus' : 'audio/webm';
            } else {
                ext = 'webm';
                mimeType = MediaRecorder.isTypeSupported('video/webm;codecs=h264,opus') ? 'video/webm;codecs=h264,opus' : 'video/webm';
            }

            currentRecordedFormat = ext;

            try {
                let streamToRecord = videoElem.srcObject;
                if (chosenFormat === 'wav') {
                    const audioTracks = streamToRecord.getAudioTracks();
                    if (audioTracks.length > 0) {
                        streamToRecord = new MediaStream(audioTracks);
                    }
                }
                mediaRecorder = mimeType ? new MediaRecorder(streamToRecord, { mimeType }) : new MediaRecorder(streamToRecord);
            } catch (err) {
                try {
                    mediaRecorder = new MediaRecorder(videoElem.srcObject);
                    currentRecordedFormat = 'webm';
                } catch (e2) {
                    alert('MediaRecorder error: ' + e2.message);
                    return;
                }
            }

            mediaRecorder.ondataavailable = (e) => {
                if (e.data.size > 0) recordedChunks.push(e.data);
            };

            mediaRecorder.onstop = () => {
                const actualMime = mediaRecorder.mimeType || (currentRecordedFormat === 'mp4' ? 'video/mp4' : 'video/webm');
                const blob = new Blob(recordedChunks, { type: actualMime });
                const url = URL.createObjectURL(blob);
                const sizeMB = (blob.size / (1024 * 1024)).toFixed(2);
                const timestamp = new Date().toISOString().substring(11, 19).replace(/:/g, '');
                const filename = `session_${timestamp}.${currentRecordedFormat}`;
                const tagLabel = currentRecordedFormat === 'mp4' ? 'MP4 Video' : (chosenFormat === 'wav' ? 'Audio Stream' : 'WebM Video');

                const tbody = document.getElementById('records-table-body');
                const row = document.createElement('tr');
                row.innerHTML = `
                    <td>
                        <div style="font-weight:600;">${filename}</div>
                        <span class="record-tag tag-video">${tagLabel}</span>
                    </td>
                    <td class="tabular">${recordSeconds}s · ${currentRecordedFormat.toUpperCase()}</td>
                    <td>
                        <div class="table-actions">
                            <a href="${url}" download="${filename}" class="btn-action-sm" title="Download ${currentRecordedFormat.toUpperCase()} file">
                                <svg class="icon" viewBox="0 0 24 24"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" x2="12" y1="15" y2="3"/></svg>
                                <span>${currentRecordedFormat.toUpperCase()}</span>
                            </a>
                            <button class="btn-action-sm btn-action-del" onclick="deleteRecord(this, '${filename}')" title="Delete recording">
                                <svg class="icon" viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg>
                            </button>
                        </div>
                    </td>
                `;
                tbody.prepend(row);
                playRecord(filename, `Recorded: ${filename}`, `${recordSeconds}s · ${sizeMB} MB (${currentRecordedFormat.toUpperCase()})`);
                document.getElementById('vault-audio').src = url;
                document.getElementById('vault-audio').play().catch(() => {});
            };

            mediaRecorder.start(1000);
            recordSeconds = 0;
            document.getElementById('rec-dot').classList.add('active');
            document.getElementById('rec-timer').style.display = 'inline';
            document.getElementById('rec-timer').textContent = '00:00:00';
            document.getElementById('record-btn').classList.add('btn-record-active');

            recordTimerInterval = setInterval(() => {
                recordSeconds++;
                const hrs = String(Math.floor(recordSeconds / 3600)).padStart(2, '0');
                const mins = String(Math.floor((recordSeconds % 3600) / 60)).padStart(2, '0');
                const secs = String(recordSeconds % 60).padStart(2, '0');
                document.getElementById('rec-timer').textContent = `${hrs}:${mins}:${secs}`;
            }, 1000);
        }

        function stopSessionRecording() {
            if (mediaRecorder) {
                mediaRecorder.stop();
            }
            clearInterval(recordTimerInterval);
            document.getElementById('rec-dot').classList.remove('active');
            document.getElementById('rec-timer').style.display = 'none';
            document.getElementById('record-btn').classList.remove('btn-record-active');
        }

        function captureSnapshot() {
            const canvasElem = document.getElementById('video-canvas');
            if (!canvasElem || !canvasElem.width || !canvasElem.height) {
                alert('No active video frame available.');
                return;
            }

            const canvas = document.createElement('canvas');
            canvas.width = canvasElem.width;
            canvas.height = canvasElem.height;
            const ctx = canvas.getContext('2d');
            ctx.drawImage(canvasElem, 0, 0);

            const a = document.createElement('a');
            const stamp = new Date().toISOString().replace(/[:.]/g, '-');
            a.href = canvas.toDataURL('image/png');
            a.download = `snapshot_${stamp}.png`;
            a.click();
        }
    </script>
</body>
</html>

)HTML";

    static constexpr const char WEBRTC_CLIENT_JS[] = R"JS(
const clientId = randomId(10);
    const signalToken = '__SIGNAL_TOKEN__';
    const wsUrl = (location.protocol === 'https:' ? 'wss://' : 'ws://') + window.location.hostname + ':8001/' + (signalToken ? signalToken + '/' : '') + clientId;
    console.log('[WebRTC] Connecting to:', wsUrl);
    const websocket = new WebSocket(wsUrl);

    let lastSignalingReply = Date.now();
    const heartbeat = setInterval(() => {
        if (websocket.readyState !== WebSocket.OPEN) return;
        if (Date.now() - lastSignalingReply > 20000) {
            websocket.close();
            return;
        }
        websocket.send(JSON.stringify({ type: 'ping' }));
    }, 5000);
    let pc = null;
    let dc = null;
    let localStream = null;
    let localMicTrack = null;
    let silentAudioTrack = null;
    let audioTransceiver = null;
    let isListening = false;
    let isTalking = false;

    const listenBtn = document.getElementById('listen-btn');
    const talkBtn = document.getElementById('talk-btn');
    const connBadge = document.getElementById('conn-status-badge');
    const connText = document.getElementById('conn-text');
    const remoteAudio = document.getElementById('remote-audio');
    const fullscreenBtn = document.getElementById('fullscreen-btn');

    const MIC_MUTED_SVG = '<svg class="icon" viewBox="0 0 24 24"><path d="m18.54 18.54a10.8 10.8 0 0 1-6.54 2.46c-5.52 0-10-4.48-10-10 0-2.45.88-4.7 2.34-6.46"/><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><line x1="1" x2="23" y1="1" y2="23"/></svg>';
    const MIC_LIVE_SVG = '<svg class="icon" viewBox="0 0 24 24"><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2"/><line x1="12" x2="12" y1="19" y2="22"/></svg>';

    const LISTEN_MUTED_SVG = '<svg class="icon" viewBox="0 0 24 24"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/></svg>';
    const LISTEN_LIVE_SVG = '<svg class="icon" viewBox="0 0 24 24"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"/></svg>';

    function getSilentTrack() {
        if (!silentAudioTrack) {
            try {
                const ctx = new (window.AudioContext || window.webkitAudioContext)();
                const dest = ctx.createMediaStreamDestination();
                silentAudioTrack = dest.stream.getAudioTracks()[0];
                if (silentAudioTrack) {
                    silentAudioTrack.enabled = true;
                }
            } catch (e) {
                console.warn('[Audio] Could not create silent track:', e);
            }
        }
        return silentAudioTrack;
    }

    websocket.onopen = () => {
        console.log('[WebRTC] WebSocket connected, requesting offer...');
        if (connBadge && connText) {
            connBadge.className = 'status-pill connecting';
            connText.textContent = 'Signaling Connected';
        }
        sendRequest();
    };

    websocket.onclose = () => {
        console.warn('[WebRTC] WebSocket closed');
        clearInterval(heartbeat);
        stopTalk();
        if (localStream) localStream.getTracks().forEach(track => track.stop());
        if (pc) pc.close();
        setTimeout(() => location.reload(), 4000 + Math.random() * 2000);
        if (connBadge && connText) {
            connBadge.className = 'status-pill disconnected';
            connText.textContent = 'Signaling Closed';
        }
    };

    websocket.onerror = (err) => {
        console.error('[WebRTC] WebSocket error:', err);
        if (connBadge && connText) {
            connBadge.className = 'status-pill disconnected';
            connText.textContent = 'Signaling Error';
        }
    };

    websocket.onmessage = async (evt) => {
        if (typeof evt.data !== 'string') return;
        const message = JSON.parse(evt.data);
        lastSignalingReply = Date.now();
        if (message.type === 'busy' && connText) connText.textContent = 'Device in use';
        if (message.type === 'offer') {
            await handleOffer(message);
        }
    };

    function createPeerConnection() {
        const config = {
            bundlePolicy: 'max-bundle',
            iceServers: [],
        };

        const peer = new RTCPeerConnection(config);

        peer.onconnectionstatechange = () => {
            console.log('[WebRTC] PeerConnection state:', peer.connectionState);
            if (peer.connectionState === 'failed') websocket.close();
            if (connBadge && connText) {
                if (peer.connectionState === 'connected') {
                    connBadge.className = 'status-pill connected';
                    connText.textContent = 'WebRTC Live';
                } else if (peer.connectionState === 'connecting') {
                    connBadge.className = 'status-pill connecting';
                    connText.textContent = 'Connecting...';
                } else if (peer.connectionState === 'failed' || peer.connectionState === 'disconnected') {
                    connBadge.className = 'status-pill disconnected';
                    connText.textContent = 'Offline';
                }
            }
        };

        peer.ontrack = (evt) => {
            if (evt.track.kind === 'video') {
                const video = document.getElementById('video');
                video.srcObject = evt.streams[0] || new MediaStream([evt.track]);
                video.play().catch(e => console.warn('[Video] Auto-play blocked:', e));

                video.addEventListener('loadedmetadata', () => {
                    const res = document.getElementById('res-badge');
                    if (res) res.textContent = video.videoWidth + ' × ' + video.videoHeight + ' @ 30fps';
                    if (typeof window.applyRotation === 'function') {
                        window.applyRotation();
                    }
                });
            } else if (evt.track.kind === 'audio') {
                if (remoteAudio) {
                    remoteAudio.srcObject = evt.streams[0] || new MediaStream([evt.track]);
                    remoteAudio.muted = !isListening;
                }
            }
        };

        peer.ondatachannel = (evt) => {
            dc = evt.channel;
            dc.onopen = () => console.log('[DataChannel] Opened');
            dc.onclose = () => console.log('[DataChannel] Closed');
            dc.onmessage = (e) => console.log('[DataChannel] Message:', e.data);
        };

        return peer;
    }

    async function waitGatheringComplete() {
        return new Promise((resolve) => {
            if (pc.iceGatheringState === 'complete') resolve();
            else {
                const check = () => {
                    if (pc.iceGatheringState === 'complete') {
                        pc.removeEventListener('icegatheringstatechange', check);
                        resolve();
                    }
                };
                pc.addEventListener('icegatheringstatechange', check);
                setTimeout(resolve, 2000);
            }
        });
    }

    async function sendAnswer(peer) {
        await peer.setLocalDescription(await peer.createAnswer());
        await waitGatheringComplete();

        const answer = peer.localDescription;
        websocket.send(JSON.stringify({
            id: 'server',
            type: answer.type,
            sdp: answer.sdp,
        }));
    }

    async function handleOffer(offer) {
        pc = createPeerConnection();
        await pc.setRemoteDescription(offer);

        audioTransceiver = pc.getTransceivers().find(
            t => t.receiver && t.receiver.track && t.receiver.track.kind === 'audio'
        );

        if (audioTransceiver) {
            audioTransceiver.direction = 'sendrecv';
            const trackToSend = localMicTrack || getSilentTrack();
            if (trackToSend && audioTransceiver.sender) {
                try {
                    await audioTransceiver.sender.replaceTrack(trackToSend);
                } catch (e) {
                    console.warn('[WebRTC] Initial track replace warning:', e);
                }
            }
        }

        await sendAnswer(pc);
    }

    function sendRequest() {
        websocket.send(JSON.stringify({ id: 'server', type: 'request' }));
    }

    function randomId(length) {
        const chars = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
        return [...Array(length)].map(() => chars.charAt(Math.floor(Math.random() * chars.length))).join('');
    }

    // --- CALL-APP LISTEN BUTTON (SPEAKER TOGGLE) ---
    if (listenBtn) {
        listenBtn.onclick = async () => {
            isListening = !isListening;
            if (remoteAudio) {
                remoteAudio.muted = !isListening;
                if (isListening) {
                    try {
                        await remoteAudio.play();
                        listenBtn.className = 'call-btn btn-listen-active';
                        listenBtn.innerHTML = LISTEN_LIVE_SVG;
                        listenBtn.title = 'Mute Field Audio';
                    } catch (e) {
                        console.warn('[Audio] Listen playback blocked by browser:', e);
                        isListening = false;
                        remoteAudio.muted = true;
                        listenBtn.className = 'call-btn btn-listen-muted';
                        listenBtn.innerHTML = LISTEN_MUTED_SVG;
                        listenBtn.title = 'Listen to Field Audio';
                    }
                } else {
                    listenBtn.className = 'call-btn btn-listen-muted';
                    listenBtn.innerHTML = LISTEN_MUTED_SVG;
                    listenBtn.title = 'Listen to Field Audio';
                }
            }
        };
    }

    // --- CALL-APP TALK BUTTON (SEAMLESS MIC TOGGLE - NO ARMED STEP) ---
    async function toggleTalk() {
        if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia) {
            alert('Microphone requires HTTPS with the MaixCAM certificate authority trusted on this device.');
            return;
        }
        if (!localStream || !localMicTrack) {
            try {
                localStream = await navigator.mediaDevices.getUserMedia({
                    audio: {
                        echoCancellation: true,
                        noiseSuppression: true,
                        autoGainControl: true
                    },
                    video: false
                });

                localMicTrack = localStream.getAudioTracks()[0];
                localMicTrack.enabled = true;

                if (audioTransceiver && audioTransceiver.sender) {
                    await audioTransceiver.sender.replaceTrack(localMicTrack);
                }

                startTalk();
            } catch (err) {
                console.error('[Audio] Microphone permission error:', err);
                alert('Microphone permission required to talk: ' + err.message);
            }
            return;
        }

        if (isTalking) {
            stopTalk();
        } else {
            startTalk();
        }
    }

    function notifyTalkState(active) {
        // 1. WebRTC DataChannel message
        if (dc && dc.readyState === 'open') {
            try {
                dc.send(JSON.stringify({ type: active ? 'talk-start' : 'talk-stop' }));
            } catch (e) {
                console.warn('[DataChannel] Talk send failed:', e);
            }
        }
        // 2. HTTP GET notification (verified 100% reliable 200 OK)
        fetch('/api/talk?active=' + (active ? '1' : '0')).catch(() => {});
        // 3. HTTP POST fallback with json body
        fetch('/api/talk', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ active: active })
        }).catch(() => {});
    }

    async function startTalk() {
        if (isTalking) return;
        isTalking = true;

        if (localMicTrack) {
            localMicTrack.enabled = true;
            if (audioTransceiver && audioTransceiver.sender) {
                try {
                    await audioTransceiver.sender.replaceTrack(localMicTrack);
                } catch (e) {
                    console.warn('[Audio] replaceTrack to mic error:', e);
                }
            }
        }

        if (talkBtn) {
            talkBtn.className = 'call-btn btn-mic-live';
            talkBtn.innerHTML = MIC_LIVE_SVG;
            talkBtn.title = 'Mute Mic (Transmitting to Headset)';
        }

        notifyTalkState(true);
    }

    async function stopTalk() {
        if (!isTalking) return;
        isTalking = false;

        if (localMicTrack) {
            localMicTrack.enabled = false;
        }

        const silent = getSilentTrack();
        if (silent && audioTransceiver && audioTransceiver.sender) {
            try {
                await audioTransceiver.sender.replaceTrack(silent);
            } catch (e) {
                console.warn('[Audio] replaceTrack to silent error:', e);
            }
        }

        if (talkBtn) {
            talkBtn.className = 'call-btn btn-mic-muted';
            talkBtn.innerHTML = MIC_MUTED_SVG;
            talkBtn.title = 'Unmute Mic (Talk to Headset)';
        }

        notifyTalkState(false);
    }

    if (talkBtn) {
        talkBtn.onclick = toggleTalk;
    }

    // Safety release: ONLY when tab is truly hidden / switched
    document.addEventListener('visibilitychange', () => {
        if (document.hidden && isTalking) {
            stopTalk();
        }
    });

    window.addEventListener('pagehide', () => {
        stopTalk();
        websocket.close();
    });

    // --- TELEMETRY POLLING ---
    async function pollTelemetry() {
        try {
            const res = await fetch('/api/audio_stats');
            if (!res.ok) return;
            const data = await res.json();

            if (data.uplink) {
                const upPkts = (data.uplink.packets_sent || 0).toLocaleString();
                const upKB = ((data.uplink.bytes_sent || 0) / 1024).toFixed(1);
                const el = document.getElementById('tel-uplink');
                if (el) el.textContent = upPkts + ' pkts (' + upKB + ' KB)';
            }

            if (data.downlink) {
                const rxPkts = (data.downlink.packets_received || 0).toLocaleString();
                const decPkts = (data.downlink.packets_decoded || 0).toLocaleString();
                const el = document.getElementById('tel-downlink');
                if (el) el.textContent = decPkts + ' / ' + rxPkts + ' pkts';

                if (data.downlink.playback) {
                    const frames = (data.downlink.playback.frames_written || 0).toLocaleString();
                    const underruns = data.downlink.playback.underruns || 0;
                    const elPlay = document.getElementById('tel-playback');
                    const elUnder = document.getElementById('tel-underruns');
                    if (elPlay) elPlay.textContent = frames + ' frames';
                    if (elUnder) elUnder.textContent = underruns + ' underruns';
                }
            }
        } catch (e) {
            // Server temporarily unreachable or rebooting
        }
    }
    setInterval(pollTelemetry, 1500);

    // Fullscreen toggle
    if (fullscreenBtn) {
        fullscreenBtn.onclick = () => {
            const container = document.querySelector('.video-container');
            if (!document.fullscreenElement) {
                container?.requestFullscreen?.() || container?.webkitRequestFullscreen?.();
            } else {
                document.exitFullscreen?.() || document.webkitExitFullscreen?.();
            }
        };
    }

)JS";

public:
    MaixWebRTCHttpServer(const std::string &host = "0.0.0.0", uint16_t port = 8080,
                         const std::string &stun_server = "stun:stun.l.google.com:19302",
                         uint16_t ws_port = 8001)
        : _host(host), _port(port), _stun_server(stun_server), _ws_port(ws_port), _is_running(false)
    {
        const char *cert = std::getenv("MAIX_WEBRTC_TLS_CERT");
        const char *key = std::getenv("MAIX_WEBRTC_TLS_KEY");
        if (cert || key) {
            if (!cert || !key) throw std::runtime_error("TLS requires both certificate and key");
            auto tls = std::make_unique<httplib::SSLServer>(cert, key);
            if (!tls->is_valid()) throw std::runtime_error("Invalid TLS certificate or key");
            _server = std::move(tls);
        } else {
            _server = std::make_unique<httplib::Server>();
        }
        _server->set_read_timeout(10);
        _server->set_write_timeout(10);
        _server->set_payload_max_length(65536);
        _server->set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
            if (req.has_header("Origin")) {
                const std::string scheme = std::getenv("MAIX_WEBRTC_TLS_CERT") ? "https://" : "http://";
                if (req.get_header_value("Origin") != scheme + req.get_header_value("Host")) {
                    res.status = 403;
                    res.set_content("Cross-origin request denied", "text/plain");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        _server->set_default_headers({{"Cache-Control", "no-store"},
            {"X-Content-Type-Options", "nosniff"},
            {"Permissions-Policy", "microphone=(self), camera=()"}});
    }

    ~MaixWebRTCHttpServer() {
        stop();
    }

    bool start() {
        if (_is_running) {
            return true;
        }

        _server->Get("/", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(WEBRTC_INDEX_HTML, "text/html; charset=utf-8");
        });

        _server->Get("/index.html", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(WEBRTC_INDEX_HTML, "text/html; charset=utf-8");
        });

        _server->Get("/client.js", [this](const httplib::Request& req, httplib::Response& res) {
            std::string js_content = std::string(WEBRTC_CLIENT_JS);
            const std::string old_port = ":8001/";
            auto pos = js_content.find(old_port);
            if (pos != std::string::npos)
                js_content.replace(pos, old_port.size(), ":" + std::to_string(_ws_port) + "/");

            const char *token = std::getenv("MAIX_WEBRTC_SIGNAL_TOKEN");
            const std::string marker = "__SIGNAL_TOKEN__";
            pos = js_content.find(marker);
            if (pos != std::string::npos) js_content.replace(pos, marker.size(), token ? token : "");
            res.set_content(js_content, "application/javascript; charset=utf-8");
        });

        _server->Get("/api/audio_stats", [this](const httplib::Request& req, httplib::Response& res) {
            if (_metrics_callback) {
                res.set_content(_metrics_callback(), "application/json; charset=utf-8");
            } else {
                res.set_content("{\"status\":\"no_metrics_provider\"}", "application/json; charset=utf-8");
            }
        });

        _server->Get("/api/talk", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.has_param("active")) {
                bool act = (req.get_param_value("active") == "1" || req.get_param_value("active") == "true");
                if (_talk_callback) _talk_callback(act);
            }
            res.set_content("{\"status\":\"ok\"}", "application/json; charset=utf-8");
        });

        _server->Post("/api/talk", [this](const httplib::Request& req, httplib::Response& res) {
            bool act = false;
            if (req.has_param("active")) {
                act = (req.get_param_value("active") == "1" || req.get_param_value("active") == "true");
            } else if (!req.body.empty()) {
                if (req.body.find("\"active\":false") != std::string::npos ||
                    req.body.find("active=0") != std::string::npos ||
                    req.body.find("false") != std::string::npos ||
                    req.body == "0") {
                    act = false;
                } else {
                    act = true;
                }
            }
            if (_talk_callback) _talk_callback(act);
            res.set_content(act ? "{\"talk\":true}" : "{\"talk\":false}", "application/json; charset=utf-8");
        });

        _server->Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
            if (_metrics_callback) {
                res.set_content(_metrics_callback(), "application/json; charset=utf-8");
            } else {
                res.set_content("{\"status\":\"no_metrics_provider\"}", "application/json; charset=utf-8");
            }
        });

        _is_running = true;
        _server_thread = std::make_unique<std::thread>([this]() {
            MAIX_WEBRTC_LOG("[HttpServer] Starting on http://%s:%d\n", _host.c_str(), _port);
            MAIX_WEBRTC_LOG("[HttpServer] Serving embedded WebRTC client interface\n");

            if (!_server->listen(_host.c_str(), _port)) {
                MAIX_WEBRTC_LOG("[HttpServer] Failed to bind to %s:%d\n", _host.c_str(), _port);
                _is_running = false;
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return _is_running;
    }

    void stop() {
        if (_is_running) {
            _is_running = false;

            if (_server) {
                _server->stop();
            }

            if (_server_thread && _server_thread->joinable()) {
                _server_thread->join();
            }

            MAIX_WEBRTC_LOG("[HttpServer] Stopped\n");
        }
    }

    bool is_running() const {
        return _is_running;
    }

    uint16_t get_port() const {
        return _port;
    }

    std::string get_url() const {
        return "http://" + (_host == "0.0.0.0" ? "localhost" : _host) + ":" + std::to_string(_port);
    }

    void set_metrics_callback(std::function<std::string()> cb) {
        _metrics_callback = std::move(cb);
    }

    void set_talk_callback(std::function<void(bool)> cb) {
        _talk_callback = std::move(cb);
    }
};
