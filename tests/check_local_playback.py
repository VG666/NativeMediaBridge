"""Local-only Windows host regression; Python reuses the existing ctypes host."""
import ctypes
import functools
import http.server
import pathlib
import re
import subprocess
import sys
import threading
import os
import json
import urllib.request
import urllib.error
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
audio = '--audio' in sys.argv
no_range = '--no-range' in sys.argv  # Negative control; playback assertions still apply.
OUT = ROOT / 'tests' / ('_local_playback_no_range' if no_range else
                       '_local_playback_audio' if audio else '_local_playback')
OUT.mkdir(exist_ok=True)
mp4 = OUT / 'sample.mp4'
subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
    '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=24',
    *(['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-c:a', 'aac'] if audio else []),
    '-t', '12',
    '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(mp4)], check=True)
data = mp4.read_bytes()
(OUT / 'sample.m4s').write_bytes(data)
seen = []
markers = []
ranges = []
class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/marker?'):
            markers.append(self.path)
            with (OUT/'page.log').open('a', encoding='utf-8') as f:
                f.write(urllib.parse.unquote(self.path.split('?', 1)[1]) + '\n')
            self.send_response(204); self.end_headers(); return
        if self.path == '/bytes-ok':
            seen.append(True)
            self.send_response(204); self.end_headers(); return
        super().do_GET()

    def send_head(self):
        self.remaining = None
        if no_range:
            return super().send_head()
        path = pathlib.Path(self.translate_path(self.path))
        if not path.is_file():
            return super().send_head()
        size = path.stat().st_size
        start, end = 0, size - 1
        header = self.headers.get('Range')
        if header:
            match = re.fullmatch(r'bytes=(\d*)-(\d*)', header)
            if not match or not any(match.groups()):
                self.send_error(400); return None
            first, last = match.groups()
            if first:
                start = int(first)
                end = min(int(last), end) if last else end
            else:
                start = max(0, size - int(last))
            if start > end or start >= size:
                self.send_response(416)
                self.send_header('Content-Range', 'bytes */%d' % size)
                self.send_header('Content-Length', '0')
                self.end_headers(); return None
        f = path.open('rb'); f.seek(start)
        self.remaining = end - start + 1
        self.send_response(206 if header else 200)
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Type', self.guess_type(str(path)))
        self.send_header('Content-Length', str(self.remaining))
        if header:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
            ranges.append((start, end))
        self.end_headers()
        return f

    def copyfile(self, source, outputfile):
        if self.remaining is None:
            return super().copyfile(source, outputfile)
        try:
            while self.remaining:
                chunk = source.read(min(65536, self.remaining))
                if not chunk: break
                outputfile.write(chunk)
                self.remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # A seeking client can close its previous response.

handler = functools.partial(Handler, directory=str(OUT))
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
page = '''<!doctype html><video id="v" src="sample.mp4" style="width:320px;height:180px"></video>
<script>
var v=document.getElementById('v'), started=false;
function marker(s){fetch('/marker?'+encodeURIComponent(s))}
var query=window.mbQuery;
window.mbQuery=function(id,req,cb){
  var op=String(req).split('\u005ct')[0];
  if(op==='seek')marker('seek-send '+req);
  return query.call(window,id,req,function(msg,response){
    if(op==='seek')marker('seek-reply '+response);
    if(started&&op==='state')marker('state-reply '+response);
    if(cb)cb(msg,response);
  });
};
v.addEventListener('seeked',function(){marker('seeked-event '+v.currentTime)});
v.addEventListener('error',function(){marker('media-error')});
var timer=setInterval(function(){if(!v.__nmbHooked||started)return;started=true;clearInterval(timer);
v.play();setTimeout(function(){marker('setter-before '+v.currentTime);v.currentTime=7;marker('setter-after '+v.currentTime)},3000)},100);
Promise.all(['sample.mp4','sample.m4s'].map(function(u){return fetch(u).then(function(r){return r.arrayBuffer()}).then(function(b){if(b.byteLength!==SIZE)throw Error('fetch bytes');return 1})}).concat(['sample.mp4','sample.m4s'].map(function(u){return new Promise(function(resolve,reject){var x=new XMLHttpRequest();x.open('GET',u);x.responseType='arraybuffer';x.onload=function(){if(x.response&&x.response.byteLength===SIZE)resolve(1);else reject(Error('xhr bytes'))};x.onerror=reject;x.send()})}))).then(function(){return fetch('/bytes-ok')});
</script>'''.replace('SIZE', str(len(data)))
(OUT / 'index.html').write_text(page, encoding='utf-8')
# A server-observed marker requires all four actual response lengths to match.
(OUT/'page.log').write_text('', encoding='utf-8')
log = OUT / 'frames.log'
native_log = pathlib.Path('C:/Windows/Temp/nmb_local_playback_frames.log')
if log.exists(): log.unlink()
if native_log.exists(): native_log.unlink()
# NativeMediaBridge 的同步日志仍经窄字符 fopen，使用固定 ASCII 绝对路径。
env = dict(os.environ, NMB_SYNC_LOG=str(native_log), PYTHONIOENCODING='utf-8')
try:
    base = 'http://127.0.0.1:%d' % server.server_port
    if not no_range:
        for value, start, end in [('bytes=10-29', 10, 29),
                                  ('bytes=100-', 100, len(data)-1),
                                  ('bytes=-17', len(data)-17, len(data)-1)]:
            with urllib.request.urlopen(urllib.request.Request(base+'/sample.mp4', headers={'Range': value})) as r:
                assert r.status == 206
                assert r.headers['Content-Range'] == 'bytes %d-%d/%d' % (start, end, len(data))
                assert r.read() == data[start:end+1]
        try:
            urllib.request.urlopen(urllib.request.Request(base+'/sample.mp4', headers={'Range': 'bytes=%d-' % len(data)}))
            raise AssertionError('missing 416')
        except urllib.error.HTTPError as e:
            assert e.code == 416 and e.headers['Content-Range'] == 'bytes */%d' % len(data)
        ranges.clear()
    run = subprocess.run([sys.executable, '-u', str(ROOT/'tests/test_bindwebview.py'),
        '--url', 'http://127.0.0.1:%d/index.html' % server.server_port,
        '--duration', '10', '--auto', '--compat-shim', 'desktop'], cwd=ROOT, env=env,
        capture_output=True, timeout=35)
    (OUT/'host.log').write_bytes(run.stdout+run.stderr)
    if native_log.exists():
        log.write_bytes(native_log.read_bytes())
    print(run.stdout.decode('utf-8', 'replace'))
    assert run.returncode == 0, run.stderr.decode('utf-8', 'replace')
    rows = [tuple(map(float,m)) for m in re.findall(r'^(\d+) pts=([\d.]+).*? pos=([\d.]+)', log.read_text(), re.M)]
    assert len(rows)>30, ('decoded frames',len(rows))
    before=[r for r in rows if 0.2<r[1]<3.5]
    after=[r for r in rows if r[1]>=7]
    assert len(before)>10 and before[-1][2]-before[0][2]>1, before
    assert len(after)>10 and after[-1][2]-after[0][2]>1, after
    assert any(b[1]-a[1]>3 for a,b in zip(rows,rows[1:])), 'seek not observed'
    assert seen, 'fetch/XHR bytes did not survive'
    messages = [urllib.parse.unquote(m.split('?', 1)[1]) for m in markers]
    for prefix in ('setter-before ', 'setter-after 7', 'seek-send seek\t', 'seek-reply '):
        assert any(m.startswith(prefix) for m in messages), ('missing marker', prefix)
    states = [json.loads(m[len('state-reply '):]) for m in messages if m.startswith('state-reply ')]
    assert states and not any(s.get('error') for s in states), states
    positions = [s['position'] for s in states if s.get('position', 0) >= 7]
    assert len(positions)>5 and max(positions)-min(positions)>1, positions
    seek_results = re.findall(r'seek target=7.000 from=5.500 reuse=(\d) result=(-?\d+)', log.read_text())
    assert seek_results == [('0', '0')], ('actual FFmpeg seek', seek_results)
    if not no_range:
        assert any(start > 0 for start, end in ranges), ('missing nonzero Range', ranges)
    if audio:
        clocks = [float(c) for c in re.findall(r'pts=[\d.]+ clock=([\d.]+).*audio=1', log.read_text()) if float(c)>=7]
        assert len(clocks)>10 and max(clocks)-min(clocks)>1, ('audio clock', clocks)
        print('audio_clock=%.3f..%.3f' % (min(clocks), max(clocks)))
    print('native_seek=%s range_requests=%s page_position=%.3f..%.3f' % (seek_results, ranges, min(positions), max(positions)))
    print('PASS decoded_frames=%d before_position=%.3f..%.3f after_seek_position=%.3f..%.3f fetch_xhr_bytes=%d x4' %
          (len(rows),before[0][2],before[-1][2],after[0][2],after[-1][2],len(data)))
finally:
    server.shutdown()
