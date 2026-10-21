# 更新记录

版本号按功能模块递增，没有走严格的语义化版本。每个版本都是可用状态。

## v1.5 — 右键菜单管理

- 新增**右键菜单管理**窗口，扫描 `HKCR` 下 8 个位置，列出全部 shell verb 和 shellex 处理器
- 菜单名称支持间接字符串解析（`SHLoadIndirectString`），`@shell32.dll,-8506` 显示为「在此处打开命令窗口」
- 支持**临时禁用**（可逆）：命令型写 `LegacyDisable`，扩展型把 CLSID 挪到 `SysCleanBackup`
- 支持**彻底删除**：先 `RegSaveKeyEx` 备份到 `backup\menu\<时间戳>\`，再 `RegDeleteTree`
- 第三方菜单排前、系统菜单排后；系统/第三方判定同时看命令路径和资源路径
- 新增 `/menutest`、`/menutoggle` 两个自检开关

## v1.4 — 占用文件强杀

- 删除失败的文件改用 **Restart Manager** 精确定位占用进程，而不是猜
- 占用进程强杀后重试，最多 3 轮
- 仍删不掉的用 `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` 标记重启删除
- explorer / dwm / 杀软 / 内核进程不杀，避免桌面崩溃或蓝屏
- 新增 `/testdelete <目录> [/nokill]` 隔离测试模式

## v1.3 — AppData 扫描 + 关联注册表清理

- 新增 **HKCU** 和 **AppData（Local / Roaming）** 两路扫描，扫出控制面板看不到的软件
- 卸载时删除对应的 AppData 数据目录（最多 4 个）
- 卸载时扫描并清理关联注册表：卸载项、软件项、启动项、服务项
- 注册表路径守卫 14 条，只删键名与目录名完全相等的键，绝不退到厂商级

## v1.2 — 进程与线程视图

- 新增**进程与线程**二级窗口，上下列表联动
- 线程起始地址通过 `NtQueryInformationThread(ThreadQuerySetWin32StartAddress)` 获取
- 地址反查所属模块，给出 `模块名 + 偏移` 和**模块磁盘路径**
- 双击线程直接打开资源管理器选中对应 dll
- 支持结束单个线程、复制线程信息

## v1.1 — 强制卸载

- 新增**卸载选中项**：关进程 → 删安装目录 → 清注册表
- 删除前 `RegSaveKeyEx` 备份成 `.hiv`
- 三套路径守卫（安装目录 16 条 / 数据目录 8 条 / 注册表 14 条），`/selftest` 可验证
- 从 `UninstallString` / `DisplayIcon` 反推缺失的安装目录

## v1.0 — 进程清理

- 一键终止当前用户会话内所有非系统进程
- 内核 critical process 白名单，Session 0 的系统服务不碰
- 三路合并的程序列表（HKLM 64/32 + HKCU）
- 静态链接 CRT，单文件 exe，Win7 起可直接运行
