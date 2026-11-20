#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def main():
    html_file = ROOT / 'src/index.html'
    client_js_file = ROOT / 'src/client.js'
    server_header = ROOT / 'src/maix_webrtc_http_server.hpp'

    with open(html_file, 'r', encoding='utf-8') as f:
        html_content = f.read()

    client_js = client_js_file.read_text(encoding='utf-8')

    demo_dir = ROOT / 'demo/rnnoise'
    demo_header = ROOT / 'src/rnnoise_demo_assets.hpp'
    assets = [('RNNOISE_DEMO_HTML', 'index.html'),
              ('RNNOISE_DEMO_PROCESSOR', 'processor.js'),
              ('RNNOISE_DEMO_MODULE', 'rnnoise-sync.js')]
    demo_header.write_text('#pragma once\n' + ''.join(
        f'static constexpr const char {name}[] = R"DEMOASSET({(demo_dir / filename).read_text(encoding="utf-8")})DEMOASSET";\n'
        for name, filename in assets), encoding='utf-8')

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
        + '\n)JS\";\n\n'
        + suffix
    )

    with open(server_header, 'w', encoding='utf-8') as f:
        f.write(new_content)

    print(f'Successfully updated {server_header}')

if __name__ == '__main__':
    main()
