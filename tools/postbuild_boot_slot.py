# -*- coding: utf-8 -*-
"""
升级 boot 槽镜像后处理：把 Keil fromelf 输出的 .bin 组装成可直接烧录的
定长 24KB 槽镜像（含尾部 CRC32/size，供引导区校验）。

    用法：
        postbuild_boot_slot.py <输入.bin> [输出基名]
        postbuild_boot_slot.py --verify <槽.bin>

    槽布局（与下列三处必须逐字节一致，改这里 = 改协议）：
        apm32_e103_bootstrap/App/boot.c 的 boot_stage2_valid_v2()
        apm32_boot_coll/App/iap.c        的槽判据
        本文件

        [0, size)                 镜像本体（fromelf 输出，未填充）
        [size, 24564)             0xFF 填充
        [24564] (SLOT-12)         crc32(镜像本体)  小端
        [24568] (SLOT-8)          size             小端
        [24572, 24576)            0xFF

    失败时输出"全 0xFF 的无效槽"而不是保留上一次的合法产物 ——
    引导区会判 stage2 无效并停在 production park，好过默默烧进一个陈旧镜像。
"""

import os
import shutil
import struct
import sys
import zlib

SLOT_SIZE = 24576           # 24KB
CRC_OFF = SLOT_SIZE - 12    # 24564
SIZE_OFF = SLOT_SIZE - 8    # 24568
# 24572..24575 保持 0xFF —— 旧布局的整段 CRC 就放在最后 4 字节，
# 留成空白才能让引导区的 legacy 回退判据读到 0xFFFFFFFF 而拒绝，避免误判。
BASE_ADDR = 0x08003000

RAM_BASE = 0x20000000
RAM_TOP = 0x20020000

HEX_LINE = 32              # 每记录字节数


def die(msg, out_bin=None):
    """报错并写一个全 0xFF 的无效槽（避免留下过期产物）。"""
    print("[postbuild] ERROR: %s" % msg)
    if out_bin:
        try:
            with open(out_bin, "wb") as f:
                f.write(b"\xff" * SLOT_SIZE)
            print("[postbuild] wrote INVALID slot (all 0xFF): %s" % out_bin)
        except OSError:
            pass
    sys.exit(1)


def write_hex(path, data, base):
    """Intel HEX，基址 base。"""
    out = []
    for off in range(0, len(data), HEX_LINE):
        chunk = data[off:off + HEX_LINE]
        addr = base + off
        n = len(chunk)
        rec = [n, (addr >> 8) & 0xFF, addr & 0xFF, 0x00] + list(chunk)
        chk = (-sum(rec)) & 0xFF
        out.append(":%s%02X" % ("".join("%02X" % b for b in rec), chk))
    out.append(":00000001FF")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def verify(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) != SLOT_SIZE:
        print("[verify] FAIL: size %d, expect %d" % (len(blob), SLOT_SIZE))
        return 1
    size = struct.unpack_from("<I", blob, SIZE_OFF)[0]
    want = struct.unpack_from("<I", blob, CRC_OFF)[0]
    if size == 0 or size == 0xFFFFFFFF or size > CRC_OFF:
        print("[verify] FAIL: bad size field 0x%08X" % size)
        return 1
    got = zlib.crc32(blob[:size]) & 0xFFFFFFFF
    if got != want:
        print("[verify] FAIL: crc 0x%08X != 0x%08X" % (got, want))
        return 1
    print("[verify] OK  size=%d crc=0x%08X slot=%d" % (size, got, len(blob)))
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "--verify":
        return verify(argv[2])

    if len(argv) < 2:
        print(__doc__)
        return 2

    src = argv[1]
    base = argv[2] if len(argv) >= 3 else os.path.splitext(src)[0]
    out_bin = base + "_slot.bin"
    out_hex = base + "_slot.hex"

    try:
        with open(src, "rb") as f:
            image = f.read()
    except OSError as e:
        die("cannot read %s (%s)" % (src, e), out_bin)

    if len(image) == 0:
        die("input image is empty", out_bin)
    if len(image) > CRC_OFF:
        die("image %d bytes exceeds slot payload %d" % (len(image), CRC_OFF), out_bin)

    # ---- 载体自检：拦住"喂错文件 / 链接基址改了"这一整类事故 ----
    sp, pc = struct.unpack_from("<II", image, 0)
    if not (RAM_BASE <= sp <= RAM_TOP):
        die("vector SP 0x%08X not in RAM [0x%08X,0x%08X]" % (sp, RAM_BASE, RAM_TOP), out_bin)
    if not (BASE_ADDR <= pc < BASE_ADDR + CRC_OFF):
        die("vector PC 0x%08X not in slot [0x%08X,0x%08X)"
            % (pc, BASE_ADDR, BASE_ADDR + CRC_OFF), out_bin)
    if (pc & 1) == 0:
        die("vector PC 0x%08X has Thumb bit clear" % pc, out_bin)

    # ---- 组装 ----
    blob = bytearray(b"\xff" * SLOT_SIZE)
    blob[:len(image)] = image
    crc = zlib.crc32(image) & 0xFFFFFFFF
    struct.pack_into("<I", blob, CRC_OFF, crc)
    struct.pack_into("<I", blob, SIZE_OFF, len(image))

    try:
        with open(out_bin, "wb") as f:
            f.write(blob)
        write_hex(out_hex, blob, BASE_ADDR)
    except OSError as e:
        die("cannot write output (%s)" % e)

    # 把原始 .bin 挪进 out/raw/ —— out/ 下只留可烧录的产物。
    # 这是为了防"拿错文件"：apm32e103_boot.bin 没有 CRC 尾，烧进槽里
    # 引导区会判无效（size/crc 字段读到 0xFFFFFFFF），直接停 production mode。
    # 已发生两次，所以从物理上把它移出视野。
    raw_dir = os.path.join(os.path.dirname(os.path.abspath(out_bin)), "raw")
    try:
        os.makedirs(raw_dir, exist_ok=True)
        shutil.move(src, os.path.join(raw_dir, os.path.basename(src)))
    except OSError as e:
        print("[postbuild] WARN: cannot archive raw bin (%s)" % e)

    # Keil 构建窗口是 GBK，保持纯 ASCII 输出
    print("[postbuild] app=%dB crc=0x%08X slot=%dB -> %s (raw -> out/raw/)"
          % (len(image), crc, SLOT_SIZE, os.path.basename(out_bin)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
