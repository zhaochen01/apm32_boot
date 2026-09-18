#!/usr/bin/env python3
"""
serial_dl.py —— 通过调试口 (USART1) 给 IoT-4G 刷 APP 固件。

用法：
    python serial_dl.py --port COM3 --file ../../IoT-4G-v2/out/IoT-4G-V2-Debug.bin

    ⚠ 先开本工具，再给设备上电（或按复位）。工具会持续发触发魔数和请求行，
      覆盖"操作员还没复位"的时序。设备已经进串口模式时直接跑也可以。

协议（设备侧实现在 apm32_boot_coll/App/{serial_dl.c,upgrade_ctrl.c}）：

    PC  "#DL <size> <crc32>\r\n"  ──►
                                ◄──  "#RDY\r\n"    已进下载模式，停发请求行
        （设备在把当前 APP 备份到 B 槽，满槽约 7s）
                                ◄──  "#GO\r\n"     可以发数据了
    PC  第 0 页 (≤2048B)         ──►
                                ◄──  ACK(0x06)
    ...  每页等一个 ACK ...
    PC  末页 (可不足 2048B)      ──►
                                ◄──  ACK(0x06)=成功并复位 / NAK(0x15)=失败

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

# 设备的控制信号一律带 '#' 前缀。本工具的接收窗口里混着设备日志
# （先开工具、再给板子复位，复位前 APP 还在打日志），裸 "RDY" 会被
# "[.. 4G] RDY confirmed, proceed to BASIC_AT" 这类日志行误命中 ——
# 实测就是这么失败过一次：工具提前收工停发魔数，复位时引导区啥也没收到。
RDY = b'#RDY'
GO = b'#GO'

# 设备在收到请求行之前不会回话（引导区侦听窗口 150ms + preserve 最长 7s），
# 所以 RDY 要给足时间：冷启动时工具可能在设备上电前就跑起来了。
T_RDY = 25.0
T_GO = 40.0
T_ACK = 10.0


class Fail(Exception):
    pass


class Dev:
    """串口 + "设备日志原样转发"的最小封装。"""

    def __init__(self, port, baud):
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.s.reset_input_buffer()
        self.buf = bytearray()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

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


def send_request_until_rdy(dev, req, timeout):
    """持续重发请求行（+ 触发魔数）直到收到 RDY。期间设备可能在打日志。"""
    end = time.time() + timeout
    nxt = 0.0
    while time.time() < end:
        now = time.time()
        if now >= nxt:
            dev.write(MAGIC + req)
            nxt = now + 0.1          # 间隔 100ms，与引导区 150ms 窗口配套

        dev._pump()
        line = dev._take_line()
        if line is None:
            time.sleep(0.01)
            continue
        if RDY in line:
            dev.log_raw(line)
            return True
        dev.log_raw(line)
    return False


def main():
    ap = argparse.ArgumentParser(
        description='IoT-4G 串口下载（USART1 调试口）',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='示例：python serial_dl.py --port COM3 --file ..\\..\\IoT-4G-v2\\out\\IoT-4G-V2-Debug.bin')
    ap.add_argument('--port', required=True, help='调试口串口号，如 COM3')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--file', required=True, help='要刷入的 .bin（APP 镜像，链接地址 0x0800F000）')
    args = ap.parse_args()

    with io.open(args.file, 'rb') as f:
        img = f.read()

    if len(img) == 0:
        sys.exit('镜像为空')
    if len(img) > 0x35800:
        sys.exit('镜像 %d 字节超过 APP 槽上限 219136' % len(img))

    size = len(img)
    crc = zlib.crc32(img) & 0xFFFFFFFF
    req = ('#DL %d %08x\r\n' % (size, crc)).encode('ascii')

    print('镜像 : %s' % args.file)
    print('大小 : %d 字节 (%d 页)' % (size, (size + PAGE - 1) // PAGE))
    print('CRC32: %08X' % crc)
    print('端口 : %s @ %d' % (args.port, args.baud))
    print('—— 若设备还没上电，现在给它上电/按复位 ——')

    dev = Dev(args.port, args.baud)
    try:
        if not send_request_until_rdy(dev, req, T_RDY):
            raise Fail('没等到 #RDY（%ds）。检查：设备是否已上电、串口是否被别的程序占用' % T_RDY)
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
        time.sleep(1.0)
        dev._pump()
        dev.log_raw(bytes(dev.buf))
        dev.buf.clear()

    except Fail as e:
        print('\n!! 失败：%s' % e)
        dev._pump()
        dev.log_raw(bytes(dev.buf))
        return 1

    except KeyboardInterrupt:
        print('\n!! 已中断。设备侧运行区可能半写 —— 复位后引导区会判 A 无效并回退到 B 槽。')
        return 1

    finally:
        dev.close()

    print('>> 成功。设备已复位，接下来应看到 [bootstrap] ... 和 APP 启动日志。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
