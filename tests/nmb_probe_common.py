# -*- coding: utf-8 -*-
"""界面响应度观测：判定"卡死"的公共代码，宿主探针与内核对照程序共用。

界面线程就是泵消息的那个循环，所以"每秒泵到多少次消息、最长一次停顿多久"
直接反映界面有没有被拖住。真正卡死时连 pump() 都回不来，主循环里的时间判断
也跟着失效，所以这里另起线程做看门狗：只观测、不阻塞，超时后打印结论并以
退出码 3 强制结束（见 STUCK_EXIT_CODE）。
"""
import os
import sys
import threading
import time

# 判定卡死后的退出码，宿主脚本据此区分"卡死"和"正常跑完"。
STUCK_EXIT_CODE = 3

# 超过这个秒数没泵到消息，就算"界面无响应"，开始报告。
STUCK_REPORT_SECONDS = 3.0


class ResponseMonitor:
    """记录泵消息节奏，并用看门狗给出卡死结论。"""

    def __init__(self, quiet=False):
        self.quiet = quiet
        self.started = time.monotonic()
        self.last_tick = self.started
        self.ticks = 0
        self.max_gap = 0.0
        self.per_second = []
        self._ticks_at_second = 0
        self._second_start = self.started

    def tick(self):
        """每泵一次消息调一次，返回当前时间。"""
        now = time.monotonic()
        gap = now - self.last_tick
        self.last_tick = now
        if gap > self.max_gap:
            self.max_gap = gap
        self.ticks += 1
        if now - self._second_start >= 1.0:
            self.per_second.append(
                (len(self.per_second) + 1, self.ticks - self._ticks_at_second)
            )
            self._ticks_at_second = self.ticks
            self._second_start = now
        return now

    @property
    def idle(self):
        """距上一次泵到消息已经过去多久（秒）。"""
        return time.monotonic() - self.last_tick

    @property
    def elapsed(self):
        return time.monotonic() - self.started

    def stats(self):
        total = self.elapsed
        return {
            "seconds": total,
            "ticks": self.ticks,
            "per_second": (self.ticks / total) if total > 0 else 0.0,
            "max_gap": self.max_gap,
            "per_second_detail": self.per_second,
        }

    def start_watchdog(self, stuck_limit):
        """持续观测界面是否无响应：超过 stuck_limit 秒判定卡死并强制退出。

        看门狗在独立线程里跑，界面线程卡在原生调用里时它依然能输出，
        这样"卡死"就不是一句感觉，而是有时间、有结论的数据。
        stuck_limit <= 0 表示只报告、不强制退出（手动操作窗口时用）。
        """

        def watch():
            reported = False
            while True:
                time.sleep(1.0)
                idle = self.idle
                if stuck_limit > 0 and idle >= stuck_limit:
                    if not self.quiet:
                        print(
                            f"\n[看门狗] 界面已无响应 {idle:.1f} 秒（上限 {stuck_limit:.0f} 秒），"
                            f"判定为卡死，强制退出。"
                        )
                        sys.stdout.flush()
                    os._exit(STUCK_EXIT_CODE)
                if idle >= STUCK_REPORT_SECONDS:
                    if not reported:
                        reported = True
                        if not self.quiet:
                            print(
                                f"\n[看门狗] 界面开始无响应：已经 {idle:.1f} 秒没有回到消息循环。"
                            )
                            sys.stdout.flush()
                elif reported:
                    reported = False
                    if not self.quiet:
                        print(
                            f"[看门狗] 界面恢复响应：最长一次停顿 {self.max_gap:.1f} 秒。"
                        )
                        sys.stdout.flush()

        threading.Thread(target=watch, daemon=True).start()


def print_responsiveness(stats, quiet=False):
    if not stats:
        return
    if quiet:
        print(
            f"\n界面响应度：平均 {stats['per_second']:.0f} 次/秒，"
            f"最长一次停顿 {stats['max_gap'] * 1000:.0f} ms"
        )
        return
    print("\n界面响应度（每秒泵到的消息次数，正常约 100）：")
    for index, count in stats["per_second_detail"]:
        bar = "#" * min(60, max(0, int(count / 2)))
        flag = "  <== 卡顿" if count < 30 else ""
        print(f"  第 {index:2d} 秒: {count:4d} {bar}{flag}")
    print(
        f"  合计 {stats['seconds']:.1f}s，平均 {stats['per_second']:.0f} 次/秒，"
        f"最长一次停顿 {stats['max_gap'] * 1000:.0f} ms"
    )
