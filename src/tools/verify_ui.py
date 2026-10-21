"""检查 SysClean 界面结构是否正确 (不截图, 直接读控件属性)。

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

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', '..', 'dist', 'SysClean.exe')

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
    """返回控件相对 parent 客户区的 (l, t, r, b)"""
    r = wintypes.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    if parent:
        pt = wintypes.POINT()
        pt.x, pt.y = r.left, r.top
        user32.ScreenToClient(parent, ctypes.byref(pt))
        r2 = wintypes.RECT()
        user32.GetWindowRect(h, ctypes.byref(r2))
        return (pt.x, pt.y, pt.x + (r2.right - r2.left), pt.y + (r2.bottom - r2.top))
    return (r.left, r.top, r.right, r.bottom)


def visible(h):
    return bool(user32.IsWindowVisible(h))


def main():
    if not os.path.exists(EXE):
        sys.exit('not built yet: %s' % EXE)

    ctypes.windll.shell32.ShellExecuteW(None, 'runas', EXE, None, None, 1)
    time.sleep(6)

    mains = find_windows('SysCleanWnd')
    if not mains:
        sys.exit('main window did not appear')
    main_wnd = mains[0]

    cr = wintypes.RECT()
    user32.GetClientRect(main_wnd, ctypes.byref(cr))
    cw, ch = cr.right, cr.bottom
    print('main window  : %s  client %dx%d' % (text(main_wnd), cw, ch))
    print()

    # ---- 按钮 ----
    allk = kids(main_wnd)
    btns = [k for k in allk if cls(k) == 'SysCleanBtn']
    shown = [k for k in btns if visible(k)]
    print('buttons total=%d visible=%d' % (len(btns), len(shown)))
    for k in shown:
        r = rect(k, main_wnd)
        print('   %-10s x=%4d y=%4d w=%3d h=%2d' %
              (text(k), r[0], r[1], r[2] - r[0], r[3] - r[1]))
    print()

    check(len(btns) == 8, '应有 8 个自绘按钮 (6 操作 + 2 确认)')
    check(len(shown) == 6, '初始只显示 6 个操作按钮, 确认按钮应隐藏')

    # 操作按钮必须都在底部操作栏
    if shown:
        maxbottom = max(rect(k, main_wnd)[3] for k in shown)
        check(maxbottom > ch - 80, '操作按钮位于窗口底部 (底部 y=%d, 客户区高=%d)' % (maxbottom, ch))
        allin = all(rect(k, main_wnd)[0] >= 0 and rect(k, main_wnd)[2] <= cw for k in shown)
        check(allin, '所有按钮水平不越界')

    # ---- 列表 ----
    lv = [k for k in allk if cls(k) == 'SysListView32']
    check(len(lv) == 1, '主窗口有 1 个 ListView')
    if lv:
        rows = user32.SendMessageW(lv[0], LVM_GETITEMCOUNT, 0, 0)
        hdr = user32.SendMessageW(lv[0], LVM_GETHEADER, 0, 0)
        cols = user32.SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0)
        r = rect(lv[0], main_wnd)
        print()
        print('listview rows=%d cols=%d  x=%d y=%d w=%d h=%d' %
              (rows, cols, r[0], r[1], r[2] - r[0], r[3] - r[1]))
        check(rows > 0, '列表已填充数据 (rows=%d)' % rows)
        check(cols == 7, '列表有 7 列')
        check(r[0] > 0 and r[1] > 0 and r[2] <= cw and r[3] <= ch, '列表在客户区内')
        check(r[3] < ch - 80, '列表不压到底部操作栏')

    # ---- 日志 ----
    ed = [k for k in allk if cls(k) == 'Edit']
    check(len(ed) == 1, '主窗口有 1 个日志 Edit')
    if ed:
        r = rect(ed[0], main_wnd)
        print('log edit    x=%d y=%d w=%d h=%d' % (r[0], r[1], r[2] - r[0], r[3] - r[1]))
        check(r[1] > rect(lv[0], main_wnd)[3] if lv else True, '日志在列表下方')

    # ---- 无弹窗验证: 点「卸载选中」未勾选时不应出现对话框 ----
    uninst = [k for k in btns if '卸载选中' in text(k)]
    if uninst:
        user32.SendMessageW(uninst[0], BM_CLICK, 0, 0)
        time.sleep(1)
        dlg = find_windows('#32770')     # 标准对话框类名
        check(len(dlg) == 0, '未勾选时点卸载不弹对话框')
        for d in dlg:
            user32.PostMessageW(d, WM_CLOSE, 0, 0)

    # ---- 右键菜单窗口 ----
    target = [k for k in btns if '右键菜单' in text(k)]
    if target:
        user32.SendMessageW(target[0], BM_CLICK, 0, 0)
        time.sleep(3)
        menus = find_windows('SysCleanMenuWnd')
        check(bool(menus), '右键菜单窗口能打开')
        if menus:
            mw = menus[0]
            mk = kids(mw)
            mlv = [k for k in mk if cls(k) == 'SysListView32']
            mbtn = [k for k in mk if cls(k) == 'SysCleanBtn']
            minfo = [k for k in mk if cls(k) == 'Static']

            print('menu window  buttons=%d listview=%d static=%d' %
                  (len(mbtn), len(mlv), len(minfo)))
            check(len(mbtn) == 4, '菜单窗口 4 个自绘按钮')
            check(len(mlv) == 1, '菜单窗口 1 个 ListView')
            check(len(minfo) == 1, '菜单窗口 1 个提示条 (替代弹窗)')

            if mlv:
                mrows = user32.SendMessageW(mlv[0], LVM_GETITEMCOUNT, 0, 0)
                mhdr = user32.SendMessageW(mlv[0], LVM_GETHEADER, 0, 0)
                mcols = user32.SendMessageW(mhdr, HDM_GETITEMCOUNT, 0, 0)
                print('menu list    rows=%d cols=%d' % (mrows, mcols))
                check(mrows > 0 and mcols == 6, '右键菜单列表 6 列且有数据')

            # 按钮必须在窗口底部, 列表在按钮上方
            mcr = wintypes.RECT()
            user32.GetClientRect(mw, ctypes.byref(mcr))
            mch = mcr.bottom
            if mbtn and mlv:
                mb = max(rect(k, mw)[3] for k in mbtn)
                lr = rect(mlv[0], mw)
                check(mb > mch - 80, '菜单窗口按钮在底部 (y=%d, 高=%d)' % (mb, mch))
                check(lr[3] < mb, '菜单窗口列表在按钮上方')

            # 未选中时点「删除选中」→ 只在提示条显示, 不弹窗
            mdel = [k for k in mbtn if '删除选中' in text(k)]
            if mdel and minfo:
                user32.SendMessageW(mdel[0], BM_CLICK, 0, 0)
                time.sleep(0.6)
                info_txt = text(minfo[0])
                print('menu info    %r' % info_txt)
                check(len(find_windows('#32770')) == 0, '删除未选中项时不弹对话框')
                check('选中' in info_txt, '删除未选中项时提示条给出引导')

                # 选中第一行再点删除 → 应进入「再点一次确认」, 而不是直接删
                # 注意: 必须点在文字列上。x 落在最左边的复选框列会真的切换
                # 该项的启用状态 (直接改注册表), 回归脚本不能碰那里。
                user32.PostMessageW(mlv[0], 0x0201, 1, 300 | (45 << 16))   # WM_LBUTTONDOWN
                user32.PostMessageW(mlv[0], 0x0202, 0, 300 | (45 << 16))   # WM_LBUTTONUP
                time.sleep(0.4)
                user32.SendMessageW(mdel[0], BM_CLICK, 0, 0)
                time.sleep(0.6)
                info_txt = text(minfo[0])
                print('menu confirm %r' % info_txt)
                check('再点一次' in info_txt, '第一次点删除只要求二次确认')
                check(len(find_windows('#32770')) == 0, '二次确认过程不弹对话框')

            user32.PostMessageW(mw, WM_CLOSE, 0, 0)
            time.sleep(0.6)

    # ---- 线程窗口 ----
    tgt = [k for k in btns if '进程与线程' in text(k)]
    if tgt:
        user32.SendMessageW(tgt[0], BM_CLICK, 0, 0)
        time.sleep(3)
        tws = find_windows('SysCleanThreadWnd')
        check(bool(tws), '线程窗口能打开')
        if tws:
            tw = tws[0]
            tk = kids(tw)
            tlv = [k for k in tk if cls(k) == 'SysListView32']
            tbtn = [k for k in tk if cls(k) == 'SysCleanBtn']
            tinfo = [k for k in tk if cls(k) == 'Static']
            print('thread window buttons=%d listview=%d static=%d' %
                  (len(tbtn), len(tlv), len(tinfo)))
            check(len(tbtn) == 5, '线程窗口 5 个自绘按钮')
            check(len(tlv) == 2, '线程窗口 2 个 ListView (进程 / 线程)')
            check(len(tinfo) == 1, '线程窗口 1 个提示条 (替代弹窗)')

            if len(tlv) == 2:
                for i, name in enumerate(('进程', '线程')):
                    n = user32.SendMessageW(tlv[i], LVM_GETITEMCOUNT, 0, 0)
                    hdr = user32.SendMessageW(tlv[i], LVM_GETHEADER, 0, 0)
                    c = user32.SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0)
                    print('  %s list rows=%d cols=%d' % (name, n, c))
                    check(n > 0, '%s列表有数据 (rows=%d)' % (name, n))
                check(user32.SendMessageW(tlv[0], LVM_GETHEADER, 0, 0) and
                      user32.SendMessageW(user32.SendMessageW(tlv[0], LVM_GETHEADER, 0, 0),
                                          HDM_GETITEMCOUNT, 0, 0) == 8, '进程列表 8 列')
                check(user32.SendMessageW(user32.SendMessageW(tlv[1], LVM_GETHEADER, 0, 0),
                                          HDM_GETITEMCOUNT, 0, 0) == 7, '线程列表 7 列')

            # 未选中线程时点「结束线程」→ 提示条, 不弹窗
            tkill = [k for k in tbtn if '结束线程' in text(k)]
            if tkill and tinfo:
                user32.SendMessageW(tkill[0], BM_CLICK, 0, 0)
                time.sleep(0.6)
                info_txt = text(tinfo[0])
                print('thread info  %r' % info_txt)
                check(len(find_windows('#32770')) == 0, '结束线程未选中时不弹对话框')
                check('选中' in info_txt, '结束线程未选中时提示条给出引导')

            # 两个列表都不能压到底部按钮
            tcr = wintypes.RECT()
            user32.GetClientRect(tw, ctypes.byref(tcr))
            if tbtn and len(tlv) == 2:
                tb = min(rect(k, tw)[1] for k in tbtn)
                check(rect(tlv[1], tw)[3] <= tb, '线程列表不压到底部按钮')

            user32.PostMessageW(tw, WM_CLOSE, 0, 0)

    print()
    if fails:
        print('RESULT: FAIL (%d 项)' % len(fails))
    else:
        print('RESULT: PASS')

    user32.PostMessageW(main_wnd, WM_CLOSE, 0, 0)
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
