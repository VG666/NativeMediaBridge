"""
腕戴蛛网发射器（玩具）v2：气助版 -> STEP

相比 v1（电动微泵）的改动：
  1. 气源改为**压缩空气**：气瓶 -> 减压阀 -> 电磁阀 -> 气助虹吸喷嘴。
     气瓶、减压阀、电磁阀全部为采购成品；减压阀自带泄压，出口压力固定，不做压力腔设计。
  2. **料瓶保持常压**：靠喷嘴处文丘里虹吸吸料，绝不给料瓶加压。
  3. 增加**激光测距模块**（1类 ToF，如 VL53L0X/1X）：测距后由 MCU 决定电磁阀开启时长，
     并做近距/超距禁发。
  4. 每个部件在模型里用 3D 文字标注用途（simhei 字体实体，带引出线）。

控制逻辑（在 MCU 里，不在模型里）：
    d = 测距读数
    d < 15cm 或 d > 120cm  ->  不发射（近距防喷脸 / 超距打不到）
    否则  电磁阀开启 t = 60ms + 0.8ms/cm * d，上限 250ms
    出口压力由减压阀固定，MCU 不调压

单位 mm。坐标：X = 手指方向（前为正），Y = 手腕横向，Z = 手背向上（掌心为负）。
运行：python models/webshooter_toy/gen_webshooter_v2_step.py
"""

import cadquery as cq

OUT_STEP = "models/webshooter_toy/webshooter_v2_air.step"
FONT = "C:/Windows/Fonts/simhei.ttf"

C_PRINT = cq.Color(0.85, 0.20, 0.25)     # 打印件
C_AIR = cq.Color(0.25, 0.65, 0.90)       # 采购件：气路（瓶/减压阀/电磁阀）
C_TANK = cq.Color(0.40, 0.80, 0.55)      # 采购件：料瓶（常压）
C_ELEC = cq.Color(0.35, 0.35, 0.40)      # 采购件：电池 / 控制板 / 测距
C_LABEL = cq.Color(0.10, 0.10, 0.12)     # 标注文字


def cyl_x(r, x0, x1, y=0.0, z=0.0):
    return cq.Workplane("YZ").center(y, z).circle(r).extrude(x1 - x0).translate((x0, 0, 0))


def tube_x(r_out, r_in, x0, x1, y=0.0, z=0.0):
    wp = cq.Workplane("YZ").center(y, z).circle(r_out).circle(r_in).extrude(x1 - x0)
    return wp.translate((x0, 0, 0))


def cone_x(r1, r2, x0, x1, y1=0.0, z1=0.0, y2=0.0, z2=0.0):
    wp = (cq.Workplane("YZ").center(y1, z1).circle(r1)
          .workplane(offset=x1 - x0).center(y2 - y1, z2 - z1).circle(r2).loft())
    return wp.translate((x0, 0, 0))


def box(cx, cy, cz, lx, ly, lz):
    return cq.Workplane("XY").box(lx, ly, lz).translate((cx, cy, cz))


def cyl_z(r, z0, z1, x=0.0, y=0.0):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(r).extrude(z1 - z0)


def uni(*parts):
    res = parts[0]
    for p in parts[1:]:
        res = res.union(p.val())
    return res


# ---------- 打印件：护腕 ----------

WRIST_R = 30.0
cuff = tube_x(33.0, WRIST_R, -30.0, 30.0)
cuff = cuff.cut(box(0.0, 0.0, -30.0, 90.0, 90.0, 48.0).val())          # 掌心侧开口
for y in (+32.0, -32.0):
    lug = box(0.0, y, -4.0, 14.0, 8.0, 12.0)
    cuff = cuff.union(lug.cut(cyl_x(2.5, -10.0, 10.0, y=y, z=-4.0).val()).val())

# ---------- 打印件：机壳 + 顶盖 ----------

deck = box(0.0, 0.0, 48.0, 60.0, 56.0, 30.0)                           # Z 33..63
deck = deck.cut(box(0.0, 0.0, 54.0, 54.0, 50.0, 18.0).val())           # 仓腔 Z 45..63
deck = deck.cut(cyl_x(5.5, 26.0, 34.0, z=50.0).val())                  # 喷嘴让位孔
lid = box(0.0, 0.0, 65.0, 60.0, 56.0, 4.0)                             # Z 63..67

# ---------- 采购件：气路（前臂段）----------

# 气瓶：采购成品，带检验标记与泄压；只做包络，不做壁厚设计
air_tank = uni(cyl_x(15.0, -87.0, -46.0, z=46.0),
               cone_x(15.0, 13.0, -92.0, -87.0, z1=46.0, z2=46.0),
               cone_x(15.0, 7.0, -46.0, -41.0, z1=46.0, z2=46.0),
               cyl_x(5.0, -41.0, -36.0, z=46.0))

# 减压阀：额定件，出口压力固定，自带泄压
regulator = uni(box(-32.0, 0.0, 46.0, 10.0, 20.0, 20.0),
                cyl_z(4.0, 56.0, 62.0, x=-32.0, y=0.0))                # 调压旋钮

# 前臂绑带（打印件，承载气瓶）
band = tube_x(33.0, WRIST_R, -95.0, -32.0)
band = band.cut(box(-63.0, 0.0, -30.0, 70.0, 90.0, 46.0).val())

# ---------- 采购件：电磁阀（控放气）----------

valve = uni(box(0.0, -13.0, 54.0, 22.0, 18.0, 18.0),
            cyl_x(3.0, 11.0, 17.0, y=-13.0, z=54.0),                   # 出气
            cyl_x(3.0, -17.0, -11.0, y=-13.0, z=54.0))                 # 进气

# ---------- 采购件：料瓶（常压，虹吸取料）----------

fluid = uni(cyl_x(9.0, -22.0, 20.0, y=13.0, z=54.0),
            cyl_x(4.0, 20.0, 26.0, y=13.0, z=54.0),
            cyl_x(9.0, -24.0, -22.0, y=13.0, z=54.0))

# ---------- 采购件：激光测距 + 控制板 + 电池 ----------

laser = uni(box(36.0, 0.0, 60.0, 12.0, 16.0, 10.0),
            cyl_x(4.0, 42.0, 44.0, z=60.0))                            # 1 类 ToF，向外
mcu = box(0.0, 0.0, 46.5, 30.0, 7.0, 3.0)
battery = box(-63.0, 0.0, 67.0, 34.0, 28.0, 12.0)

# ---------- 采购件：气助虹吸喷嘴 + 打印件：护圈 ----------

nozzle = uni(cone_x(5.0, 1.6, 30.0, 52.0, z1=50.0, z2=44.0),
             cyl_x(6.0, 30.0, 34.0, z=50.0))                           # 气帽
guard = uni(tube_x(7.0, 5.0, 46.0, 58.0, z=44.0),
            box(40.0, 0.0, 46.0, 20.0, 8.0, 6.0))                      # 喷口内缩 6mm

# ---------- 打印件：扳机（只驱动电气开关）----------

trigger = uni(box(14.0, 0.0, -20.0, 22.0, 34.0, 6.0),
              box(14.0, 0.0, -11.0, 6.0, 12.0, 14.0))

# ---------- 3D 文字标注（含引出线）----------

def label(text, x, y, z, edge, side):
    wp = (cq.Workplane("XY").transformed(offset=(x, y, z))
          .text(text, 5.0, 0.4, fontPath=FONT, halign="left", valign="center"))
    parts = list(wp.solids().vals())
    y0, y1 = sorted([edge * side, y - 2.5 * side])                      # 引出线
    parts.append(box(x - 1.5, (y0 + y1) / 2.0, z, 0.8, max(y1 - y0, 0.1), 0.8).val())
    return cq.Compound.makeCompound(parts)


L = label
LABELS = [
    ("护腕(打印件)", -30.0, -46.0, 28.0, 33.0, -1),
    ("扳机(仅电气开关)", 4.0, -46.0, -18.0, 17.0, -1),
    ("前臂绑带(固定气瓶)", -90.0, -46.0, 34.0, 33.0, -1),
    ("电磁阀(控放气)", -6.0, -46.0, 54.0, 28.0, -1),
    ("控制板(MCU·测距判定)", -20.0, -46.0, 46.0, 28.0, -1),
    ("料瓶(常压·虹吸)", -24.0, -46.0, 63.0, 28.0, -1),
    ("虹吸喷嘴(气助)", 34.0, -46.0, 48.0, 5.0, -1),
    ("喷嘴护圈(防戳碰)", 56.0, -46.0, 44.0, 7.0, -1),
    ("气瓶(采购·带泄压)", -84.0, 46.0, 46.0, 15.0, +1),
    ("减压阀(额定·定压)", -40.0, 46.0, 58.0, 10.0, +1),
    ("电池", -80.0, 46.0, 68.0, 14.0, +1),
    ("机壳(打印件)", -30.0, 46.0, 48.0, 28.0, +1),
    ("顶盖(打印件)", 4.0, 46.0, 65.0, 28.0, +1),
    ("激光测距(1类ToF)", 34.0, 46.0, 60.0, 8.0, +1),
]

# ---------- 导出 ----------

PARTS = [
    ("cuff_printed", cuff, C_PRINT),
    ("deck_printed", deck, C_PRINT),
    ("lid_printed", lid, C_PRINT),
    ("forearm_band_printed", band, C_PRINT),
    ("nozzle_guard_printed", guard, C_PRINT),
    ("trigger_printed", trigger, C_PRINT),
    ("air_cylinder_buy", air_tank, C_AIR),
    ("regulator_buy", regulator, C_AIR),
    ("solenoid_valve_buy", valve, C_AIR),
    ("siphon_nozzle_buy", nozzle, C_AIR),
    ("fluid_bottle_buy", fluid, C_TANK),
    ("laser_rangefinder_buy", laser, C_ELEC),
    ("mcu_board_buy", mcu, C_ELEC),
    ("battery_buy", battery, C_ELEC),
]

assy = cq.Assembly()
for name, wp, color in PARTS:
    bb = wp.val().BoundingBox()
    assert bb.xmax - bb.xmin > 0.1 and bb.ymax - bb.ymin > 0.1 and bb.zmax - bb.zmin > 0.1, name
    assy.add(wp.val(), name=name, color=color)

for i, (text, x, y, z, edge, side) in enumerate(LABELS):
    shp = L(text, x, y, z, edge, side)
    assert shp.BoundingBox().xmax - shp.BoundingBox().xmin > 1.0, text
    assy.add(shp, name=f"label_{i:02d}_{text}", color=C_LABEL)

assy.save(OUT_STEP)
print("wrote", OUT_STEP)
for name, wp, _ in PARTS:
    bb = wp.val().BoundingBox()
    print(f"  {name:24s} X[{bb.xmin:7.1f},{bb.xmax:7.1f}] "
          f"Y[{bb.ymin:6.1f},{bb.ymax:6.1f}] Z[{bb.zmin:7.1f},{bb.zmax:7.1f}]")
print("  labels:", len(LABELS))
