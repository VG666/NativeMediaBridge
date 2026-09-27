"""Minimal source-contract + seek interleaving regression (no player required)."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
v = (root / 'media/nmb_video.cpp').read_text(encoding='utf-8-sig')
a = (root / 'media/nmb_audio.cpp').read_text(encoding='utf-8-sig')
p = (root / 'page/nmb_protocol.cpp').read_text(encoding='utf-8-sig')
demux = v.split('void demuxLoop(', 1)[1].split('void decodeLoop(', 1)[0]
decode = v.split('void decodeLoop(', 1)[1].split('bool openInput(', 1)[0]
assert '.        if' not in demux
for text in ('const double window', 'bool startupFill', 'auto lastData', 'auto seekTo', 'while (!media->stop)'):
    assert text in demux, text
assert 'avcodec_flush_buffers' not in demux
assert decode.count('avcodec_flush_buffers(video.codec)') == 1
assert a.count('avcodec_flush_buffers(audio.codec)') == 1
assert decode.count('publishFrame(media, video.frame)') == 1  # EOF uses outputFrame too
assert 'if (!current()) return;' in decode
assert 'if (!current()) continue;\n                media->ended' in decode
assert decode.count('std::this_thread::sleep_') == 1
assert a.count('std::this_thread::sleep_') == 1
for source in (decode, a):
    assert 'commit.unlock();\n' in source
assert 'if (media->seeking || epoch != audio.clockEpoch.load()) continue;' in a
assert 'swr_close(audio.resampler)' in a
assert 'kAudioSamplesPerBuffer - filled, nullptr, 0)' in a
assert 'if (pending) { wait(3); continue; }' in a
seek = p.split('else if (op == L"seek")', 1)[1]
assert seek.index('commit(media->decodeMutex)') < seek.index('media->seeking = true') < seek.index('seekPending.store(true)')
assert 'std::min(media->position.load(), audio.clock.load())' in v
assert 'stamped ? pts - clock : 0.0, media->position.load(),' in v

# Exhaust all placements of an old commit around request/completion of two seeks.
# Mutex serializes each action; generation changes only at completion.
for slot in range(5):
    epoch, seeking, old_epoch, accepted = 0, False, 0, False
    actions = ['request', 'complete', 'request', 'complete']
    actions.insert(slot, 'old_commit')
    for action in actions:
        if action == 'request':
            seeking = True
        elif action == 'complete':
            epoch += 1
            seeking = False
        else:
            accepted = not seeking and old_epoch == epoch
    assert accepted == (slot == 0), (slot, actions)
print('PASS: seek source contracts and 5 serialized stale-commit interleavings')
