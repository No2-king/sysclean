"""截取主窗口 + 右键菜单窗口, 存成 PNG。

用途: 更新 README 里的界面截图。需要先编译出 dist\\SysClean.exe。
会弹 UAC, 点「是」即可。截图完成后窗口自动关闭。

    python src/tools/shot_all.py docs/preview.png docs/preview_menu.png
"""
import ctypes, ctypes.wintypes as wt, time, zlib, struct, sys, os
from ctypes import wintypes

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.windll.gdi32
PW_RENDERFULLCONTENT = 2
BM_CLICK = 0x00F5

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', '..', 'dist', 'SysClean.exe')


def cls(h):
    b = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(h, b, 128)
    return b.value


def text(h):
    b = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(h, b, 512)
    return b.value


def kids(hwnd):
    out = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        out.append(h)
        return True

    user32.EnumChildWindows(hwnd, cb, 0)
    return out


def find_windows(c):
    out = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        if cls(h) == c:
            out.append(h)
        return True

    user32.EnumWindows(cb, 0)
    return out


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
    raw = b''.join(buf[y * w * 4:(y + 1) * w * 4] for y in range(h))

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
    if len(sys.argv) < 3:
        sys.exit('usage: shot_all.py <main.png> <menu.png>')
    if not os.path.exists(EXE):
        sys.exit('not built yet: %s' % EXE)

    ctypes.windll.shell32.ShellExecuteW(None, 'runas', EXE, None, None, 1)
    time.sleep(6)

    mains = find_windows('SysCleanWnd')
    if not mains:
        sys.exit('main window did not appear')
    main_wnd = mains[0]
    user32.SetForegroundWindow(main_wnd)
    time.sleep(0.5)
    shoot(main_wnd, sys.argv[1])

    btn = [k for k in kids(main_wnd) if cls(k) == 'Button' and '右键菜单' in text(k)]
    if btn:
        user32.SendMessageW(btn[0], BM_CLICK, 0, 0)
        time.sleep(3)
        menus = find_windows('SysCleanMenuWnd')
        if menus:
            user32.SetForegroundWindow(menus[0])
            time.sleep(0.5)
            shoot(menus[0], sys.argv[2])
            user32.PostMessageW(menus[0], 0x0010, 0, 0)
    user32.PostMessageW(main_wnd, 0x0010, 0, 0)


if __name__ == '__main__':
    main()
