#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def main():
    html_file = ROOT / 'src/index.html'
    client_js_file = ROOT / 'src/client.js'
    worklet_js_file = ROOT / 'src/noise-gate-processor.js'
    server_header = ROOT / 'src/maix_webrtc_http_server.hpp'

    with open(html_file, 'r', encoding='utf-8') as f:
        html_content = f.read()

    client_js = client_js_file.read_text(encoding='utf-8')
    worklet_js = worklet_js_file.read_text(encoding='utf-8')

    with open(server_header, 'r', encoding='utf-8') as f:
        orig = f.read()

    prefix = orig[:orig.find('static constexpr const char WEBRTC_INDEX_HTML[] = R"HTML(')]
    public_pos = orig.find('public:\n    MaixWebRTCHttpServer(')
    if public_pos == -1:
        public_pos = orig.find('public:')
    suffix = orig[public_pos:]

    new_content = (
        prefix
        + 'static constexpr const char WEBRTC_INDEX_HTML[] = R"HTML(\n'
        + html_content
        + '\n)HTML\";\n\n    '
        + 'static constexpr const char WEBRTC_CLIENT_JS[] = R"JS(\n'
        + client_js
        + '\n)JS\";\n\n    '
        + 'static constexpr const char WEBRTC_NOISE_GATE_PROCESSOR_JS[] = R"JS(\n'
        + worklet_js
        + '\n)JS\";\n\n'
        + suffix
    )

    with open(server_header, 'w', encoding='utf-8') as f:
        f.write(new_content)

    print(f'Successfully updated {server_header}')

if __name__ == '__main__':
    main()
