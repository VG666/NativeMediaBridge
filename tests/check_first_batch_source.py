"""Source-shape regression only; NOT a runtime/concurrency or C++ compile test.
Run: python tests/check_first_batch_source.py [source-root]
No dependencies, builds, media/network access, or production-file writes.
"""
from pathlib import Path
import re
import sys

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
# Preserve quoted literals, remove comments so stale comments cannot satisfy checks.
LEX = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)


def clean(text):
    return LEX.sub(lambda m: ' ' if m[0].startswith(('/',)) else m[0], text)


def read(path):
    return clean((ROOT / path).read_text(encoding='utf-8-sig'))


def body(text, signature):
    start = text.index('{', text.index(signature))
    # Blank strings before balancing braces (JSON literals contain braces).
    masked = LEX.sub(lambda m: ' ' * len(m[0]), text)
    depth = 0
    for pos in range(start, len(masked)):
        depth += (masked[pos] == '{') - (masked[pos] == '}')
        if depth == 0:
            return text[start + 1:pos]
    raise AssertionError('unbalanced body: ' + signature)


def ordered(text, *parts):
    pos = 0
    for part in parts:
        hit = text.find(part, pos)
        assert hit >= 0, 'missing/out-of-order: ' + part
        pos = hit + len(part)


video = read('media/nmb_video.cpp')
audio = read('media/nmb_audio.cpp')
protocol = read('page/nmb_protocol.cpp')
life = read('media/nmb_media.cpp')
types = read('core/nmb_types.h')
checks = []


def check(name, fn):
    try:
        fn()
    except (AssertionError, ValueError) as exc:
        checks.append(False)
        print('FAIL:', name, '-', exc)
    else:
        checks.append(True)
        print('PASS:', name)


def require(condition, message):
    assert condition, message


for source, name in [(video, 'openInput'), (video, 'openVideo'), (audio, 'openAudio')]:
    def no_worker(source=source, name=name):
        text = body(source, 'bool ' + name + '(')
        require(not re.search(r'std::thread|CreateThread|_beginthread|\b(?:demuxLoop|decodeLoop|audioLoop)\s*\(', text), 'initializer starts worker')
    check(name + ' initializes without starting workers', no_worker)


def startup():
    ordered(protocol, 'ok = openVideo(media, view, hwnd);', 'if (ok) openAudio(media);',
            'ok = openAudio(media);', 'if (ok && !media->stop)', 'try {',
            'media->demuxer = std::thread(demuxLoop, media);', 'if (media->video.codec)',
            'media->decoder = std::thread(decodeLoop, media, view, hwnd);',
            'if (media->audio.codec)', 'media->audioDecoder = std::thread(audioLoop, media);',
            'catch (...)', 'ok = false;')
check('initialize both sides before guarded worker startup', startup)


def cleanup(text, queues=False):
    ordered(text, 'media->stop = true;', 'media->audio.stop = true;',
            'if (media->demuxer.joinable()) media->demuxer.join();',
            'if (media->decoder.joinable()) media->decoder.join();',
            'if (media->audioDecoder.joinable()) media->audioDecoder.join();',
            'closeFfmpegAudio(media->audio);', 'closeFfmpegVideo(media->video);')
    if queues:
        ordered(text, 'closeFfmpegVideo(media->video);', 'media->videoQueue.clear();', 'media->audioQueue.clear();')
    else:
        ordered(text, 'closeFfmpegVideo(media->video);', 'delete media;')
check('startup failure stops/joins before close and queue clear', lambda: cleanup(body(protocol, 'if (!ok)'), True))
check('normal release joins before close/delete', lambda: cleanup(body(life, 'void releaseMedia(')))
check('close cancels opening without early delete', lambda: ordered(body(protocol, 'if (op == L"close")'),
      'browser->media.erase(it);', 'media->stop = true;', 'if (!media->openRunning.load()) releaseMediaAsync(media);'))
check('replacement cancels old opening before installing new media', lambda: ordered(protocol,
      'const bool evictOpen = media && media->openRunning;', 'if (media) media->stop = true;',
      'if (media && !evictOpen) releaseMediaAsync(media);', 'media = new Media();'))
check('navigation cancels opening and defers release', lambda: ordered(body(life, 'void releaseAllMedia('),
      'if (item.second && item.second->openRunning)', 'item.second->stop = true;', 'continue;',
      'releaseMediaAsync(item.second);', 'browser->media.clear();'))
check('completion serializes ownership and disposes unlisted media', lambda: ordered(protocol,
      'std::lock_guard<std::mutex> lock(owner->mutex);', 'media->openRunning = false;',
      'it->second == media', 'if (!listed)', 'releaseMediaAsync(media);'))
check('input interruption installed before blocking open', lambda: ordered(body(video, 'bool openInput('),
      'attachInterruptCallback(video.format, &media->videoIo, media);', 'avformat_open_input('))
check('interrupt callback observes cancellation', lambda: require(
      'if (guard->media->stop.load()) return 1;' in body(life, 'int interruptIo('), 'missing stop check'))
check('peekTimestamp copies pts/dts under queue lock', lambda: ordered(body(types, 'bool peekTimestamp('),
      'std::lock_guard<std::mutex> lock(mutex);', 'if (packets.empty()) return false;',
      'const AVPacket* packet = packets.front();',
      'stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;', 'return true;'))
check('PacketQueue destructor clears owned packets', lambda: (
      require('clear();' in body(types, '~PacketQueue()'), 'destructor does not clear'),
      ordered(body(types, 'void clear()'), 'std::lock_guard<std::mutex> lock(mutex);',
              'for (AVPacket*& packet : packets) av_packet_free(&packet);', 'packets.clear();')))
check('paused decoder consumes copied timestamp, not borrowed packet', lambda: ordered(video,
      'int64_t stamp = AV_NOPTS_VALUE;', 'media->videoQueue.peekTimestamp(stamp);',
      'stamp != AV_NOPTS_VALUE', 'stamp * video.timeBase', 'media->videoQueue.pop();'))


def inventory():
    # Count definitions + declarations + calls, so new callers require explicit review.
    expected = {
        'openInput': {'core/nmb_internal.h': 1, 'media/nmb_video.cpp': 2, 'media/nmb_audio.cpp': 1},
        'openVideo': {'core/nmb_internal.h': 1, 'media/nmb_video.cpp': 1, 'page/nmb_protocol.cpp': 1},
        'openAudio': {'core/nmb_internal.h': 1, 'media/nmb_audio.cpp': 1, 'page/nmb_protocol.cpp': 2},
        'peekTimestamp': {'core/nmb_types.h': 1, 'media/nmb_video.cpp': 1},
    }
    found = {name: {} for name in expected}
    count = 0
    for path in ROOT.rglob('*'):
        if path.suffix.lower() not in {'.c', '.cpp', '.cc', '.cxx', '.h', '.hpp', '.inl', '.inc'}:
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel.startswith('ffmpeg-static/') or any(part.startswith('.') for part in path.relative_to(ROOT).parts):
            continue
        text = clean(path.read_text(encoding='utf-8-sig'))
        count += 1
        require(not re.search(r'\bpeek\s*\(', text), 'legacy peek: ' + rel)
        for name in expected:
            n = len(re.findall(r'\b' + name + r'\s*\(', text))
            if n:
                found[name][rel] = n
    require(found == expected, 'call-site inventory changed: ' + repr(found))
    print('  scanned', count, 'source/header files; caller inventory matches')
check('all call sites and absence of legacy peek', inventory)
print(f'SOURCE ONLY: {sum(checks)}/{len(checks)} passed; no runtime/concurrency guarantees.')
sys.exit(0 if all(checks) else 1)
