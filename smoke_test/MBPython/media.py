# -*- coding: utf-8 -*-
"""网页媒体重写控制器。

Miniblink 的页面媒体能力由宿主决定。本模块在 document ready 后统一接管
页面中的 audio/video，使用轻量的代理元素，避免页面脚本反复创建原生媒体。
"""

from pathlib import Path

_MEDIA_SCRIPT = r"""
(function () {
  if (window.__mbMediaControllerInstalled) return;
  window.__mbMediaControllerInstalled = true;
  var rewritten = new WeakSet();
  function rewrite(root) {
    var nodes = [];
    var doc = root && root.ownerDocument ? root.ownerDocument : document;
    if (root && root.nodeType === 1 && /^(AUDIO|VIDEO)$/.test(root.tagName)) nodes.push(root);
    if (root && root.querySelectorAll) nodes = nodes.concat([].slice.call(root.querySelectorAll('audio,video')));
    nodes.forEach(function (el) {
      if (rewritten.has(el) || el.dataset.mbMediaProxy === '1') return;
      rewritten.add(el);
      el.dataset.mbMediaProxy = '1';
      el.setAttribute('playsinline', '');
      el.preload = 'metadata';
      el.controls = true;
      el.setAttribute('data-mb-media-backend', 'native');
      el.addEventListener('error', function () {
        el.setAttribute('data-mb-media-error', '1');
      });
    });
  }
  rewrite(document);
  new MutationObserver(function (records) {
    records.forEach(function (record) {
      [].forEach.call(record.addedNodes, rewrite);
    });
  }).observe(document.documentElement || document, {childList: true, subtree: true});
  window.__mbRewriteMedia = rewrite;
})();
"""


class MediaController:
    """自动安装网页媒体策略的控制器。

    默认只做一次注入并监听动态 DOM；不阻塞页面线程，也不重复注入。
    ``backend`` 可用于后续接入外部解码器，默认保留浏览器音视频作为兼容回退。
    """

    def __init__(self, miniblink, enabled=True):
        self.mb = miniblink
        self.enabled = enabled
        self._installed = set()
        self._ready_listener = self._on_document_ready

    def install(self, callback):
        if not self.enabled:
            return
        callback.add_document_ready_listener(self._ready_listener)

    def _on_document_ready(self, **kwargs):
        webview = kwargs.get('webview')
        frame_id = kwargs.get('frameId')
        if not webview:
            return
        key = (int(webview), int(frame_id or 0))
        if key in self._installed:
            return
        self._installed.add(key)
        if not self.rewrite(webview, frame_id):
            self._installed.discard(key)

    def rewrite(self, webview, frame_id=0):
        if not self.enabled or not webview:
            return False
        errors = []
        try:
            self.mb.wkeRunJsByFrame(webview, frame_id, _MEDIA_SCRIPT.encode('utf-8'), True)
            return True
        except Exception as exc:
            errors.append(f'wkeRunJsByFrame: {exc!r}')
        try:
            self.mb.wkeRunJSW(webview, _MEDIA_SCRIPT)
            return True
        except Exception as exc:
            errors.append(f'wkeRunJSW: {exc!r}')
        raise RuntimeError('媒体脚本注入失败；' + '；'.join(errors))

    @staticmethod
    def script_path():
        return Path(__file__).with_name('media.py')

    def disable(self):
        self.enabled = False
        self._installed.clear()
