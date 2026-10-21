"""检查 SysClean 三标签页界面结构是否正确 (不截图, 直接读控件属性)。

比截图更可靠: 能读到列表实际行数、表头列数、按钮位置、控件是否越界。
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
WM_CLOSE = 0x0010
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', '..', 'dist', 'SysClean.exe')

# 与源码 LayoutControls 一致
TAB_X0, TAB_W, TAB_GAP, TAB_CY = 250, 110, 8, 38

fails = []


def check(cond, msg):
    print(('  OK   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


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


def rect(h, parent=None):
    r = wintypes.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    if parent:
        pt = wintypes.POINT()
        pt.x, pt.y = r.left, r.top
        user32.ScreenToClient(parent, ctypes.byref(pt))
        return (pt.x, pt.y, pt.x + (r.right - r.left), pt.y + (r.bottom - r.top))
    return (r.left, r.top, r.right, r.bottom)


def visible(h):
    return bool(user32.IsWindowVisible(h))


def click_tab(hwnd, idx):
    x = TAB_X0 + idx * (TAB_W + TAB_GAP) + TAB_W // 2
    lp = (TAB_CY << 16) | x
    user32.SendMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
    user32.SendMessageW(hwnd, WM_LBUTTONUP, 0, lp)


def no_dialog(msg):
    dlg = find_windows('#32770')
    check(len(dlg) == 0, msg)
    for d in dlg:
        user32.PostMessageW(d, WM_CLOSE, 0, 0)


def main():
    if not os.path.exists(EXE):
        sys.exit('not built yet: %s' % EXE)

    ctypes.windll.shell32.ShellExecuteW(None, 'runas', EXE, None, None, 1)
    time.sleep(7)

    mains = find_windows('SysCleanWnd')
    if not mains:
        sys.exit('main window did not appear')
    mw = mains[0]

    cr = wintypes.RECT()
    user32.GetClientRect(mw, ctypes.byref(cr))
    cw, ch = cr.right, cr.bottom
    print('main window  : %s  client %dx%d' % (text(mw), cw, ch))
    print()

    allk = kids(mw)
    btns = [k for k in allk if cls(k) == 'SysCleanBtn']
    lvs = [k for k in allk if cls(k) == 'SysListView32']
    eds = [k for k in allk if cls(k) == 'Edit']
    sts = [k for k in allk if cls(k) == 'Static']

    print('controls: buttons=%d listview=%d edit=%d static=%d'
          % (len(btns), len(lvs), len(eds), len(sts)))

    # 三个页面共 4+2(自身) + 5(进程) + 4(菜单) = 15 个自绘按钮
    check(len(btns) == 15, '共 15 个自绘按钮 (自身 4+2 / 进程 5 / 菜单 4)')
    check(len(lvs) == 4, '共 4 个 ListView (程序 / 进程 / 线程 / 菜单)')
    check(len(eds) == 1, '1 个日志 Edit')
    check(len(sts) == 2, '2 个提示条 STATIC (进程页 / 菜单页)')

    # ---- 第 1 页: 首页 ----
    print()
    print('--- 第 1 页 首页 ---')
    click_tab(mw, 0)
    time.sleep(1.2)
    vis_btn = [k for k in btns if visible(k)]
    vis_lv = [k for k in lvs if visible(k)]
    vis_ed = [k for k in eds if visible(k)]
    print('visible buttons=%d listview=%d edit=%d'
          % (len(vis_btn), len(vis_lv), len(vis_ed)))
    for k in vis_btn:
        r = rect(k, mw)
        print('   %-10s x=%4d y=%4d w=%3d h=%2d' %
              (text(k), r[0], r[1], r[2] - r[0], r[3] - r[1]))

    check(len(vis_btn) == 4, '首页 4 个按钮 (确认/取消默认隐藏)')
    check(len(vis_lv) == 1, '首页 1 个 ListView')
    check(len(vis_ed) == 1, '首页显示日志')

    if vis_btn:
        maxb = max(rect(k, mw)[3] for k in vis_btn)
        check(maxb > ch - 80, '操作按钮位于窗口底部 (y=%d, 高=%d)' % (maxb, ch))
        check(all(rect(k, mw)[0] >= 0 and rect(k, mw)[2] <= cw for k in vis_btn),
              '所有按钮水平不越界')

    if vis_lv:
        rows = user32.SendMessageW(vis_lv[0], LVM_GETITEMCOUNT, 0, 0)
        hdr = user32.SendMessageW(vis_lv[0], LVM_GETHEADER, 0, 0)
        cols = user32.SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0)
        r = rect(vis_lv[0], mw)
        print('program list rows=%d cols=%d x=%d y=%d w=%d h=%d'
              % (rows, cols, r[0], r[1], r[2] - r[0], r[3] - r[1]))
        check(rows > 0, '程序列表已填充 (rows=%d)' % rows)
        check(cols == 7, '程序列表 7 列')
        check(r[0] > 0 and r[1] > 0 and r[2] <= cw and r[3] <= ch, '列表在客户区内')
        check(r[3] < ch - 80, '列表不压到底部操作栏')

    # 未勾选时点卸载 -> 不弹窗, 进入内嵌确认
    uninst = [k for k in vis_btn if '卸载选中' in text(k)]
    if uninst:
        user32.SendMessageW(uninst[0], BM_CLICK, 0, 0)
        time.sleep(0.8)
        no_dialog('未勾选时点卸载不弹对话框')

    # ---- 第 2 页: 进程管理 ----
    print()
    print('--- 第 2 页 进程管理 ---')
    click_tab(mw, 1)
    time.sleep(2.5)
    vis_btn = [k for k in btns if visible(k)]
    vis_lv = [k for k in lvs if visible(k)]
    vis_st = [k for k in sts if visible(k)]
    print('visible buttons=%d listview=%d static=%d'
          % (len(vis_btn), len(vis_lv), len(vis_st)))
    check(len(vis_btn) == 5, '进程管理页 5 个按钮')
    check(len(vis_lv) == 2, '进程管理页 2 个 ListView (进程 / 线程)')
    check(len(vis_st) == 1, '进程管理页 1 个提示条')
    check(len([k for k in eds if visible(k)]) == 0, '进程管理页不显示日志框')

    if len(vis_lv) == 2:
        for i, name in enumerate(('进程', '线程')):
            n = user32.SendMessageW(vis_lv[i], LVM_GETITEMCOUNT, 0, 0)
            hdr = user32.SendMessageW(vis_lv[i], LVM_GETHEADER, 0, 0)
            c = user32.SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0)
            print('  %s list rows=%d cols=%d' % (name, n, c))
            check(n > 0, '%s列表有数据 (rows=%d)' % (name, n))
        check(user32.SendMessageW(user32.SendMessageW(vis_lv[0], LVM_GETHEADER, 0, 0),
                                  HDM_GETITEMCOUNT, 0, 0) == 8, '进程列表 8 列')
        check(user32.SendMessageW(user32.SendMessageW(vis_lv[1], LVM_GETHEADER, 0, 0),
                                  HDM_GETITEMCOUNT, 0, 0) == 7, '线程列表 7 列')

        if vis_btn:
            tb = min(rect(k, mw)[1] for k in vis_btn)
            check(rect(vis_lv[1], mw)[3] <= tb, '线程列表不压到底部按钮')

    # 未选中线程时点结束线程 -> 提示条, 不弹窗
    tkill = [k for k in vis_btn if '结束线程' in text(k)]
    if tkill and vis_st:
        user32.SendMessageW(tkill[0], BM_CLICK, 0, 0)
        time.sleep(0.6)
        info = text(vis_st[0])
        print('thread info  %r' % info)
        no_dialog('结束线程未选中时不弹对话框')
        check('选中' in info, '结束线程未选中时提示条给出引导')

    # ---- 第 3 页: 右键菜单 ----
    print()
    print('--- 第 3 页 右键菜单 ---')
    click_tab(mw, 2)
    time.sleep(2.5)
    vis_btn = [k for k in btns if visible(k)]
    vis_lv = [k for k in lvs if visible(k)]
    vis_st = [k for k in sts if visible(k)]
    print('visible buttons=%d listview=%d static=%d'
          % (len(vis_btn), len(vis_lv), len(vis_st)))
    check(len(vis_btn) == 4, '右键菜单页 4 个按钮')
    check(len(vis_lv) == 1, '右键菜单页 1 个 ListView')
    check(len(vis_st) == 1, '右键菜单页 1 个提示条')

    if vis_lv:
        rows = user32.SendMessageW(vis_lv[0], LVM_GETITEMCOUNT, 0, 0)
        hdr = user32.SendMessageW(vis_lv[0], LVM_GETHEADER, 0, 0)
        cols = user32.SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0)
        print('menu list rows=%d cols=%d' % (rows, cols))
        check(rows > 0 and cols == 6, '右键菜单列表 6 列且有数据')

    # 未选中时点删除 -> 提示条, 不弹窗
    mdel = [k for k in vis_btn if '删除选中' in text(k)]
    if mdel and vis_st:
        user32.SendMessageW(mdel[0], BM_CLICK, 0, 0)
        time.sleep(0.6)
        info = text(vis_st[0])
        print('menu info    %r' % info)
        no_dialog('删除未选中项时不弹对话框')
        check('选中' in info, '删除未选中项时提示条给出引导')

        # 选中一行再点删除 -> 应进入「再点一次确认」
        # 注意: 必须点在文字列上。x 落在最左边的复选框列会真的切换
        # 该项的启用状态 (直接改注册表), 回归脚本不能碰那里。
        if vis_lv:
            user32.PostMessageW(vis_lv[0], WM_LBUTTONDOWN, 1, 300 | (45 << 16))
            user32.PostMessageW(vis_lv[0], WM_LBUTTONUP, 0, 300 | (45 << 16))
            time.sleep(0.4)
            user32.SendMessageW(mdel[0], BM_CLICK, 0, 0)
            time.sleep(0.6)
            info = text(vis_st[0])
            print('menu confirm %r' % info)
            check('再点一次' in info, '第一次点删除只要求二次确认')
            no_dialog('二次确认过程不弹对话框')

    print()
    if fails:
        print('RESULT: FAIL (%d 项)' % len(fails))
    else:
        print('RESULT: PASS')

    user32.PostMessageW(mw, WM_CLOSE, 0, 0)
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
