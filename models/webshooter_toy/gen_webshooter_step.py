"""
蛛网发射器（玩具）装配模型 -> STEP

定位：腕戴玩具，喷出水性成丝液（PVA 胶 / 瓜尔胶凝胶 / 水性拉丝液），用于趣味喷射。
      不载人、不承重、不做锚固，喷出的丝只粘得住极轻的东西。

与上一版（台架双组份出丝器）的两处根本区别：
  1. 单组份水性料 -> 不在喷嘴/管内固化，不堵，可水洗；不含异氰酸酯（不刺激皮肤和呼吸道）。
  2. 低压微泵（<0.5 bar），无压力腔；电气为低压直流，电池与泵均为采购成品。

单位 mm。坐标：X = 手指方向（前为正），Y = 手腕横向，Z = 手背向上（掌心为负）。
手腕近似为 r30 的圆柱，轴线沿 X， cuff 绕 X 轴包覆。

运行：python models/webshooter_toy/gen_webshooter_step.py
"""

import cadquery as cq

OUT_STEP = "models/webshooter_toy/webshooter_toy.step"

C_PRINT = cq.Color(0.85, 0.20, 0.25)     # 打印件：护腕、机壳、盖、护圈、扳机
C_BUY = cq.Color(0.30, 0.55, 0.85)       # 采购件：微泵、喷嘴
C_TANK = cq.Color(0.40, 0.80, 0.55)      # 采购件：料瓶
C_BATT = cq.Color(0.35, 0.35, 0.40)      # 采购件：电池


def cyl_x(r, x0, x1, y=0.0, z=0.0):
    return cq.Workplane("YZ").center(y, z).circle(r).extrude(x1 - x0).translate((x0, 0, 0))


def tube_x(r_out, r_in, x0, x1, y=0.0, z=0.0):
    wp = (cq.Workplane("YZ").center(y, z)
          .circle(r_out).circle(r_in).extrude(x1 - x0))
    return wp.translate((x0, 0, 0))


def cone_x(r1, r2, x0, x1, y1=0.0, z1=0.0, y2=0.0, z2=0.0):
    wp = (cq.Workplane("YZ").center(y1, z1).circle(r1)
          .workplane(offset=x1 - x0).center(y2 - y1, z2 - z1).circle(r2).loft())
    return wp.translate((x0, 0, 0))


def box(cx, cy, cz, lx, ly, lz):
    return cq.Workplane("XY").box(lx, ly, lz).translate((cx, cy, cz))


def uni(*parts):
    res = parts[0]
    for p in parts[1:]:
        res = res.union(p.val())
    return res


# ---------- 打印件：护腕（绕 X 轴的 C 形壳，掌心侧开口）----------

WRIST_R = 30.0
cuff = tube_x(33.0, WRIST_R, -30.0, 30.0)
cuff = cuff.cut(box(0.0, 0.0, -30.0, 90.0, 90.0, 48.0).val())   # 切掉掌心侧，Z<-6 去除

for y in (+32.0, -32.0):                                        # 两侧绑带耳片 + 穿带孔
    lug = box(0.0, y, -4.0, 14.0, 8.0, 12.0)
    cuff = cuff.union(lug.cut(cyl_x(2.5, -10.0, 10.0, y=y, z=-4.0).val()).val())

# ---------- 打印件：机壳（敞口仓）+ 盖子 ----------

DECK_Z0, DECK_Z1 = 33.0, 63.0
deck = box(0.0, 0.0, (DECK_Z0 + DECK_Z1) / 2, 60.0, 56.0, DECK_Z1 - DECK_Z0)
deck = deck.cut(box(0.0, 0.0, 54.0, 54.0, 50.0, 18.0).val())    # 仓腔：底 12mm，侧壁 3mm
deck = deck.cut(cyl_x(5.5, 26.0, 34.0, z=50.0).val())           # 喷嘴过孔（采购件的让位孔）
lid = box(0.0, 0.0, 65.0, 60.0, 56.0, 4.0)

# ---------- 采购件：料瓶、微泵 ----------

tank = cyl_x(9.0, -22.0, 20.0, y=13.0, z=54.0)
tank = tank.union(cyl_x(4.0, 20.0, 26.0, y=13.0, z=54.0).val())     # 瓶口
tank = tank.union(cyl_x(9.0, -24.0, -22.0, y=13.0, z=54.0).val())   # 瓶底凸缘

# 微泵包络：低压直流震动泵/隔膜泵，额定件，不做压力腔
pump = cyl_x(8.5, -20.0, 20.0, y=-13.0, z=54.0)
pump = pump.union(cyl_x(9.0, -6.0, 8.0, y=-13.0, z=54.0).val())     # 线圈段

# ---------- 采购件：喷嘴（出丝口 r1.6）----------

nozzle = cone_x(5.0, 1.6, 30.0, 52.0, z1=50.0, z2=44.0)

# ---------- 打印件：喷嘴护圈（喷口内缩 6mm，避免戳碰）----------

guard = tube_x(7.0, 5.0, 46.0, 58.0, z=44.0)
guard = guard.union(box(40.0, 0.0, 46.0, 20.0, 8.0, 6.0).val())     # 连回机壳的支臂

# ---------- 打印件：掌心扳机（微动开关拨杆，非阀）----------

trigger = box(14.0, 0.0, -20.0, 22.0, 34.0, 6.0)
trigger = trigger.union(box(14.0, 0.0, -11.0, 6.0, 12.0, 14.0).val())   # 连到护腕下缘

# ---------- 采购件：电池包 + 打印件：前臂绑带 ----------

battery = box(-52.0, 0.0, 40.5, 38.0, 34.0, 15.0)   # 坐在前臂绑带外表面（Z=33）上
band = tube_x(33.0, WRIST_R, -72.0, -32.0)
band = band.cut(box(-52.0, 0.0, -30.0, 50.0, 90.0, 46.0).val())     # 同样开口在掌心侧

# ---------- 导出 ----------

PARTS = [
    ("cuff_printed", cuff, C_PRINT),
    ("deck_printed", deck, C_PRINT),
    ("lid_printed", lid, C_PRINT),
    ("nozzle_guard_printed", guard, C_PRINT),
    ("trigger_printed", trigger, C_PRINT),
    ("forearm_band_printed", band, C_PRINT),
    ("tank_buy", tank, C_TANK),
    ("micro_pump_buy", pump, C_BUY),
    ("nozzle_buy", nozzle, C_BUY),
    ("battery_buy", battery, C_BATT),
]

assy = cq.Assembly()
for name, wp, color in PARTS:
    bb = wp.val().BoundingBox()
    assert bb.xmax - bb.xmin > 0.1 and bb.ymax - bb.ymin > 0.1 and bb.zmax - bb.zmin > 0.1, name
    assy.add(wp.val(), name=name, color=color)

assy.save(OUT_STEP)
print("wrote", OUT_STEP)
for name, wp, _ in PARTS:
    bb = wp.val().BoundingBox()
    print(f"  {name:22s} X[{bb.xmin:7.1f},{bb.xmax:7.1f}] "
          f"Y[{bb.ymin:6.1f},{bb.ymax:6.1f}] Z[{bb.zmin:7.1f},{bb.zmax:7.1f}]")
