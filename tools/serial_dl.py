#!/usr/bin/env python3
"""
serial_dl.py —— 通过调试口 (USART1) 给 IoT-4G 刷 APP 固件。

用法：
    python serial_dl.py --port COM3 --file ../../IoT-4G-v2/out/IoT-4G-V2-Debug.bin
    python serial_dl.py --port COM3 --file <app.bin> --baud 921600   # 约 3 倍速

    ⚠ 先开本工具，再给设备上电（或按复位）。工具会持续发触发魔数和请求行，
      覆盖"操作员还没复位"的时序。设备已经进串口模式时直接跑也可以。

协议（设备侧实现在 apm32_boot_coll/App/{serial_dl.c,upgrade_ctrl.c}）：

    PC  "#DL <size> <crc32> [baud]\r\n"  ──►            （每 100ms 重发）
                                ◄──  "#RDY\r\n"    已进下载模式，停发请求行
    双方切到 baud（不指定则不切），PC 在新波特率重发同一行作为确认 ──►
                                ◄──  "#SYNC\r\n"    链路双向已确认
        （设备在把当前 APP 备份到 B 槽，满槽约 7s）
                                ◄──  "#GO\r\n"      可以发数据了
    PC  第 0 页 (≤2048B)         ──►
                                ◄──  ACK(0x06)
    ...  每页等一个 ACK ...
    PC  末页 (可不足 2048B)      ──►
                                ◄──  ACK(0x06)=成功并复位 / NAK(0x15)=失败

波特率（--baud，缺省 921600 = 最高档）：串口刷机的瓶颈是传输本身 —— 实测
恒定 8.6KB/s，提到 921600 后整轮快约 3 倍（Flash 擦写变成新瓶颈）。

**新旧两种 boot 都能刷**，靠"先按高档谈、谈不拢自动降级"：
  新 boot 认三段式 → 立刻回 #RDY → 双方切档 → #SYNC
  旧 boot 不认三段式（永远不响应）→ 见到设备说话后 4 秒自动改用两段式
    → 照旧 115200 刷完（只多等那 4 秒，`--baud 115200` 可免掉）

⚠ 握手阶段【恒为 115200】：引导区的 150ms 魔数侦听、以及旧版 boot 的请求
  行解析都在 115200 上，切档只发生在会话内部（收到 #RDY 之后），且设备
  不落盘 —— 任何复位都会回到 115200，旧工具/旧固件零影响。
⚠ 必须【收到 #RDY 就切】，不能等 #GO：设备在 #RDY 之后的所有输出
  （preserve 日志、#GO）都是新波特率，用旧波特率等只会永远等不到。
⚠ 本文件与 C# 版 SerialDlGui/SerialDlClient.cs 必须同策略（跨工程协议不变量）。

依赖：pyserial。CRC 用 zlib.crc32 —— 与设备侧 CRC32() 同为标准 IEEE
802.3 反射算法（poly 0xEDB88320，初值/末值取反），两边必须一致。
"""

import argparse
import io
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：pip install pyserial")

MAGIC = b'\x55\xAA'
PAGE = 2048
ACK = 0x06
NAK = 0x15

BAUD_HANDSHAKE = 115200          # 握手阶段固定值：引导区魔数侦听与 #RDY 都在这档
# 与设备侧 serial_dl.c 的 serial_dl_check_baud() 白名单必须一致。
# 设备对表外的值会静默回落 115200，而那时双方对不上 —— 这里直接拦住。
BAUD_WHITELIST = (115200, 230400, 460800, 921600)

# 设备的控制信号一律带 '#' 前缀。本工具的接收窗口里混着设备日志
# （先开工具、再给板子复位，复位前 APP 还在打日志），裸 "RDY" 会被
# "[.. 4G] RDY confirmed, proceed to BASIC_AT" 这类日志行误命中 ——
# 实测就是这么失败过一次：工具提前收工停发魔数，复位时引导区啥也没收到。
RDY = b'#RDY'
GO = b'#GO'
SYNC = b'#SYNC'                  # 切档成功（双向确认）

# 设备在收到请求行之前不会回话（引导区侦听窗口 150ms + preserve 最长 7s），
# 所以 RDY 要给足时间：冷启动时工具可能在设备上电前就跑起来了。
T_RDY = 25.0
T_GO = 40.0
T_ACK = 10.0
# 等 #SYNC：设备侧等确认行的上限是 2s，这里留一点余量。给得比设备短就
# 会在设备还在等确认行时报"切档失败"，把一次本可成功的刷机打断。
T_SYNC = 5.0

# 见到设备说话之后再等这么久还没 #RDY，就认定"旧版 boot"（它不认三段式
# 请求行，永远不会响应）并降级成两段式。与 C# 版 SerialDlClient.TFallback 同值。
# 新 boot 从启动横幅到"在等请求行"实测 1s 量级，4s 余量充足。
FALLBACK_S = 4.0


class Fail(Exception):
    pass


class Dev:
    """串口 + "设备日志原样转发"的最小封装。"""

    def __init__(self, port):
        # 恒定以握手波特率打开：引导区魔数侦听与 #RDY 都在这一档，
        # 切档只发生在收到 #RDY 之后（见 set_baud）。
        self.s = serial.Serial(port, BAUD_HANDSHAKE, timeout=0.05)
        self.s.reset_input_buffer()
        self.buf = bytearray()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def set_baud(self, baud):
        """切端口波特率。必须清接收缓冲 —— 切换前后在途的字节都是错帧。

        不清的话，那些字节会和切换后的正常数据拼在一起，把 #GO/ACK 的
        匹配搅乱（按页 ACK 的协议里错位一个字节就等于整包报废）。"""
        self.s.baudrate = baud
        try:
            self.s.reset_input_buffer()
            self.s.reset_output_buffer()
        except Exception:
            pass
        self.buf.clear()

    def dump_log(self):
        """把当前缓冲里的设备日志吐出来并清空。"""
        self._pump()
        if self.buf:
            self.log_raw(bytes(self.buf))
            self.buf.clear()

    # -- 原始字节 --
    def _pump(self):
        n = self.s.in_waiting
        if n:
            self.buf += self.s.read(n)
        return len(self.buf)

    def _take_line(self):
        """从缓冲里取出一整行（\\n 结尾）。没有则返回 None。"""
        i = self.buf.find(b'\n')
        if i < 0:
            return None
        line = bytes(self.buf[:i + 1])
        del self.buf[:i + 1]
        return line

    def log_raw(self, data):
        """设备日志原样透传。

        走 sys.stdout.buffer（二进制）而不是 sys.stdout：控制台编码是 GBK，
        而设备日志里既有 UTF-8 中文、也可能有半截多字节序列，解码成 str 再
        用 GBK 编码会直接抛 UnicodeEncodeError 把整个下载打断。"""
        sys.stdout.flush()
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    def wait_token(self, token, timeout, fail_tokens=()):
        """等 token 出现，沿途把设备日志原样打出来。返回 True/False。

        fail_tokens 命中即抛 Fail —— 设备放弃时（如 preserve 失败回的
        "ERR preserve"）不该让操作员白等满超时。"""
        end = time.time() + timeout
        while time.time() < end:
            self._pump()
            line = self._take_line()
            if line is None:
                time.sleep(0.01)
                continue
            self.log_raw(line)
            if any(t in line for t in fail_tokens):
                raise Fail('设备回了 %s' % line.strip().decode('ascii', 'replace'))
            if token in line:
                return True
        return False

    def read_byte(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self._pump()
            if self.buf:
                b = self.buf[0]
                del self.buf[0]
                return b
            time.sleep(0.005)
        return None

    def write(self, data):
        self.s.write(data)


def send_request_until_rdy(dev, fast_req, slow_req, timeout, dl_baud):
    """持续重发请求行（+ 触发魔数）直到收到 RDY。期间设备可能在打日志。

    **新旧两种 boot 都要能刷**，靠"先按高档谈、谈不拢自动降级"：
    新 boot 认三段式（fast_req）→ 立刻回 #RDY；旧 boot 的请求行状态机
    根本不认第三个字段（crc 后见空格就重新找 '#'），对三段式永远不响应 ——
    所以见到设备说话之后再等 FALLBACK_S 秒没等到 #RDY，就改用老的两段式
    （slow_req），照旧 115200 刷完。

    ⚠ 降级计时【必须从"见到设备说话"起算】，不能从函数开始起算：
    本工具的用法是"先开工具、再给板子上电"，板子可能 20 秒后才出现；
    从开始就计时的话，新 boot 上电时看到的已经是降级后的请求行了。

    返回 True = 设备接受的是三段式（调用方随后切档并等 #SYNC）；
    False = 已在用两段式；None = 超时没等到。
    """
    end = time.time() + timeout
    nxt = 0.0
    use_fast = fast_req is not None
    downgrade_at = None          # 见到设备说话之后才开始计时
    while time.time() < end:
        now = time.time()

        if use_fast and (downgrade_at is not None) and (now >= downgrade_at):
            use_fast = False
            print('>> 设备未响应高速请求（旧版 boot？），已自动回落 115200')

        if now >= nxt:
            dev.write(MAGIC + (fast_req if use_fast else slow_req))
            nxt = now + 0.1          # 间隔 100ms，与引导区 150ms 窗口配套

        dev._pump()
        line = dev._take_line()
        if line is None:
            time.sleep(0.01)
            continue
        dev.log_raw(line)
        if RDY in line:
            if use_fast:
                # 收到 RDY 就地切档并重发请求行作为确认 —— 【不能等 #GO 再切】：
                # 设备在 #RDY 之后的一切输出（preserve 日志、#GO）都是新波特率。
                #
                # 【只发一条】原先是每 100ms 发 3 条，但设备收下第一条就往下走了 ——
                # 后两条晚到 0~200ms，正好落进紧接着的收页窗口，被 DMA 当成【页首
                # 数据】写进 flash，整条字节流错位（2026-10-15 实测：页 0 = 一条确认行
                # + 镜像前 2018 字节，也正是"115200 能刷、230400+ 必失败"的原因）。
                # 设备侧现在会等 350ms 再 flush，两边都堵上。
                dev.set_baud(dl_baud)
                dev.write(MAGIC + fast_req)
            return use_fast
        if downgrade_at is None:
            downgrade_at = now + FALLBACK_S   # 设备还活着（刚打了一行日志）
    return None


def main():
    ap = argparse.ArgumentParser(
        description='IoT-4G 串口下载（USART1 调试口）',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='示例：python serial_dl.py --port COM3 --file ..\\..\\IoT-4G-v2\\out\\IoT-4G-V2-Debug.bin\n'
               '      python serial_dl.py --port COM3 --file <app.bin> --baud 921600')
    ap.add_argument('--port', required=True, help='调试口串口号，如 COM3')
    ap.add_argument('--baud', type=int, default=BAUD_WHITELIST[-1],
                    help='下载阶段波特率上限（握手恒为 %d）；可选 %s。'
                         '设备是旧版 boot 时自动回落 115200；想去掉那 4 秒回落等待就显式给 115200'
                         % (BAUD_HANDSHAKE, '/'.join(str(b) for b in BAUD_WHITELIST)))
    ap.add_argument('--file', required=True, help='要刷入的 .bin（APP 镜像，链接地址 0x0800F000）')
    args = ap.parse_args()

    if args.baud not in BAUD_WHITELIST:
        sys.exit('--baud %d 不在白名单 %s 内。设备对表外的值会回落 115200，'
                 '双方对不上会刷机失败，所以这里直接拦住。'
                 % (args.baud, '/'.join(str(b) for b in BAUD_WHITELIST)))
    dl_baud = args.baud

    with io.open(args.file, 'rb') as f:
        img = f.read()

    if len(img) == 0:
        sys.exit('镜像为空')
    if len(img) > 0x35800:
        sys.exit('镜像 %d 字节超过 APP 槽上限 219136' % len(img))

    size = len(img)
    crc = zlib.crc32(img) & 0xFFFFFFFF
    # 两段式是老格式（旧 boot 唯一认的），三段式带波特率字段（新 boot 认）。
    # fast_req=None 表示本轮不协商（选了 115200）。
    slow_req = ('#DL %d %08x\r\n' % (size, crc)).encode('ascii')
    fast_req = None
    if dl_baud > BAUD_HANDSHAKE:
        fast_req = ('#DL %d %08x %d\r\n' % (size, crc, dl_baud)).encode('ascii')

    print('镜像 : %s' % args.file)
    print('大小 : %d 字节 (%d 页)' % (size, (size + PAGE - 1) // PAGE))
    print('CRC32: %08X' % crc)
    print('端口 : %s (握手 @%d，下载 @%d)' % (args.port, BAUD_HANDSHAKE, dl_baud))
    if fast_req is None:
        print('提示 : 下载波特率选的是 115200，本轮不协商提速（实测 8.6 KB/s）')
    print('—— 若设备还没上电，现在给它上电/按复位 ——')

    dev = Dev(args.port)
    try:
        res = send_request_until_rdy(dev, fast_req, slow_req, T_RDY, dl_baud)
        if res is None:
            raise Fail('没等到 #RDY（%ds）。检查：设备是否已上电、串口是否被别的程序占用' % T_RDY)
        if res:
            if not dev.wait_token(SYNC, T_SYNC, fail_tokens=(b'#ERR',)):
                raise Fail('切档到 %d 未确认（%ds）。多半是这条 USB 转串口撑不住这个波特率 ——'
                           '用 --baud 115200 重试' % (dl_baud, T_SYNC))
            print('>> 已切到 %d（高速下载）' % dl_baud)
        else:
            print('>> 按 %d 下载' % BAUD_HANDSHAKE)
        print('>> 设备已进下载模式，正在备份当前固件到 B 槽...')

        if not dev.wait_token(GO, T_GO, fail_tokens=(b'#ERR',)):
            raise Fail('没等到 #GO（%ds）。设备可能在备份阶段失败，看上面的设备日志' % T_GO)
        print('>> 开始传输')

        total_pages = (size + PAGE - 1) // PAGE
        dev.write(img[0:PAGE])                       # 先发第 0 页，避免空等
        t0 = time.time()

        for i in range(total_pages):
            b = dev.read_byte(T_ACK)
            if b is None:
                raise Fail('第 %d/%d 页等 ACK 超时' % (i + 1, total_pages))
            if b == NAK:
                raise Fail('设备回 NAK，第 %d/%d 页 —— 看设备日志里的 [serial] 行' % (i + 1, total_pages))
            if b != ACK:
                raise Fail('第 %d/%d 页收到意外字节 0x%02X（协议错位）' % (i + 1, total_pages, b))

            sys.stdout.write('\r  %d/%d 页' % (i + 1, total_pages))
            sys.stdout.flush()

            off = (i + 1) * PAGE
            if off < size:
                dev.write(img[off:off + PAGE])

        dt = time.time() - t0
        print('\n>> 传输完成，%.1fs (%.1f KB/s)' % (dt, size / 1024.0 / max(dt, 0.001)))
        print('>> 设备正在回读校验并提交...')
        time.sleep(0.6)
        dev.dump_log()                  # 提交日志（仍是新波特率）
        # 设备提交后复位 -> 引导区重新以 115200 初始化 USART1，之后的日志
        # 全在 115200 上；不切回来只能读到乱码。
        dev.set_baud(BAUD_HANDSHAKE)
        time.sleep(1.0)
        dev.dump_log()                  # 引导区 / APP 启动日志

    except Fail as e:
        print('\n!! 失败：%s' % e)
        dev.dump_log()
        return 1

    except KeyboardInterrupt:
        print('\n!! 已中断。设备侧运行区可能半写 —— 复位后引导区会判 A 无效并回退到 B 槽。')
        return 1

    finally:
        # 【先抓日志，再切波特率】—— 顺序不能反：失败时设备可能正用高波特率打
        # 错误详情（"[serial] rx timeout at off=… got=… ore=…"），那些字节在切回
        # 115200 之后才读就是一堆乱码（实测只看到 "!   DV 1 DV"）。
        time.sleep(0.2)
        dev.dump_log()
        # 设备多半已复位（= 回到 115200），而端口还停在新档：切回去，否则下一个
        # 打开这个口的工具（串口助手之类）看到的是乱码。
        dev.set_baud(BAUD_HANDSHAKE)
        time.sleep(0.3)
        dev.dump_log()
        dev.close()

    print('>> 成功。设备已复位，接下来应看到 [bootstrap] ... 和 APP 启动日志。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
