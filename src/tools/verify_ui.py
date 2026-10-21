"""检查 SysClean 界面结构是否正确填充 (不截图, 直接读控件属性)。

比截图更可靠: 能读到列表实际行数、表头列数、状态栏文字。
需要先编译出 dist\\SysClean.exe。会弹 UAC, 点「是」即可。

    python src/tools/verify_ui.py

退出码 0 = PASS, 1 = FAIL。
"""
import ctypes, ctypes.wintypes as wt, time, sys, os
from ctypes import wintypes

user32 = ctypes.WinDLL('user32', use_last_error=True)
LVM_GETITEMCOUNT = 0x1000 + 4
LVM_GETHEADER = 0x1000 + 31
HDM_GETITEMCOUNT = 0x1200
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


def main():
    if not os.path.exists(EXE):
        sys.exit('not built yet: %s' % EXE)

    ctypes.windll.shell32.ShellExecuteW(None, 'runas', EXE, None, None, 1)
    time.sleep(5)

    mains = find_windows('SysCleanWnd')
    if not mains:
        sys.exit('main window did not appear')
    main_wnd = mains[0]
    print('main window  :', text(main_wnd))

    btns = [(text(k), k) for k in kids(main_wnd) if cls(k) == 'Button']
    print('main buttons : %d' % len(btns))
    for t, _ in btns:
        print('   -', t)

    target = [k for t, k in btns if '右键菜单' in t]
    if not target:
        print('\nRESULT: FAIL (no context-menu button)')
        user32.PostMessageW(main_wnd, 0x0010, 0, 0)
        return 1
    user32.SendMessageW(target[0], BM_CLICK, 0, 0)
    time.sleep(3)

    menus = find_windows('SysCleanMenuWnd')
    if not menus:
        print('\nRESULT: FAIL (menu window did not open)')
        user32.PostMessageW(main_wnd, 0x0010, 0, 0)
        return 1
    mw = menus[0]
    print('\nmenu window  :', text(mw))

    lv = [k for k in kids(mw) if cls(k) == 'SysListView32']
    if not lv:
        print('RESULT: FAIL (no listview)')
        user32.PostMessageW(mw, 0x0010, 0, 0)
        user32.PostMessageW(main_wnd, 0x0010, 0, 0)
        return 1

    rows = user32.SendMessageW(lv[0], LVM_GETITEMCOUNT, 0, 0)
    hdr = user32.SendMessageW(lv[0], LVM_GETHEADER, 0, 0)
    cols = user32.SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0)
    print('list rows    :', rows)
    print('list columns :', cols)

    for s in [text(k) for k in kids(mw) if cls(k) == 'Static' and text(k)]:
        print('status       :', s)

    mbtns = [text(k) for k in kids(mw) if cls(k) == 'Button']
    print('menu buttons :', mbtns)

    ok = rows > 0 and cols == 6 and len(mbtns) == 4 and len(btns) == 6
    print('\nRESULT:', 'PASS' if ok else 'FAIL')

    user32.PostMessageW(mw, 0x0010, 0, 0)
    time.sleep(0.5)
    user32.PostMessageW(main_wnd, 0x0010, 0, 0)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
