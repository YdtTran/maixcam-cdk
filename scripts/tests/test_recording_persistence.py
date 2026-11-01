#!/usr/bin/env python3
"""Real browser profile: encoded clips survive reload/restart and retain previews.
No camera/network hardware. All artifacts/profile files stay in persistent evidence.
"""
import argparse
import asyncio
import hashlib
import json
import tempfile
from pathlib import Path
from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[2]
ORIGIN = 'https://maixcam.test/'

async def run(args):
    output = ROOT / 'agent_progress/standalone/evidence/recording-persistence'
    output.mkdir(parents=True, exist_ok=True)
    html = (ROOT / 'src/index.html').read_text().replace('<script src="client.js"></script>', '')
    async with async_playwright() as playwright:
        with tempfile.TemporaryDirectory(dir=output, prefix='profile-') as profile:
            async def launch():
                context = await playwright.chromium.launch_persistent_context(
                    profile, executable_path=str(args.chromium), headless=True, accept_downloads=True,
                    args=['--autoplay-policy=no-user-gesture-required'])
                await context.route(ORIGIN + '**', lambda route: route.fulfill(
                    status=200 if route.request.url == ORIGIN else 404, content_type='text/html',
                    body=html if route.request.url == ORIGIN else 'Not found'))
                page = context.pages[0]
                await page.goto(ORIGIN)
                await page.evaluate('recordingsReady')
                return context, page

            async def source(page):
                await page.evaluate('''async () => {
                    const canvas = document.createElement('canvas'); canvas.width=640; canvas.height=480;
                    const ctx=canvas.getContext('2d');
                    window.testTimer=setInterval(() => {
                        for (const [x,y,c] of [[0,0,'red'],[320,0,'lime'],[0,240,'blue'],[320,240,'yellow']]) {
                            ctx.fillStyle=c; ctx.fillRect(x,y,320,240);
                        }
                    },33);
                    const stream=canvas.captureStream(30);
                    const audio=new AudioContext(); const destination=audio.createMediaStreamDestination();
                    const tone=audio.createOscillator(); tone.connect(destination); tone.start();
                    stream.addTrack(destination.stream.getAudioTracks()[0]);
                    const video=document.getElementById('video'); video.srcObject=stream; await video.play();
                    currentRotation=90; applyRotation();
                }''')
                await page.wait_for_timeout(200)

            async def record(page, audio=False):
                return await page.evaluate('''async audioOnly => {
                    document.getElementById('rec-format-select').value=audioOnly?'wav':'webm';
                    startSessionRecording();
                    if (!mediaRecorder || mediaRecorder.state!=='recording') throw new Error('No recorder');
                    await new Promise(resolve=>setTimeout(resolve,750));
                    const stopped=new Promise(resolve=>mediaRecorder.addEventListener('stop',resolve,{once:true}));
                    stopSessionRecording(); await stopped;
                    const entry=[...localRecordings.values()].at(-1);
                    await entry.savePromise;
                    const digest=await crypto.subtle.digest('SHA-256',await entry.blob.arrayBuffer());
                    return {filename:entry.filename,persisted:entry.persisted,size:entry.blob.size,
                        hash:Array.from(new Uint8Array(digest),v=>v.toString(16).padStart(2,'0')).join(''),url:entry.url};
                }''', audio)

            async def restored(page, saved):
                result = await page.evaluate('''async filename => {
                    await recordingsReady;
                    const entry=localRecordings.get(filename);
                    if (!entry) return null;
                    const digest=await crypto.subtle.digest('SHA-256',await entry.blob.arrayBuffer());
                    return {persisted:entry.persisted,size:entry.blob.size,isVideo:entry.isVideo,
                        hash:Array.from(new Uint8Array(digest),v=>v.toString(16).padStart(2,'0')).join(''),url:entry.url};
                }''', saved['filename'])
                assert result and result['persisted'] and result['size'] == saved['size'] and result['hash'] == saved['hash'], result
                assert result['url'] != saved['url'], 'Reload must recreate Blob URLs from saved bytes'
                return result

            context, page = await launch()
            await source(page)
            video = await record(page)
            assert video['persisted'] and video['size'] > 0
            await page.reload()
            restored_video = await restored(page, video)
            assert restored_video['isVideo']
            await page.locator('.local-recording-row .btn-record-preview').click()
            await page.wait_for_function("document.getElementById('vault-video').readyState>=2")
            assert await page.evaluate("[document.getElementById('vault-video').videoWidth,document.getElementById('vault-video').videoHeight]") == [480,640]
            await page.screenshot(path=str(output / 'preview-after-refresh.png'))
            async with page.expect_download() as downloaded:
                await page.locator('.local-recording-row .record-download').click()
            download = await downloaded.value
            downloaded_file = output / 'restored-video.webm'
            await download.save_as(downloaded_file)
            assert hashlib.sha256(downloaded_file.read_bytes()).hexdigest() == video['hash']
            await source(page)
            audio = await record(page, audio=True)
            assert audio['persisted'] and audio['size'] > 0
            await context.close()
            # Reopen the same on-disk profile in a fresh browser process.
            context, page = await launch()
            assert await page.locator('.local-recording-row').count() == 2
            await restored(page, video)
            assert not (await restored(page, audio))['isVideo']
            await page.evaluate('''filename => {
                const entry=localRecordings.get(filename); entry.row.querySelector('.btn-record-preview').click();
            }''', audio['filename'])
            await page.wait_for_function("document.getElementById('vault-audio').readyState>=2")
            assert await page.locator('#vault-video').is_hidden()
            await page.evaluate('''async filename => {
                const entry=localRecordings.get(filename);
                await deleteRecord(entry.row.querySelector('.btn-action-del'),filename);
            }''', video['filename'])
            await page.reload()
            await page.evaluate('recordingsReady')
            assert not await page.evaluate('filename=>localRecordings.has(filename)', video['filename'])
            await restored(page, audio)
            # Quota failure must retain a downloadable, explicitly unsaved clip.
            await source(page)
            await page.evaluate('''() => {
                window.normalStoreOperation=recordingStoreOperation;
                recordingStoreOperation=async (mode,operation) => {
                    if (mode==='readwrite') throw new DOMException('Injected quota failure','QuotaExceededError');
                    return normalStoreOperation(mode,operation);
                };
            }''')
            failed = await record(page)
            assert not failed['persisted']
            assert 'Storage full' in await page.locator('#record-storage-status').inner_text()
            assert await page.evaluate('''filename => {
                const entry=localRecordings.get(filename);
                const event=new Event('beforeunload',{cancelable:true}); window.dispatchEvent(event);
                return entry.row.querySelector('.record-save-state').textContent.includes('Not saved') &&
                       entry.row.querySelector('.record-download').href===entry.url && event.defaultPrevented;
            }''', failed['filename'])
            await page.evaluate('''async filename => {
                recordingStoreOperation=normalStoreOperation;
                const entry=localRecordings.get(filename);
                await deleteRecord(entry.row.querySelector('.btn-action-del'),filename);
            }''', failed['filename'])
            # A failed persistent delete must not remove the UI or saved object.
            await page.evaluate('''async filename => {
                recordingStoreOperation=async () => { throw new Error('Injected delete failure'); };
                const entry=localRecordings.get(filename);
                await deleteRecord(entry.row.querySelector('.btn-action-del'),filename);
                recordingStoreOperation=normalStoreOperation;
            }''', audio['filename'])
            assert 'Could not delete' in await page.locator('#record-storage-status').inner_text()
            await page.reload()
            await restored(page, audio)
            report = {'refresh':'PASS','browser_restart':'PASS','rotated_preview_after_refresh':'PASS',
                      'download_hash_matches':'PASS','audio_only_restored':'PASS','delete_survives_refresh':'PASS',
                      'quota_failure_retains_download_and_warns':'PASS','failed_delete_preserves_recording':'PASS',
                      'video':{key:value for key,value in video.items() if key!='url'}}
            (output / 'result.json').write_text(json.dumps(report,indent=2)+'\n')
            print(json.dumps(report,indent=2))
            await context.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--chromium',type=Path,required=True)
    asyncio.run(run(parser.parse_args()))
