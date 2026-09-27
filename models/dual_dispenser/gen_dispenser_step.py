"""
双组分出丝器（A/B 1:1）装配模型 -> STEP

用途：挤出双组分聚脲丝，用于悬挂 / 固定**小物件**。
边界：不载人、不承人体重量、不做自制压力腔。
     所有承压与流体接触件均为采购成品（料筒、汇流块、静态混合管、喷嘴、震动泵），
     本模型只给出它们的外部包络与安装尺寸；内部通道一律画成**盲孔**，不是可加工流道。

单位 mm。坐标：X = 出料方向（喷嘴在 +X），Y = A/B 并排方向，Z = 上。

运行：python models/dual_dispenser/gen_dispenser_step.py
"""

import cadquery as cq

OUT_STEP = "models/dual_dispenser/dual_dispenser.step"

C_PRINT = cq.Color(0.62, 0.64, 0.67)     # 打印件：机架、握把、护罩
C_BUY = cq.Color(0.95, 0.72, 0.20)       # 采购件：料筒、混合管、喷嘴
C_PUMP = cq.Color(0.30, 0.55, 0.85)      # 采购件：震动泵


# ---------- 基础体：沿 X 的圆柱 / 管 / 锥 ----------

def cyl_x(r, x0, x1, y=0.0, z=0.0):
    wp = cq.Workplane("YZ").center(y, z).circle(r).extrude(x1 - x0)
    return wp.translate((x0, 0, 0))


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


def cyl_z(r, z0, z1, x=0.0, y=0.0):
    wp = cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(r).extrude(z1 - z0)
    return wp


def uni(*parts):
    res = parts[0]
    for p in parts[1:]:
        res = res.union(p.val())
    return res


# ---------- 采购件：A/B 料筒（50ml，1:1）----------

CARTRIDGE_R = 13.0
CARTRIDGE_Y = 15.0
CARTRIDGE_X0, CARTRIDGE_X1 = 0.0, 120.0

cart_a = cyl_x(CARTRIDGE_R, CARTRIDGE_X0, CARTRIDGE_X1, y=+CARTRIDGE_Y)
cart_b = cyl_x(CARTRIDGE_R, CARTRIDGE_X0, CARTRIDGE_X1, y=-CARTRIDGE_Y)
cap_a = cyl_x(15.5, -8.0, 0.0, y=+CARTRIDGE_Y)
cap_b = cyl_x(15.5, -8.0, 0.0, y=-CARTRIDGE_Y)
cartridges = uni(cart_a, cart_b, cap_a, cap_b)

# 出口颈：从料筒端部收拢到汇流块（采购件随料筒供货）
neck_a = cone_x(6.5, 5.0, 120.0, 134.0, y1=+CARTRIDGE_Y, y2=+5.0)
neck_b = cone_x(6.5, 5.0, 120.0, 134.0, y1=-CARTRIDGE_Y, y2=-5.0)

# ---------- 采购件：汇流块（盲孔包络，非可加工流道）----------

manifold = box(149.0, 0.0, 0.0, 30.0, 46.0, 34.0)
manifold = manifold.cut(cyl_x(6.0, 134.0, 145.0, y=+5.0).val())   # 进胶盲孔
manifold = manifold.cut(cyl_x(6.0, 134.0, 145.0, y=-5.0).val())
manifold = manifold.cut(cyl_x(6.0, 152.0, 164.0, y=0.0).val())    # 出胶盲孔，与进胶孔留 7mm 实体

# ---------- 采购件：静态混合管 + 喷嘴 ----------

mixer_flange = cyl_x(6.5, 164.0, 172.0)
mixer_tube = cyl_x(4.5, 172.0, 272.0)
mixer = uni(mixer_flange, mixer_tube)

nozzle = cone_x(4.5, 1.2, 272.0, 292.0)

# ---------- 采购件：震动泵（只做包络 + 接口凸台）----------

PUMP_Z = -42.0
pump_body = cyl_x(21.0, 20.0, 115.0, z=PUMP_Z)
pump_coil = cyl_x(24.0, 45.0, 85.0, z=PUMP_Z)
pump_in = cyl_z(6.0, PUMP_Z - 16.0, PUMP_Z, x=28.0)
pump_out = cyl_x(6.0, 115.0, 127.0, z=PUMP_Z)
pump = uni(pump_body, pump_coil, pump_in, pump_out)

# ---------- 打印件：机架 ----------

base = box(60.0, 0.0, -15.5, 170.0, 62.0, 5.0)
rear_bulk = box(-4.0, 0.0, -8.0, 8.0, 62.0, 44.0)
front_bulk = box(116.0, 0.0, -8.0, 8.0, 62.0, 44.0)
for y in (+CARTRIDGE_Y, -CARTRIDGE_Y):
    rear_bulk = rear_bulk.cut(cyl_x(16.0, -10.0, 2.0, y=y).val())
    front_bulk = front_bulk.cut(cyl_x(14.0, 110.0, 122.0, y=y).val())

# 夹环必须落在泵体段（X20-45 / X85-115），避开线圈段 X45-85
ring_rear = tube_x(27.0, 21.5, 28.0, 40.0, z=PUMP_Z)
ring_front = tube_x(27.0, 21.5, 90.0, 102.0, z=PUMP_Z)
post_rear = box(34.0, 0.0, -34.0, 12.0, 26.0, 26.0)
post_front = box(96.0, 0.0, -34.0, 12.0, 26.0, 26.0)

frame = uni(base, rear_bulk, front_bulk, ring_rear, ring_front, post_rear, post_front)

# ---------- 打印件：混合管护罩（非承压，防折）----------

guard = tube_x(10.0, 6.5, 196.0, 256.0)
guard_arm = box(171.0, 0.0, -13.0, 62.0, 8.0, 6.0)   # 撑回机架，避免护罩悬空
guard = uni(guard, guard_arm)

# ---------- 打印件：握把 + 扳机（扳机只驱动泵的电器开关，非阀）----------

grip = box(45.0, 0.0, -60.0, 50.0, 34.0, 85.0)
grip = grip.rotate((45.0, 0.0, -18.0), (45.0, 1.0, -18.0), 12.0)

trigger = box(72.0, 0.0, -50.0, 10.0, 18.0, 34.0)
trigger = trigger.rotate((72.0, 0.0, -34.0), (72.0, 1.0, -34.0), 20.0)

# ---------- 采购件：电池包（包络）----------

battery = box(-25.0, 0.0, -60.0, 55.0, 42.0, 30.0)

# ---------- 导出 ----------

PARTS = [
    ("frame_printed", frame, C_PRINT),
    ("grip_printed", grip, C_PRINT),
    ("trigger_printed", trigger, C_PRINT),
    ("mixer_guard_printed", guard, C_PRINT),
    ("cartridges_AB_buy", cartridges, C_BUY),
    ("outlet_necks_buy", uni(neck_a, neck_b), C_BUY),
    ("manifold_buy", manifold, C_BUY),
    ("static_mixer_buy", mixer, C_BUY),
    ("nozzle_buy", nozzle, C_BUY),
    ("vibration_pump_buy", pump, C_PUMP),
    ("battery_buy", battery, C_PUMP),
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
