#!/usr/bin/env python3
"""Real Chromium MediaRecorder regression with synthetic media, no camera access.
Requires playwright on the host and an installed Chromium executable.
"""
import argparse
import asyncio
import json
import subprocess
from pathlib import Path
from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[2]

async def run(args):
    output = ROOT / 'agent_progress/standalone/evidence/recording-rotation'
    output.mkdir(parents=True, exist_ok=True)
    html = (ROOT / 'src/index.html').read_text().replace('<script src="client.js"></script>', '')
    async with async_playwright() as playwright:
        browser = await playwright.chromium.launch(executable_path=str(args.chromium), headless=True,
                                                   args=['--autoplay-policy=no-user-gesture-required'])
        page = await browser.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        await page.route('https://maixcam.test/**', lambda route: route.fulfill(
            status=200 if route.request.url == 'https://maixcam.test/' else 404,
            content_type='text/html', body=html if route.request.url == 'https://maixcam.test/' else 'Not found'))
        await page.goto('https://maixcam.test/')
        await page.evaluate('recordingsReady')
        await page.evaluate('''async () => {
            window.testUrls = [];
            const createURL = URL.createObjectURL.bind(URL);
            URL.createObjectURL = blob => { const url = createURL(blob); testUrls.push(url); return url; };
            const source = document.createElement('canvas');
            source.width = 640; source.height = 480;
            const ctx = source.getContext('2d');
            window.sourceTimer = setInterval(() => {
                for (const [x, y, color] of [[0,0,'#ff0000'],[320,0,'#00ff00'],[0,240,'#0000ff'],[320,240,'#ffff00']]) {
                    ctx.fillStyle = color; ctx.fillRect(x,y,320,240);
                }
            }, 33);
            const stream = source.captureStream(30);
            window.testSourceTrack = stream.getVideoTracks()[0];
            window.testAudio = new AudioContext();
            const destination = testAudio.createMediaStreamDestination();
            const tone = testAudio.createOscillator();
            tone.connect(destination); tone.start();
            window.testAudioTrack = destination.stream.getAudioTracks()[0];
            stream.addTrack(testAudioTrack);
            const video = document.getElementById('video');
            video.srcObject = stream;
            await video.play();
            document.getElementById('rec-format-select').value = 'webm';
        }''')
        await page.wait_for_timeout(500)
        report = []
        for angle in [0, 90, 180, 270]:
            await page.evaluate('(angle) => { currentRotation = angle; applyRotation(); }', angle)
            await page.wait_for_timeout(150)
            result = await page.evaluate('''async () => {
                const canvas = document.getElementById('video-canvas');
                const points = [[.25,.25],[.75,.25],[.25,.75],[.75,.75]];
                const pixels = target => {
                    const ctx = target.getContext('2d');
                    return points.map(([x,y]) => Array.from(ctx.getImageData(Math.floor(target.width*x),Math.floor(target.height*y),1,1).data).slice(0,3));
                };
                const expected = pixels(canvas);
                const width = canvas.width, height = canvas.height;
                startSessionRecording();
                if (!mediaRecorder || mediaRecorder.state !== 'recording') throw new Error('Recording did not start');
                window.ownedTrack = recordingCanvasStream.getVideoTracks()[0];
                await new Promise(resolve => setTimeout(resolve, 900));
                const stopped = new Promise(resolve => mediaRecorder.addEventListener('stop', resolve, {once:true}));
                stopSessionRecording(); await stopped;
                const url = testUrls[testUrls.length - 1];
                const playback = document.createElement('video');
                playback.muted = true; playback.src = url;
                await playback.play();
                await new Promise(resolve => playback.requestVideoFrameCallback(resolve));
                const decoded = document.createElement('canvas');
                decoded.width = playback.videoWidth; decoded.height = playback.videoHeight;
                decoded.getContext('2d').drawImage(playback,0,0);
                const result = {width, height, recordedWidth: playback.videoWidth, recordedHeight: playback.videoHeight,
                    expected, actual: pixels(decoded), previewPixels: pixels((() => {
                        const previewCanvas = document.createElement('canvas');
                        const preview = document.getElementById('vault-video');
                        previewCanvas.width = preview.videoWidth; previewCanvas.height = preview.videoHeight;
                        previewCanvas.getContext('2d').drawImage(preview,0,0);
                        return previewCanvas;
                    })()),
                    preview: {tag: document.getElementById('vault-video').tagName,
                              visible: !document.getElementById('vault-video').hidden,
                              width: document.getElementById('vault-video').videoWidth,
                              height: document.getElementById('vault-video').videoHeight,
                              audioHidden: document.getElementById('vault-audio').hidden},
                    sourceVideo: testSourceTrack.readyState,
                    sourceAudio: testAudioTrack.readyState, ownedTrack: ownedTrack.readyState, url};
                playback.pause();
                return result;
            }''')
            assert (result['width'], result['height']) == (result['recordedWidth'], result['recordedHeight']), result
            assert all(abs(a-b) < 35 for expected, actual in zip(result['expected'], result['actual']) for a,b in zip(expected,actual)), result
            assert result['preview'] == {'tag':'VIDEO','visible':True,'width':result['width'],
                                         'height':result['height'],'audioHidden':True}, result
            assert all(abs(a-b)<35 for expected,actual in zip(result['expected'],result['previewPixels']) for a,b in zip(expected,actual)), result
            assert result['sourceVideo'] == result['sourceAudio'] == 'live', result
            assert result['ownedTrack'] == 'ended', result
            content = await page.evaluate('async url => Array.from(new Uint8Array(await (await fetch(url)).arrayBuffer()))', result.pop('url'))
            filename = output / f'rotation-{angle}.webm'
            filename.write_bytes(bytes(content))
            if angle == 90:
                assert await page.locator('#vault-video').is_visible()
                assert await page.evaluate('''() => {
                    const row = document.querySelector('.btn-record-preview').closest('tr');
                    return row.querySelector('.btn-action-del').getBoundingClientRect().right <=
                           document.querySelector('.records-table-wrap').getBoundingClientRect().right;
                }''')
                await page.screenshot(path=str(output / 'preview-video-90.png'))
            result['angle'] = angle
            report.append(result)
        # Mid-record rotation keeps a fixed encoder size and fits the entire view.
        dynamic = await page.evaluate('''async () => {
            currentRotation = 0; applyRotation(); startSessionRecording();
            const width = recordingCanvas.width, height = recordingCanvas.height;
            await new Promise(resolve => setTimeout(resolve, 600));
            currentRotation = 90; applyRotation();
            await new Promise(resolve => setTimeout(resolve, 600));
            const ctx = recordingCanvas.getContext('2d');
            const display = document.getElementById('video-canvas');
            const expected = Array.from(display.getContext('2d').getImageData(display.width*.25, display.height*.25,1,1).data);
            const scale = Math.min(width / display.width, height / display.height);
            const left = (width - display.width * scale)/2;
            const top = (height - display.height * scale)/2;
            const actual = Array.from(ctx.getImageData(left+display.width*.25*scale,top+display.height*.25*scale,1,1).data);
            const margin = Array.from(ctx.getImageData(0,height/2,1,1).data);
            const stopped = new Promise(resolve => mediaRecorder.addEventListener('stop',resolve,{once:true}));
            stopSessionRecording(); await stopped;
            return {expected, actual, margin, width, height,
                    sampleX: Math.floor(left+display.width*.25*scale),
                    sampleY: Math.floor(top+display.height*.25*scale),
                    url: testUrls[testUrls.length - 1], sourceAudio: testAudioTrack.readyState};
        }''')
        dynamic_content = await page.evaluate('async url => Array.from(new Uint8Array(await (await fetch(url)).arrayBuffer()))', dynamic.pop('url'))
        (output / 'rotation-mid-record.webm').write_bytes(bytes(dynamic_content))
        assert dynamic['expected'] == dynamic['actual'], dynamic
        assert dynamic['margin'][:3] == [0,0,0], dynamic
        assert dynamic['sourceAudio'] == 'live', dynamic
        decoded = subprocess.check_output([
            'ffmpeg', '-v', 'error', '-i', str(output / 'rotation-mid-record.webm'),
            '-vf', 'reverse', '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'])
        def pixel(x, y):
            offset = (y * dynamic['width'] + x) * 3
            return list(decoded[offset:offset + 3])
        dynamic['recorded'] = pixel(dynamic['sampleX'], dynamic['sampleY'])
        dynamic['recordedMargin'] = pixel(0, dynamic['height'] // 2)
        assert all(abs(a-b)<35 for a,b in zip(dynamic['expected'][:3],dynamic['recorded'])), dynamic
        assert all(v<20 for v in dynamic['recordedMargin']), dynamic
        # Constructor fallback must use the canvas too, never the raw camera.
        fallback = await page.evaluate('''async () => {
            const NativeRecorder = MediaRecorder;
            window.MediaRecorder = class extends NativeRecorder {
                constructor(stream, options) { if (options) throw new Error('Test unsupported codec'); super(stream); }
            };
            startSessionRecording();
            const rotated = mediaRecorder.stream.getVideoTracks()[0] === recordingCanvasStream.getVideoTracks()[0];
            const stopped = new Promise(resolve => mediaRecorder.addEventListener('stop',resolve,{once:true}));
            stopSessionRecording(); await stopped;
            window.MediaRecorder = NativeRecorder;
            return rotated;
        }''')
        assert fallback
        audio_only = await page.evaluate("""async () => {
            document.getElementById('rec-format-select').value = 'wav';
            startSessionRecording();
            const tracks = {video: mediaRecorder.stream.getVideoTracks().length,
                            audio: mediaRecorder.stream.getAudioTracks().length};
            await new Promise(resolve => setTimeout(resolve,150));
            const stopped = new Promise(resolve => mediaRecorder.addEventListener('stop',resolve,{once:true}));
            stopSessionRecording(); await stopped;
            tracks.sourceAudio = testAudioTrack.readyState;
            return tracks;
        }""")
        assert audio_only == {'video':0,'audio':1,'sourceAudio':'live'}, audio_only
        preview_controls = await page.evaluate("""() => {
            const video = document.getElementById('vault-video');
            const audio = document.getElementById('vault-audio');
            const audioMode = video.hidden && !audio.hidden && audio.src.startsWith('blob:');
            const buttons = document.querySelectorAll('.btn-record-preview');
            buttons[buttons.length - 1].click();
            return {audioMode, previewButtons: buttons.length, replayVideo: !video.hidden && audio.hidden};
        }""")
        assert preview_controls['audioMode'] and preview_controls['replayVideo'], preview_controls
        assert preview_controls['previewButtons'] >= 7, preview_controls
        await page.wait_for_function("document.getElementById('vault-video').readyState >= 2")
        deletion = await page.evaluate("""async () => {
            const filename = playingRecord;
            const url = localRecordings.get(filename).url;
            const last = document.querySelectorAll('.btn-record-preview');
            const row = last[last.length - 1].closest('tr');
            await deleteRecord(row.querySelector('.btn-action-del'), filename);
            let revoked = false;
            try { await fetch(url); } catch { revoked = true; }
            return {revoked, removed: !localRecordings.has(filename),
                    reset: playingRecord === null && document.getElementById('vault-video').hidden};
        }""")
        assert all(deletion.values()), deletion
        await page.evaluate("playRecord('missing-preview.wav', 'Missing test file', 'test')")
        await page.wait_for_function("document.getElementById('now-playing-meta').textContent.includes('Preview unavailable')")
        assert await page.evaluate("typeof synthesizeDemoAudio === 'undefined'")
        assert not errors, errors
        (output / 'result.json').write_text(json.dumps({'static_angles':report,'mid_record_rotation':dynamic,
                                                        'codec_fallback_canvas':fallback,'audio_only':audio_only,'preview_controls':preview_controls,'delete_preview':deletion,'page_errors':errors},indent=2)+'\n')
        print('PASS: decoded recordings match displayed 0/90/180/270 angles; live rotation fits without cropping; audio preserved; codec fallback uses canvas; owned tracks cleaned up; real video/audio preview, replay, deletion and missing-file reporting verified')
        await browser.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--chromium', type=Path, required=True)
    asyncio.run(run(parser.parse_args()))
