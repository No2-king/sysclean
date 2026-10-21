"""截取三个标签页, 存成 PNG。

用途: 更新 README 里的界面截图。需要先编译出 dist\\SysClean.exe。
会弹 UAC, 点「是」即可。截图完成后窗口自动关闭。

    python src/tools/shot_all.py docs/preview.png docs/preview_proc.png docs/preview_menu.png

三个参数分别是: 自身功能页 / 进程管理页 / 右键菜单页。
标签栏的三个标签是父窗口自绘的, 没有子控件, 所以用 WM_LBUTTONDOWN 直接点坐标。
"""
import ctypes, ctypes.wintypes as wt, time, zlib, struct, sys, os
from ctypes import wintypes

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.windll.gdi32
PW_RENDERFULLCONTENT = 2

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', '..', 'dist', 'SysClean.exe')

# 与源码 LayoutControls 保持一致: 标签从 x=250 开始, 宽 110, 间距 8, 高 34, 顶栏 76
TAB_X0, TAB_W, TAB_GAP, TAB_CY = 250, 110, 8, 38


def cls(h):
    b = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(h, b, 128)
    return b.value


def find_windows(c):
    out = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if cls(h) == c:
            out.append(h)
        return True

    user32.EnumWindows(cb, 0)
    return out


def click_tab(hwnd, idx):
    """点第 idx 个标签 (0/1/2)"""
    x = TAB_X0 + idx * (TAB_W + TAB_GAP) + TAB_W // 2
    lp = (TAB_CY << 16) | x
    user32.SendMessageW(hwnd, 0x0201, 1, lp)   # WM_LBUTTONDOWN
    user32.SendMessageW(hwnd, 0x0202, 0, lp)   # WM_LBUTTONUP


def shoot(hwnd, out):
    rc = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    hdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    # PW_RENDERFULLCONTENT: 窗口被遮挡时也能拿到正确内容
    user32.PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT)

    class BI(ctypes.Structure):
        _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG),
                    ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                    ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                    ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                    ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                    ('biClrImportant', wintypes.DWORD)]

    bi = BI()
    bi.biSize = ctypes.sizeof(BI)
    bi.biWidth, bi.biHeight, bi.biPlanes, bi.biBitCount = w, -h, 1, 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    # GetDIBits 给的是 BGRA, PNG 要 RGBA; 每行前面还得加一个 filter 字节 (0 = None)
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        row = bytearray(buf[y * stride:(y + 1) * stride])
        row[0::4], row[2::4] = row[2::4], row[0::4]
        raw.append(0)
        raw += row
    raw = bytes(raw)

    def chunk(t, d):
        return (struct.pack('>I', len(d)) + t + d
                + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    open(out, 'wb').write(png)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    print('saved', out, w, 'x', h)


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: shot_all.py <self.png> [proc.png] [menu.png]')
    if not os.path.exists(EXE):
        sys.exit('not built yet: %s' % EXE)

    ctypes.windll.shell32.ShellExecuteW(None, 'runas', EXE, '/noglass', None, 1)
    time.sleep(6)

    mains = find_windows('SysCleanWnd')
    if not mains:
        sys.exit('main window did not appear')
    w = mains[0]
    user32.SetForegroundWindow(w)
    time.sleep(1.2)          # 等首次扫描把列表填上

    click_tab(w, 0)          # 显式回到第 1 页, 防止启动时的残留点击落在标签栏
    time.sleep(0.8)
    shoot(w, sys.argv[1])    # 第 1 页: 自身功能

    if len(sys.argv) > 2:
        click_tab(w, 1)      # 进程管理
        time.sleep(2.5)      # 首次进入要枚举全部进程/线程
        shoot(w, sys.argv[2])

    if len(sys.argv) > 3:
        click_tab(w, 2)      # 右键菜单
        time.sleep(2.5)      # 首次进入要扫 HKCR
        shoot(w, sys.argv[3])

    user32.PostMessageW(w, 0x0010, 0, 0)


if __name__ == '__main__':
    main()
