#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def main():
    html_file = ROOT / 'src/index.html'
    server_header = ROOT / 'src/maix_webrtc_http_server.hpp'

    with open(html_file, 'r', encoding='utf-8') as f:
        html_content = f.read()

    client_js = (ROOT / 'src/client.js').read_text(encoding='utf-8')

    with open(server_header, 'r', encoding='utf-8') as f:
        orig = f.read()

    prefix = orig[:orig.find('static constexpr const char WEBRTC_INDEX_HTML[] = R\"HTML(')]
    mid_marker = ')HTML\";'
    mid_pos = orig.find(mid_marker)
    js_marker = 'static constexpr const char WEBRTC_CLIENT_JS[] = R\"JS('
    js_pos = orig.find(js_marker, mid_pos)
    end_marker = ')JS\";'
    end_pos = orig.find(end_marker, js_pos)
    suffix = orig[end_pos + len(end_marker):]

    new_content = (
        prefix
        + 'static constexpr const char WEBRTC_INDEX_HTML[] = R\"HTML(\n'
        + html_content
        + '\n)HTML\";\n\n    '
        + 'static constexpr const char WEBRTC_CLIENT_JS[] = R\"JS(\n'
        + client_js
        + '\n)JS\";'
        + suffix
    )

    with open(server_header, 'w', encoding='utf-8') as f:
        f.write(new_content)

    print(f'Successfully updated {server_header}')

if __name__ == '__main__':
    main()
