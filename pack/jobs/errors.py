"""Stable job error codes. Diagnose at the failure site, never by guessing prose."""
from __future__ import annotations

import errno
import json
import os
import re
from pathlib import Path
import stat


RECOVERY = {
    "addr_short": "这款游戏要的地址范围比 macOS 能给 App 的多（每台 Mac 都一样，和内存大小、存储空间都无关，换 Mac 没有用）：可能会卡或停在开头，我们在改进；游戏照常能启动。",   # Mac 起局宿主报「地址空间不够这款」（build.addr_short_watch 发的不阻塞提示 notice）
    "sgrade_upgrading": "等这款的 S 级优化升级做完（游戏上有进度），再点开始游戏；别的游戏照常能玩。",   # 11-17 S 级优化改手动单款升级（sgrade.upgrade_game），升级中只挡这一款
    "ios_install_dir_unwritable": "先试这两步：①在 iPhone 上打开一次 S2Arm，等游戏库界面出来；②回电脑对这款游戏再点一次「推到 iPhone」，会接着已传的部分继续，不用从头来。还是写不进，就要删掉 iPhone 上的 S2Arm App 再重装：先在游戏菜单「存档管理」里导出存档，存到 S2Arm 文件夹以外（比如 iCloud 云盘）；再在 iPhone 桌面长按 S2Arm，删除 App；然后在电脑上的 S2Arm 里对这款游戏再点一次「推到 iPhone」；装好后在「存档管理」里导回存档。电脑上的游戏和 S2Arm 都不用删。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_install_no_space": "清出空间后点「重试」。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_install_img_format": "更新 iPhone 上的 S2Arm，再打开这款游戏。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_install_img_corrupt": "从电脑重新装到 iPhone，会接着已传的部分续传。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_import_img_corrupt": "在游戏库里长按这款，选「删除游戏…」，再把电脑上导出的这款 .img 重新放进「放这里导入」。",   # 同上，但坏的 game.img 是玩家放进「放这里导入」认领的（pack_apply.c came_from_import）：从电脑续传不对路，删掉重新导入
    "ios_install_pack_truncated": "重新连电脑推送，会自动补齐。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_install_files_missing": "从电脑重新装到 iPhone。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_install_manifest_bad": "从电脑重新装到 iPhone。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_install_write_failed": "重新打开 S2Arm 后点「重试」；还不行就导出诊断包发给我们。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_import_update_no_base": "先把导出文件夹里这款的完整 .img（不带「着色器更新包」字样的那个）放进「放这里导入」，导好后再放更新包。",   # 着色器更新包认领时本机没有本体（pack_apply.c img_update）
    "ios_import_update_mismatch": "把电脑上这次导出的完整 .img 放进「放这里导入」重新导一遍（游戏版本换过，只换着色器套不上）。",   # 更新包的资源包号和本机本体不一致
    "ios_import_img_failed": "检查文件是否拷完整，再打开 S2Arm。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "ios_install_unknown": "导出诊断包发给我们。",   # iOS 端装机 / 导入失败：手机上的 S2Arm 报码、按码显示（11-11 维护者「补上说人话的原因」）
    "rewrite_gap": "之前已经装到 iPhone 上的这款可以照常接着玩。点「复制错误摘要」发给我们，修好后更新 S2Arm 再出一次包。",
    "update_incomplete": "按黄字里写的补上缺的那一项，再点一次「更新这款」；其余部分这次已经更新好了。",   # 「更新这款」做不齐（缺原料 / 宿主约定号变了 / Mac 上正在运行），黄字说明哪一项
    "update_failed": "检查网络后重试；还是不行就到下载页下载新版覆盖安装，当前版本和存档不受影响。",
    "glslc_windows_unavailable": "这款游戏的着色器编译目前只能在 Mac 上完成，Windows 版暂不支持；请改用 Mac 版 S2Arm 出包。",
    "win_host_unsupported": "请改用64位Windows主机运行S2Arm，确认后点「重新检查」。",
    "win_metal_missing": "用自己的Apple ID登录苹果开发者下载页（https://developer.apple.com/download/），下载安装官方Metal Developer Tools for Windows，装好后点「重新检查」。",
    "win_apple_devices_missing": "从Microsoft Store安装Apple Devices或iTunes（https://support.apple.com/118290），装好后点「重新检查」。",
    "win_apple_service_stopped": "在Windows「服务」中启动Apple Mobile Device Service；若没有该服务，从Microsoft Store安装Apple Devices或iTunes，完成后点「重新检查」。",
    "win_icloud_missing": "从Microsoft Store安装官方iCloud for Windows（https://support.apple.com/103232），装好后点「重新检查」。",
    "win_anisette_source": "选择本机官方iCloud或我方自建来源；公共服务器仅可手动作为备选，选中仍不判就绪，配置后点「重新检查」。",
    "win_disk_budget_unknown": "先在游戏库选择游戏并生成出包计划，再点「重新检查」。",
    "win_check_failed": "本次环境探测失败；按诊断检查工具安装和权限，处理后点「重新检查」。",
    "impactor_missing": "S2Arm Windows 版自带的签名工具 Impactor 不在了（安装包不完整）：重新下载并完整解压 S2Arm Windows 版，用里面的 S2Arm.exe 打开后再试。",   # 11-07 起发版随包带原版 Impactor（维护者：不让玩家跳转下载）；缺 = 安装包不完整。源码树的说法见 DEVELOPMENT_RECOVERY；引导 pack/jobs/impactor_guide.json
    "impactor_version_untested": "换成实测过的 Impactor 2.7.3（官方发布页 https://github.com/claration/impactor/releases/tag/v2.7.3）；别的版本可能丢掉「提高内存上限」权限，大游戏会闪退。",   # Windows 签装交给玩家自装的 Impactor（11-04 维护者）；引导 pack/jobs/impactor_guide.json
    "apple_id_wrong": "Apple ID 或密码不对：把输入法切到英文后重新登录（全角字符会被判错）；密码只发给 Apple。",   # Windows 签装交给玩家自装的 Impactor（11-04 维护者）；引导 pack/jobs/impactor_guide.json
    "two_factor_wrong": "两步验证码不对或已过期：重新获取验证码后输入；短信收码目前常失败，用受信任设备上弹出的验证码。",   # Windows 签装交给玩家自装的 Impactor（11-04 维护者）；引导 pack/jobs/impactor_guide.json
    "login_identity_rejected": "密码已通过，但苹果拒绝了这台电脑的登录身份：在 Impactor「设置」里点「Reset Anisette Data」后重新添加账号；仍不行就换一台电脑或稍后再试。",   # Windows 签装交给玩家自装的 Impactor（11-04 维护者）；引导 pack/jobs/impactor_guide.json
    "ipa_needs_repack": "这个包是旧版 S2Arm 出的（zip64 或中文显示名），侧载工具读不了：更新 S2Arm 后重新出包再装。",   # Windows 签装交给玩家自装的 Impactor（11-04 维护者）；引导 pack/jobs/impactor_guide.json
    "job_failed": "查看本任务的具体日志，修复提示的问题后重试；反馈时复制已脱敏的错误摘要。",
    "tool_exit": "查看本任务日志和失败阶段，按提示修复后重试；反馈时复制错误摘要。",
    "game_crashed": "再启动一次试试；还是这样，就在「环境检查」导出诊断包，到 GitHub 发帖附上，我们按崩溃现场修。",   # Mac 起局：游戏进程中途非 0 退出（build.launch）
    "game_heap_learned": "再点一次「开始」就能进：这一局已记下这款要的内存，下次起局按它给。",   # Mac 起局：普通堆超出堆区、已记下（runtime svc_mem.c heap_need，build.launch）
    "game_gpu_stopped": "点「再启动一次」一般能接着玩，存档停在游戏上次保存的地方。",   # Mac 起局：显卡挂死、运行时安全停（退出码 86，build.launch）
    "keys_missing": "在「设置 → 玩家密钥」导入自行导出的 prod.keys，然后重新导入游戏。",
    "keys_invalid": "重新选择完整的 prod.keys 文本文件，不要手工补写密钥值；原有密钥保持不变。",
    "content_unreadable": "检查文件和票据是否完整；若另行确认 Switch 密钥版本不足，可重新导出并到「设置 → 玩家密钥」重新导入。",
    "ticket_missing": "核对源文件是否含有所需票据。",
    "content_truncated": "从原始位置重新复制完整的游戏文件，再重新导入。",
    "keys_outdated": "用系统版本更新的 Switch 重新导出 prod.keys，再到「设置 → 玩家密钥」重新导入。",
    "input_type_unsupported": "选择 NSP、XCI 文件或包含它们的文件夹，然后重新导入。",
    "input_empty": "确认所选文件夹包含 NSP 或 XCI 文件，再重新导入。",
    "input_permission": "打开「系统设置 › 隐私与安全性 › 文件与文件夹」（外接盘看「可移除的宗卷」），允许本应用读取游戏文件所在位置，或把游戏文件拷到「下载」之外的本机文件夹，然后重试。",
    "input_unreadable": "确认游戏文件所在磁盘已连接、文件没有损坏（换一个位置拷一份再放），然后重试；反馈时复制错误摘要。",
    "input_missing": "接上游戏原件所在的磁盘，确认文件仍在原位置且可读取；原件挪了位置就把它重新放进来一次。",
    "input_wrong_game": "选这一款的原件（nsp / xci 等）再试：选错的那份没有记进游戏库。",   # 「重新添加原件…」带 expectGameId（#1961）
    "input_base_missing": "将对应的游戏本体 NSP 或 XCI 一并导入；仅更新或 DLC 不能单独打包。",
    "guest_isa_unsupported": "这款游戏的 32 位 ARM 程序暂不支持，重试无法解决；请选择 64 位 ARM 游戏。",
    "content_invalid": "检查原始游戏文件是否完整，重新导入后再试；仍失败时复制错误摘要并保留对应日志。",
    "library_not_apfs": "游戏原件放哪块盘都行，导出时的工作数据（游戏库）要放在 APFS 或 exFAT 格式的磁盘上：在「设置 → 游戏库与玩家数据」改用这台 Mac 上的默认位置（内置盘），或选一块 APFS / exFAT 外接盘，再重试；FAT32 等格式不适合做游戏库。游戏原件不用挪。",
    "library_in_cloud": "游戏库在云盘同步目录里（iCloud 云盘、OneDrive、Dropbox 等）：出包时会写几十 GB 中间文件，云盘会上传它们、换成占位文件，出包中途会读不到。在「设置 → 游戏库与玩家数据」把游戏库改到本机磁盘上不同步的位置（例如默认位置），再重试；游戏原件放在哪都行。",
    "library_id_mismatch": "点「仍然继续」改用这个游戏库；不是你要的，就到设置里重新选游戏库。",   # 这个位置是另一个游戏库（库号和启动器记的不同）：说清是哪个、几款，玩家确认就改用（11-17 维护者）
    "library_folder_not_empty": "选的文件夹里已有别的文件：确认「仍然继续」就照用这个文件夹，S2Arm 只在自己建的文件和子文件夹里读写，不改也不删它们；不想这样就选一个空文件夹。",
    "library_bad_format": "可以点「仍然用这里」照用这块盘（出包会多占空间）；想省空间就从下面列出的 NTFS 盘里选一块，点「放到这里」。",
    "fat32_file_too_large": "在出包页「存放位置」换到 NTFS 格式的盘（上面列出了这台电脑上可用的），再出包；游戏原件放在哪都行。",
    "library_move_failed": "游戏库还在原来的位置，什么都没有丢：按上面的原因处理后再搬一次。",
    "batch_record_failed": "批次记录（数据目录下的 batch.json）写不进：检查数据目录所在磁盘的空间与权限，处理后在批量出包页点「继续」；已出好的包都在。",
    "library_busy": "等正在用游戏库的那个任务结束（或在任务列表里取消它）再重试；什么都没有改动。",
    "library_lock_stuck": "原任务进程已不在，但游戏库的锁仍被占用。重新连接或重新挂载游戏库所在磁盘后再试；不要手动删除锁文件。本次没有改动游戏文件。",
    "downstream_ineligible": "这个下游方案只支持某一版更新（上面写了是哪一版）：导入那一版更新后再导出；不换更新就在确认页选「仍用原版方案」。",
    "downstream_export_unsupported": "这款选的下游方案是独立 App，资源要由电脑推进去：连上手机后点「推到 iPhone」；想导出自签包就在确认页改选「仍用原版方案」。",
    "download_failed": "检查这台电脑的网络（能打开 s2arm.com）后再点一次；下了一半的不会被当成好的用。",
    "diag_upload_failed": "导出诊断包，手动发到 GitHub 帖子或群里；网络恢复后也可以再点一次「故障报修」。",   # 上传诊断没成（服务没开通 / 连不上 / 被拒收，diag_upload.py）：界面退回导出
    "disk_io_error": "这块盘读写出了错（硬件或连接问题）：插稳数据线、换个接口，或用系统自带的磁盘检查工具查一下这块盘；也可以把游戏库搬到别的盘再试。存档和已出好的版本不受影响。",
    "disk_full": "释放任务所在磁盘的空间，检查游戏库和应用数据所在磁盘后重试；不要删除存档或上次成功的运行版本。",
    "out_of_memory": "关掉其它占内存的程序（浏览器、游戏等）后重试；还不行就在 Windows 设置里调大虚拟内存（页面文件），或换一台内存更大的电脑出包。已做完的步骤下次会复用。",
    "permission_denied": "检查文件和目录权限；若位于桌面、文稿、下载或外接磁盘，请在「系统设置 → 隐私与安全性 → 文件与文件夹」允许 S2Arm 访问，再重试。",
    "volume_ops_unsupported": "游戏存放位置或游戏库所在的盘不支持 S2Arm 需要的文件操作（常见于 exFAT / FAT 格式的外接盘）：把这款游戏的存放位置换到 APFS 格式的盘（Mac 内置盘就是；外接盘可在「磁盘工具」里另分一个 APFS 分区），再重试；游戏原件不用挪。",
    "architecture_unsupported": "当前路径需要 Apple Silicon Mac，请换用支持的设备；重试不会改变硬件兼容性。",
    "os_unsupported": "当前游戏路径需要 macOS 15 或更新版本；在支持的系统上重新检查环境后再试。",
    "xcode_unavailable": "去 App Store 安装 Xcode，装好打开一次并完成必要组件安装，回来点「重新检查」。",
    "xcode_setup_required": "打开 Xcode，同意许可并完成首次设置，回来点「重新检查」。",
    "metal_unavailable": "在设置中安装 Metal 编译组件，完成后重新检查环境并重试。",
    "metal_download_unreachable": "连不上苹果的组件服务器：检查网络，关掉或换个代理后在设置里再点「一键下载」；还不行就从苹果开发者网站下载与你的 Xcode 配套的 Metal Toolchain（.dmg），在「终端」运行 xcodebuild -importComponent MetalToolchain -importPath <下载的 .dmg>，回来点「重新检查」。",
    "package_verify_mismatch": "直接再出一次包：对不上的文件会重新读、重新算，不用重新导入游戏。还是这样就检查一下游戏库所在的磁盘（Windows：磁盘属性 →「工具」→「检查」），"
                               "并关掉对游戏库文件夹的杀毒实时扫描或云同步，或把游戏库换到另一块盘。",
    "ios_platform_missing": "在设置里点「iOS 平台组件」的「一键下载」（苹果官方，约 8 GB），下完自动重新检查后重试；也可以打开 Xcode → 设置 → Components 装上与 Xcode 配套的 iOS 平台。",
    "ios_platform_unregistered": "iOS 平台组件已经下载，只是还没注册到模拟器：点「重新检查」再试；不用重复下载（约 8 GB）。还不行就在 Xcode → 设置 → Components 里确认这一项，或重启这台 Mac 后再点「重新检查」。",
    "dotnet_unavailable": "重新安装完整的 S2Arm，然后点「重新检查」。",
    "dotnet_sdk_missing": "在「设置」的环境检查中安装 .NET 10 SDK，完成后重新检查，再打 iPhone 包。",
    "android_disabled": "安卓出包还没开放：等新版本开放后再出。",
    "adb_download_failed": "检查网络后重试；也可以自己装 Android SDK Platform-Tools（含 adb），装好后再试。",
    "adb_failed": "重新插好数据线，在手机上允许 USB 调试，然后点「刷新」再试。",
    "reader_unavailable": "重新安装完整的 S2Arm，然后重新导入游戏。",
    "engine_incomplete": "重新安装完整的 S2Arm，然后点「重新检查」。",
    "engine_toolchain_newer": "把 S2Arm 更新到最新版（新版认得你的 Xcode），然后再点一次。",
    "graphics_api_unsupported": "这款暂时打不了包，换一款官方游戏试试；如果确定是正版游戏，点「复制错误摘要」发给我们。",   # #222：同人移植把图形库静态链进程序，判不出图形接口   # 11-07 联合开发者：beta.4 遇 Xcode 27.2（metal 比锁新）直连库「链号前后不一」
    "game_structure_unsupported": "这款还没适配，点「复制错误摘要」发给我们，我们按它补上。",   # v0.3.1：直连配置推导不认这款的程序结构（⑪ 照停、说人话）
    "tool_missing": "重新安装完整的 S2Arm，点「重新检查」后重试。",
    "target_unsupported": "这台设备的 GPU 或系统不在当前支持范围内，重试无法解决；需要 A14 / M1 或更新芯片及 iOS / iPadOS 18 或更新系统。",
    "xcode_sdk_too_old": "把 Xcode 更新到最新版，装好打开一次，回来点「重新检查」，再打 iPhone 包。",
    "runtime_invalid": "重新打包这款游戏以恢复完整运行文件；保留原有存档，打包成功后再启动。",
    "xcode_team_pending": "在装机页点「解决办法」打开引导项目，在 Xcode 里选 Personal Team，连上手机点运行一次，再点一次「推到 iPhone」。也可以先到 Xcode › Settings › Apple Accounts 看团队列表有没有显示。",
    "signing_certificate_missing": "打开 Xcode，点菜单 Xcode › Settings › Apple Accounts，选中账号后点 Manage Certificates，点 + 选 Apple Development，再点一次「推到 iPhone」。",
    "xcode_account_missing": "打开 Xcode →「设置」→「账户」，点「+」登录你的 Apple ID（免费的就可以），再点一次「推到 iPhone」；不想登录 Xcode 也可以改用 AltStore 安装。",
    "device_not_ready": "按上面的提示做完，再点一次安装。",
    "device_not_connected": "让你选的那台 iPhone / iPad 连上这台电脑：插上数据线，或解锁后和电脑连同一个 Wi-Fi；第一次连要在设备上点「信任」，并在「设置 → 隐私与安全性 → 开发者模式」打开开发者模式，然后再试。",
    "device_required": "在导出确认页勾上要装到的那台 iPhone / iPad，再点「开始」。",
    "signing_failed": "查看本任务日志里签名那一步的报错；确认 Xcode 能用你的账号给这台 {0} 签名，或改用 AltStore 安装。",
    "device_tunnel_unavailable": "电脑连不上这台 {0} 的开发者通道（通过 Wi-Fi 连着时最常见，不是没解锁）：用数据线把它直接插到这台电脑（别经过扩展坞或集线器），解锁停在主屏幕，再点一次；还不行就把它重启一次，插上后在设备上重新点「信任」。",
    "device_locked": "解锁这台设备（保持亮屏、停在主屏幕），再点这台的「重试」；装的过程中别锁屏。",
    "shell_install_required": "先在首位「S2Arm」卡把 App 装到这台设备，装好后点这台的「重试」。",   # 通用壳「推到 iPhone」：手机上还没装壳（install.push_game）
    "phone_app_choice_required": "点游戏库最前面的「S2Arm」，选好这台 {0} 用哪一个 S2Arm（AltStore / SideStore 装的，还是启动器装的），再推游戏；选一次就记住。",   # 同机装着不止一个 S2Arm（install.s2arm_owner choose）
    "phone_app_gone": "点游戏库最前面的「S2Arm」，重新选一次用哪一个 S2Arm：选过的那个已经不在这台 {0} 上了。",   # phoneChooseApp 选的包不在手机上
    "shell_update_required": "先在首位「S2Arm」卡把这台上的 App 更新到这一版，再点这台的「重试」。",
    "phone_app_standalone": "装一个通用 S2Arm（游戏库最前面「S2Arm」卡）后再推；如果它其实就是通用 S2Arm，用当初装它的工具装一次这一版的 S2Arm.ipa，再点这台的「重试」。",   # 推送目标多半是老独立 App（install.standalone_target），推进去不会出现
    "phone_app_outdated": "想马上用上新版修复就照上面说的先更新这台 {0} 上的 S2Arm；不急的话点「仍然继续」照旧推。",   # #1842：别的工具 / 别的电脑装的 S2Arm 比启动器旧、接口兼容（install.external_app_outdated）
    "phone_app_incompatible": "照上面说的把这台 {0} 上的 S2Arm 更新到这一版，再点这台的「重试」。",   # 同上，App 早于接口起始版（host_update.COMPAT_SINCE），推进去打不开   # 通用壳「推到 iPhone」：手机上的壳不是本版
    "device_not_visible": "用数据线把这台 {0} 连到这台 Mac，解锁后在设备上点「信任」；打开开发者模式（设置 → 隐私与安全性 → 开发者模式，开了要重启）；等 Xcode 菜单「窗口 → 设备与模拟器」里出现这台，再点一次「推到 iPhone」。",   # pack/ensure_profile.sh：Xcode 看不到这台、账号上也没登记过它（D-44DP 兜底的盲区，11-16）
    "free_app_limit": "免费 Apple ID 在每台设备上最多同时装 3 个自签 App，这台 {0} 已经装满：在设备上长按删掉一个不再玩的自签 App 后重试；或改用付费 Apple 开发者账号（没有这个上限）。",
    "signing_identity_invalid": "这台 {0} 不认这次签名用的开发证书（手机说它已失效）。常见原因：在 AltStore、SideStore、iloader 等别的工具里新建或撤销过这个 Apple ID 的证书，把 S2Arm 在用的那张顶掉了；或者这台 {0} 的日期时间不对。办法：在 {0} 上打开「设置 → 通用 → 日期与时间」里的「自动设置」，再点一次重试：S2Arm 签名前会重新向 Apple 核这张证书，已被撤掉的会换一张新的（要撤别的工具的证书时会先问你）。手机上已装的 App 和存档都不受影响。",   # D-7VBE：installd 0xe8008018（impactor.EXPLAIN / install.SIGN_ERRORS）
    "free_appid_limit": "这是 Apple 对免费 Apple ID 的限制：7 天内最多注册 10 个包名，这个账号已经用满；在手机上删 App 不会退还额度。办法：等几天，最早那批包名注册满 7 天就会恢复；或换一个 Apple ID 登录后再装（Mac 在 S2Arm 里登录，Windows 在 Impactor 里登录）；或改用付费 Apple 开发者账号（没有这个限制）。",   # Apple 错误 9120；与 free_app_limit（设备上同时 3 个）是两件事，删 App 无效
    "app_id_taken": "苹果拒绝注册这个包名（Apple 错误 9401：同一个包名在苹果那边已被占用）。请在 Impactor（Mac 上是 Xcode）里确认登录的是当初装 S2Arm 用的那个 Apple ID 再装；付费开发者账号也可以到 developer.apple.com 的「Certificates, Identifiers & Profiles → Identifiers」删掉 io.s2arm.games 开头的旧包名后重试。还不行请导出诊断包反馈。",   # Apple 错误 9401；Windows 装机先用大小写两种包名各试一次（install_win.install_app），都被拒才报这个码
    "app_other_signer": "手机上已经有一个用别的签名装的 S2Arm（包名相同、签名不同），系统不让覆盖。二选一：继续用你原来的签名工具更新它；或者在 iPhone 上长按 S2Arm 删掉它（里面的存档会一起删，先在 S2Arm 里导出存档），再点「推到 iPhone」。",   # 玩家先自签装过同包名的 S2Arm（11-11 反馈）：installation_proxy MismatchedApplicationIdentifierEntitlement
    "bundle_id_switched": "这个包名之前被别的签名用过（多半是你自己签名装 S2Arm 时用了同一个包名），这次已自动换成新包名装好。手机上可能有两个 S2Arm：新装的这个里有这台电脑装的游戏；旧的那个不再需要的话可以删掉（先导出存档）。",   # 包名被占（9401）时 Mac 自动换 <包名>.s2 装好（install.MacPhone.install），黄字说明
    "image_exec_cert": "现在用的证书是分发（发布）证书，缺「调试权限（get-task-allow）」：通用 S2Arm 里的游戏用它签了打不开。在设置里把出法改成「独立 App」（一个 App 一款游戏）就能玩；或改用 Apple ID 登录签名、换成开发证书。",   # 通用壳 + 分发证书（install.phone_signing 黄字；手机端初启失败页同码）
    "app_bundle_fixed": "你的描述文件是显式 App ID，只有一个包名：每款独立 App 和通用 S2Arm 都只能装成它，装一个会顶掉另一个（存档跟着那个 App 走）。想同时装多款，请让证书发放人给通配（*）的描述文件。",   # devcert.bundle_id 显式描述文件（install.put_app 黄字）
    "free_app_limit_note": "免费 Apple ID 有苹果的限制：同时最多装 3 个自己签的 App（AltStore / SideStore 也算），每 7 天最多注册 10 个 App ID；独立 App 每款占一个。装满了就在 iPhone 上删掉不玩的那个再装。",   # install.put_app 免费账号装独立 App 时的提示（只提示不拦）
    "save_carried": "新装的独立 App 用的是从旧 App 复制来的存档：进游戏确认进度没问题，再在 iPhone 上删掉旧 App。",   # install.carry_saves：新独立 App 里的存档是从手机上旧包名的 App / 通用壳复制来的（黄字）
    "save_carry_choose": "在启动器的手机存档里选你要的那一份，还原到新装的独立 App；其余几个 App 里的存档都没动。",   # install.carry_saves：几个 App 都有这款的存档，不替玩家挑（黄字）
    "save_carry_incomplete": "那个 App 里这款的存档没动：先别删它。在它里面进一次这款游戏、回到主界面后，再从启动器导出到这台手机一次。",   # install.carry_saves：另一个 App 里有这款存档，但索引 / 用户档案拉不全，没复制（黄字）
    "save_carry_skipped": "旧进度还在原来那个 App 里，一个字节没动：先别删它。",   # install.carry_saves / saves_elsewhere（黄字）
    "save_carry_failed": "手机连稳（数据线插好、解锁）后，对这款再导出到这台手机一次，会重新复制。",   # install.carry_saves / saves_elsewhere（黄字）
    "signing_server_unreachable": "这台电脑连不上 Impactor 签名要用的登录辅助服务器（mac-ani.jkcoxson.com，Impactor 内置、改不了），已自动重试过一次。检查这台电脑的网络：浏览器能打开这个地址就再点一次「推到 iPhone」；打不开就换个 DNS（如 223.5.5.5 或 1.1.1.1）或开 / 关代理后再装。手机、已装的 App 和存档都不受影响。",   # #3fc02b：plumesign 取 anisette（mac-ani.jkcoxson.com，Impactor v2.7.3 omnisette 写死）时 DNS / 连接失败（impactor.explain）
    "apple_session_expired": "Apple ID 登录已过期，请重新登录后再装：在导出页点「打开 Impactor」→「设置」，选中你的 Apple ID 点「移除账号」，再点「添加账号」用同一个 Apple ID 重新登录（邮箱、密码、手机上的验证码，输入前切到英文输入法），然后回来点这台的「重试」。密码只在 Impactor 里输，S2Arm 不经手。",   # Windows plumesign 恢复保存的会话时 Apple 1100（impactor.explain）；Mac 同类原文归 xcode_account_missing（去 Xcode 重新登录）
    "apple_not_signed_in": "还没用 Apple ID 登录：点「登录 Apple ID」（导出页和失败那一行都有，也可以在设置 →「签名身份」下面点），在弹出的「用 Apple ID 签名」里填邮箱和密码（你的苹果设备上会弹出验证码），再点这台的「重试」。密码只用于这次向 Apple 登录，S2Arm 不保存。",   # Mac 通用壳：随包 plumesign 自己的登录面板（launchers/macos AppleLoginViews，按钮名 / 面板名与界面逐字一致；Windows 的 Impactor 说法在 WINDOWS_RECOVERY）
    "apple_login_refused": "苹果的登录服务器拒收了这次登录请求（回了「服务暂不可用」，还没到核对账号密码那一步），不是这台电脑的网络、Apple ID 或密码的问题，换网络、换 DNS 也没用。多半是苹果又调整了对第三方签名工具的规则：把 S2Arm 更新到最新版后再登录；最新版仍这样，点「复制错误摘要」发给我们。手机、已装的 App 和存档都没动。",   # 苹果 GSA 回 5xx（11-20 起拒收系统名写 macOS; 的请求：随包 plumesign 补丁 0002 曾这样拼，D-6NB2 / D-KRNJ / #1798）：不是网络问题
    "devcert_unreadable": "要用自己的开发者证书：检查杀毒软件或 Windows 安全中心的「受控文件夹访问」有没有拦 S2Arm（把 S2Arm 加进允许的应用），然后在设置里重新导入证书。没启用过开发者证书可以不管这条。",   # 开发者证书设置读不了（D-BW75 Windows 拒绝访问）：照常按 Apple ID 签名，只说清楚
    "apple_server_unreachable": "这台电脑连不上苹果的服务器（Impactor 登录和签名都要连），不是 Apple ID 或密码的问题。检查这台电脑的网络：浏览器能打开 appleid.apple.com 就再点一次「推到 iPhone」；打不开就换个 DNS（如 223.5.5.5 或 1.1.1.1）或开 / 关代理后再装。手机、已装的 App 和存档都没动。",   # 连不上 *.apple.com（#edc5f5 拆分：网络问题，不让玩家去重登 Apple ID）
    "crash_restored": "照常玩就行；如果又崩溃，会再自动关掉，也可以在启动设置里自己关。",
    "aoc_missing": "本体已出好，照常安装即可；没解出的追加内容不在包里，游戏里用不到那部分内容。请导出诊断反馈，解码支持后重新打包即可带上。",
    "kit_oversized": "游戏照常能玩、能装；只是这款的程序件里混进了不该有的大文件，多占电脑空间、装机多推一些。请导出诊断反馈给我们。",   # 程序件（除改写产物）超过 300 MB：资源镜像混进同层（library.loud_if_kit_oversized），出包不拦、当场看见
    "direct_offline_empty": "游戏照常能玩，只是开头几次进新场景会卡一下（着色器现编，编过的会记住）。请导出诊断反馈给我们，修好后重新出包就能提前编好。",
    "sibling_clone_failed": "已改从解包目录准备，本次照常完成，只是多花一些时间；游戏库放在 APFS 格式的磁盘上（Mac 内置盘或格式化为 APFS 的外接盘）时，各平台导出可以共用已准备好的游戏数据。",
    "iml_missing": "按你装 S2Arm 的方式补上：\n· 电脑上的 S2Arm（免费或付费 Apple ID）：点游戏库第一格「S2Arm」里的「续签 S2Arm」重装一次；重装后还看到这条提示，就用下面的 GetMoreRam 补上，再点一次「续签 S2Arm」。\n· 自己的开发者证书（.p12 + 描述文件）：在苹果开发者网站给 S2Arm 的 App ID（苹果给每个 App 登记的身份）打开「Increased Memory Limit」，重新生成描述文件，用新描述文件重签再装。\n· AltStore：用 AltStore Classic 2.2 或更新版重装（不是刷新）。\n· SideStore 0.7.0：签名会丢这项权限，用下面的 GetMoreRam 补上后，在 SideStore 里重装。\n· Sideloadly：暂不支持，它没说免费账号会保留这项权限，自定义权限要付费开发者账号。\n· GetMoreRam：用装 S2Arm 的同一个 Apple ID 把它自签装到手机（占免费账号一个 App 名额，用完可删），登录后打开 App IDs、点刷新、选 S2Arm、点「Add Increased Memory Limit」，再用原来的方式重装 S2Arm。",
    "export_dir_busy": "用新文件夹里的 S2Arm.ipa 和 .img 自签安装；关掉占用旧文件夹的程序后，可以把旧的那个删掉。",
    "manual_unpack_required": "资源包已经在 {0} 上，不用重推：在设备上打开 S2Arm，选这款游戏，保持在前台别锁屏，等它导入完就能玩。",
    "usb_slow": "换一根支持 USB 3 的 USB-C 数据线，直接插 Mac 上的 USB-C / 雷雳口（不经扩展坞、集线器、显示器上的口），再装下一款；iPhone 15 Pro 及更新的 Pro 机型才支持 USB 3，其它机型只能 USB 2.0。",
    "devcert_password": "重新输入导出 .p12 时设的密码（区分大小写）；还不行就重新导出一份 .p12 再导入。",   # 开发者证书签名（pack/jobs/devcert.py，v0.2.4）
    "devcert_invalid": "选导出的 .p12 证书和开发者网站下载的 .mobileprovision 描述文件，再导入一次。",   # 开发者证书签名（pack/jobs/devcert.py，v0.2.4）
    "diag_code_invalid": "从帖子里完整复制那一整串诊断码（以 S2A-DIAG1- 开头）再粘一次；还不行就在帖子里回复，请维护者重发一条。",   # 诊断码（pack/jobs/diagcode.py，#1449）
    "devcert_mismatch": "在开发者网站重新生成描述文件并勾上这张证书，下载后和证书一起重新导入。",   # 开发者证书签名（pack/jobs/devcert.py，v0.2.4）
    "devcert_distribution": "推到 iPhone 的方式选「独立 App」，用这张证书照常能玩；要用通用 S2Arm：点「改用 Apple ID 登录」用 Apple ID 签名，或点「复制给证书发放人的话」发给对方，换成开发证书和开发描述文件后重新导入。",   # 分发描述文件没有 get-task-allow：通用 S2Arm 里游戏打不开，独立 App 能用（pack/jobs/devcert.py inspect）
    "devcert_missing": "在出包页「我的开发者证书」里重新导入证书和描述文件，或切回「Apple ID 签名」。",   # 开发者证书签名（pack/jobs/devcert.py，v0.2.4）
    "identity_unavailable": "在 AltStore 或 SideStore 的设置里导出签名证书，打开手机上的 S2Arm →「签名证书」→「导入 .p12…」导入；想让启动器新建证书就点「安装 S2Arm」（会先说明会让哪张证书失效）。",   # 交签名身份（install.deliver_identity，11-19）：Apple ID 方式、本机没有可交的证书；不为此新建（会顶掉 AltStore / SideStore 的证书）
    "identity_team_mismatch": "在启动器里登录装 S2Arm 用的那个 Apple ID（或选用和装 S2Arm 同一团队的证书），再点一次「把签名身份交给手机」。",   # 交签名身份（install.identity_target，11-19）：手机上的 S2Arm 是别的团队签的（AltStore 等用了另一个 Apple ID），交过去手机也签不了游戏
    "devcert_keychain": "解锁登录钥匙串并允许 S2Arm 访问后，重新导入证书。",   # 开发者证书签名（pack/jobs/devcert.py，v0.2.4）
    "devcert_choose": "在出包页「我的开发者证书」里给这台设备选一张证书，再点「重试」。",   # 几张证书、这台没选（pack/jobs/devcert.py，#487）
    "source_integrity_skipped": "能玩就照常用；出包或游戏里出现花屏、闪退时，换一份原版完整的文件（本体与更新 / DLC 分开的那种）再试。",   # 维护者 11-17：校验不过不再拒收（#1467）
    "widget_skipped": "App 已装好、游戏照常玩。想要主屏小组件：用 Apple ID 签名（不是自己的开发者证书）再装一次；免费 Apple ID 包名额度满了就等几天再装。",   # 主屏小组件（install.widget_skip，11-16）
    "shell_sdk_old": "装好的包能用；要在 iPhone Duo 等新机型上铺满全屏，把 Xcode 更新到不低于这台 iPhone 系统大版本的版本（如 iOS 27 用 Xcode 27），在启动器里选它后重新打 iPhone 包再装。",
    'configuration_failed': '检查当前阶段的配置推导错误，保留原始游戏输入；更新完整引擎后重新准备，仍失败时反馈错误摘要。',
    'shader_extract_failed': '检查着色器抽取的具体错误；确认游戏输入完整且版本一致，重新准备后仍失败时反馈摘要。',
    'compilation_failed': '按编译错误检查 Xcode、Metal 组件和 .NET SDK；完成环境检查后重试，保留原有成功版本与存档。',
    'rewrite_failed': '检查程序改写的首个错误；保留原始游戏文件，更新完整引擎后重试，仍失败时反馈摘要。',
    'resource_preparation_failed': '检查资源准备错误和输入目录权限，确认资源完整后重新准备；不要删除存档或上次成功的包。',
    'package_failed': '这一步的失败还没归类：错误摘要里有失败阶段、退出码和错误原文，点「复制错误摘要」发给我们。',   # 11-07 维护者：失败摘要不再带阶段 / 红行原文（只进复制诊断），原句「看摘要里的阶段」已无从看起
    'device_install_failed': '按安装错误检查设备连接与包的有效性；保持手机解锁后重试，保留原 App 和存档。',
    'resource_transfer_failed': '解锁手机并检查数据线及剩余空间后重试资源传输；保留已有 App、存档和电脑上的资源包。',
    'developer_mode_required': '在 iPhone「设置 → 隐私与安全性 → 开发者模式」按系统提示开启开发者模式，再重新安装。',
    'device_pairing_required': '重新用数据线连接并解锁 iPhone，在手机上确认「信任此电脑」，然后重新检查设备并安装。',
    'free_team_capability': '免费个人开发者团队签不了这个 App 申请的某项权限（例如「扩展虚拟寻址」）；更新 S2Arm 后重新安装（新版对免费团队只申请能签的权限），或在 Xcode 改用付费开发者团队。',
    'tvos_eva_required': 'Switch 2 游戏在 Apple TV 上要「扩展虚拟寻址」权限，只有付费 Apple 开发者账号签得了：在 Xcode「设置 → 账户」登录付费开发者账号或选付费团队，再装到 Apple TV（Switch 1 游戏用免费 Apple ID 就能装）。',
    'provisioning_expired': '开发描述文件已过期；在 Xcode 重新登录签名账号、重新申请描述文件并签名安装，保留原 App 存档。',
    'compiler_cache_incomplete': '已完成的安装包可以保留；检查磁盘余量与缓存目录权限后再清理缓存，不要删除游戏或存档。',
    'stage_timeout': '当前阶段已超时；检查该阶段日志和设备连接，确认原因后重试，不要同时重复启动同一任务。',
    'child_unkillable': '进程卡在系统内核里还没退出（多为内存不够时等换页）；等它自行结束后再开，关掉其他占内存的程序可减少再次卡住。',
    'target_unknown': '重新选择可识别的目标机型与系统版本，或连接并解锁目标设备后重新检查。',
    'job_no_request': '任务缺少请求记录，无法安全接续；返回游戏库重新发起操作，不要手工修改任务记录。',
    'job_not_ours': '此进程不属于当前任务，未发送停止信号；返回任务列表检查目标任务，不要强行结束其他进程。',
    'job_pid_reused': '原任务进程已经结束，当前进程号属于另一进程；刷新任务列表，不要按旧进程号停止任务。',
    'job_unknown': '当前私有目录里没有这条任务；刷新任务列表并确认使用原游戏库，必要时重新发起操作。',
    'journal_write_failed': '任务记录写入失败；检查磁盘空间和目录权限，保留产物与日志，恢复可写后重新检查任务。',
    'save_backup': '存档备份未完成，原存档保持不动；检查备份目录权限与空间后重新备份。',
    'save_changed': '备份期间存档发生变化；先正常退出游戏，再重新检查并备份存档。',
    'save_cleanup': '存档恢复已完成但事务清理未完成；保留备份和事务记录，检查目录权限后再检查恢复状态。',
    'save_destination': '选择游戏库和应用私有目录之外、可写且有足够空间的备份目的地。',
    'save_empty': '当前没有可备份的存档；先在游戏中正常保存并退出，再重新检查。',
    'save_exists': '目标备份已经存在；选择新文件名或新目录，不要覆盖已有备份。',
    'save_game_id': '存档缺少有效的游戏程序号；重新选择原始备份，不要手工改写存档元数据。',
    'save_invalid': '这份文件不是可识别的存档备份；重新选择由 S2Arm 导出的完整备份。',
    'save_damaged': '备份里有文件校验对不上；手动恢复会跳过坏的那几个、保留当前存档里原来的，其余照常恢复。',
    'save_no_target': '这款游戏尚未建立存档目录；先启动游戏并正常保存退出，再恢复备份。',
    'save_recovery': '存档回退没完成：事务记录与副本都保留着。可以先在存档页备份当前存档；确认可能丢掉没做完的导入 / 恢复后，可选「仍要启动」。不要删除事务目录。',
    'container_would_drop': '确认后再点一次：点名的游戏会暂时打不开（存档和资源包留在手机上）。别的电脑装的，到那台电脑上再「推到 iPhone」一次就回来；已从游戏库删除的，重新加回、「打 iPhone 包」再装就回来。不想这样就先别装。',
    'phone_app_running': '在 {0} 上从后台划掉 S2Arm（或回到主屏后等几秒），再点一次还原。',
    'save_backup_failed': '确认后再点一次就照装：存档没备份，万一覆盖出问题可能丢档；想先备份就把手机解锁、数据线插稳后再点一次。',
    'cert_revoke_confirm': '点「仍然继续」会新建证书照签。不想动现有证书的话，点「改用已有证书」，把在 AltStore / SideStore 导出的 .p12 导进来，就不用新建。',
    'app_bundle_replace_confirm': '点「仍然继续」照装。不想顶掉的话，换一份通配描述文件（App ID 以 .* 结尾），或改用 Apple ID 签名，或把几款都装进通用 S2Arm。',
    'device_space_low': '点「仍然继续」照传：手机报的空闲不含系统随时能清掉的空间，实际多半放得下，真写满了会停下并说明还差多少；也可以先在「设置 → 通用 → iPhone 储存空间」腾出空间。',
    'container_stale': '先给点名的游戏各「打 iPhone 包」一次，再装到 iPhone；也可以确认后这次先不带它们照装，它们的存档和资源留在手机上，重新出包再装就回来。',
    'container_incompatible': '这款没法和手机上已有的游戏放进同一个 App：先从 iPhone 删掉不兼容的那款。',
    'game_files_mismatch': '用原版游戏文件重新导入这款：先在游戏库里移除它，再把原版文件拖进来。',
    'container_purge_failed': '手机解锁、数据线插稳后再删一次；不影响其他游戏。',
    'keychain_prompt': '在这台 Mac 屏幕上的钥匙串弹窗里点「始终允许」（需要的话输入 Mac 登录密码），再点一次「推到 iPhone」。',
    'build_stalled': '点「重试」，已完成的阶段会核对后跳过；同一步反复卡住就导出诊断包发给我们，并关掉其他占内存的程序再试。',
    'windows_mandatory_aslr': '打开「Windows 安全中心 → 应用和浏览器控制 → 漏洞利用保护设置 → 系统设置」，把「强制映像随机化（强制性 ASLR）」改回「使用默认值（关）」，再打一次包。',
    'windows_path_too_long': '路径太长：把游戏库搬到盘符根目录附近的短路径（比如 D:\\S2Arm），再打一次包；反馈时请带上错误摘要。',
    'pack_missing': '没找到这款的资源包目录：重新「打 iPhone 包」后再装到 iPhone。',
    'game_folder_missing': '在启动器里重新添加这款游戏、打一次 iPhone 包，再装到 iPhone 或续签；手机上已装的 App 和存档不受影响。',
    'save_recovery_overridden': '已按你的确认跳过存档回退启动游戏；事务记录保留，下次启动会再试恢复。',
    'save_space': '释放备份目的地的磁盘空间后重试；不要删除原存档或唯一的备份。',
    'save_unsafe_path': '存档路径含符号链接或不安全目录；选择普通目录中的完整备份，不要绕过路径保护。',
    'save_user': '存档用户信息缺失或无效；重新选择游戏用户与原始备份，不要修改用户标识兜底。',
    'save_version': '备份与当前游戏版本或 DLC 不一致；使用匹配版本的游戏和备份后重试。',
    'save_wrong_game': '这份备份属于另一款游戏；回到对应游戏恢复，或选择当前游戏的备份。',
    'save_wrong_user': '这份备份属于另一用户；选择对应用户或该用户自己的备份后重试。',
    "app_control_blocked": "在「Windows 安全中心 → 保护历史记录」看是谁拦的。杀毒软件拦的：把 S2Arm 安装文件夹加进信任再重试；"
                           "「智能应用控制」拦的：它不能单独放行，只能在「应用和浏览器控制 → 智能应用控制」里关掉（关掉后要重装 Windows 才能再打开）；"
                           "公司电脑请让管理员放行。",
}


DEVELOPMENT_RECOVERY = {
    "impactor_missing": "先装 Impactor：在 S2Arm 里点「打开官方页面」打开官方发布页（https://github.com/claration/impactor/releases/tag/v2.7.3），下载免安装版放到桌面，再点「重新检查」。",   # 源码树 / 命令行：没有随包件，照旧让开发者自己放 Impactor（11-04 引导）
    "dotnet_unavailable": "安装 .NET 10 SDK，然后点「重新检查」。",
    "reader_unavailable": "在设置中重新准备引擎工具，然后重新导入游戏。",
    "engine_incomplete": "重新选择完整的 S2Arm 源码目录，然后点「重新检查」。",
    "tool_missing": "查看任务日志中缺少的工具，补齐构建依赖后重试。",
}


# Windows 上同一个码的「下一步」（D-4MAH：permission_denied 写死 macOS「系统设置 → 隐私与安全性」，Windows 玩家照着做不了）。
#   Windows 上「拒绝访问」多半是文件 / 文件夹正被别的程序拿着（资源管理器开着、自签工具还拿着上次的 S2Arm.ipa、杀毒软件在扫），
#   其次是「受控文件夹访问」（Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护）拦了往文档 / 桌面写
WINDOWS_RECOVERY = {
    "apple_not_signed_in": "Impactor 里还没登录 Apple ID：在导出页点「打开 Impactor」→「设置」→「添加账号」，用你的 Apple ID 登录（邮箱、密码、手机上的验证码，输入前切到英文输入法）；已经存了几个账号的，选中要用的那个（或移除后重新添加）。然后回来点这台的「重试」。密码只在 Impactor 里输，S2Arm 不经手。",   # Impactor 里没有选中的 Apple ID（从没登录 / 存了几个没选中，#edc5f5 从登录过期拆出）
    "permission_denied": "关掉打开着这个文件夹的资源管理器窗口和自签工具（如 Sideloadly），稍等杀毒软件扫完再重试；"
                         "若开了「受控文件夹访问」（Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护），把 S2Arm 加进「允许的应用」。",
    "input_permission": "关掉正在使用这个游戏文件的程序再重试；若开了「受控文件夹访问」（Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护），"
                        "把 S2Arm 加进「允许的应用」，或把游戏文件拷到别的文件夹后重新导入。",
}


# Apple TV 作业（tvInstall / tvExport / tvSaveBackup …，作业名以 tv 开头）同一个码的「下一步」：电视没有数据线、没有锁屏、不能用 AltStore 装，
#   照 iPhone 那句说会叫玩家去解锁手机、插数据线（D-NCDE / D-GZHR / D-3KDG 同批：「给这台 iPhone 签名」「手机没有锁屏」）
TV_RECOVERY = {
    "resource_transfer_failed": "让 Apple TV 开着、别让它休眠，和这台 Mac 连同一个网络，再点一次「装到 Apple TV」；已经传到电视上的部分不重传，电视上的 App 和存档都保留。",
    "signing_failed": "查看本任务日志里签名那一步的报错；确认 Xcode「设置 → 账户」里登录了你的 Apple ID、能给这台 Apple TV 签名，再试一次。",
    "device_install_failed": "让 Apple TV 开着、和这台 Mac 连同一个网络，再点一次「装到 Apple TV」；电视上原有的 App 和存档都保留。",
    "device_not_connected": "让 Apple TV 开着、和这台 Mac 连同一个网络，并在 Xcode「窗口 → 设备与模拟器」里和它配对过一次，再试一次。",
    "device_locked": "让 Apple TV 开着、别让它休眠，和这台 Mac 连同一个网络，再点一次「装到 Apple TV」。",   # 电视没有锁屏；推送层不对电视报这个码，兜底也不说「解锁」
}


# 指代「这一台」设备的玩家句用 {0} 占位设备类型（#1962，同 install.S_OFFLINE）：填这次认出的 iPad / iPhone（install.device_kind、
#   装机对象 info["deviceKind"]，作业上下文 c.device_kind），认不出填 DEVICE_ANY；不写死 iPhone
DEVICE_ANY = "iPhone / iPad"


def device_of(c):
    """这次作业认出的设备类型（MacPhone / WinPhone 认出时记在 c.device_kind）；没认出 → 空串"""
    kind = getattr(c, "device_kind", "")
    return kind if isinstance(kind, str) else ""


def on_device(text, kind=""):
    """玩家句里的 {0} 填设备类型；kind 为空（认不出 / 还没连）填 DEVICE_ANY"""
    return text.replace("{0}", kind or DEVICE_ANY)


def recovery(code, *, packaged, windows=None, tv=False, device=""):
    """这个码的「下一步」；指代这一台的句子里的 {0} 填 device（认不出填 DEVICE_ANY）"""
    if tv and code in TV_RECOVERY:
        return TV_RECOVERY[code]
    if not packaged and code in DEVELOPMENT_RECOVERY:
        return on_device(DEVELOPMENT_RECOVERY[code], device)
    if (os.name == "nt" if windows is None else windows) and code in WINDOWS_RECOVERY:
        return on_device(WINDOWS_RECOVERY[code], device)
    return on_device(RECOVERY.get(code, RECOVERY["job_failed"]), device)


class Failure(Exception):
    """message 是上屏的那句；internal 是只进诊断 detail（「复制错误摘要」带走）的内部原文：阶段、rc、构建号、红行。"""
    def __init__(self, message, *, code="job_failed", internal="", holder=None, facts=None, advice=None):
        super().__init__(message)
        self.advice = advice   # 这一次的「下一步」（带当场读数的，如磁盘满点名哪块盘、差多少）；None = 按码查 RECOVERY
        self.code = code
        self.internal = internal
        self.holder = holder   # library_busy：占着游戏库锁的任务（filelock 旁挂文件里记的那份），其余为 None
        self.facts = facts     # 机读的失败事实（cause_facts：改写失败的模块 / 缺口数 / 缺口类别），前端按字段显示，不解析中文；没有为 None


def library_lock_failure(holder, message, internal=""):
    """Keep ordinary lock contention unchanged; specialize only a locally proven dead/reused owner."""
    import filelock
    if filelock.holder_is_stale(holder) is True:
        return Failure("原任务进程已不在，但游戏库的锁仍被占用。重新连接或重新挂载游戏库所在磁盘后再试；不要手动删除锁文件。本次没有改动游戏文件。",
                       code="library_lock_stuck", holder=holder, internal=internal)
    return Failure(message, code="library_busy", holder=holder, internal=internal)


def _under(path, roots):
    # 两边都安全解析后再比（macOS 的 /var 实为 /private/var、外接盘挂载点软链：请求里记的与报错带的写法可能不同）。
    try:
        # core.resolve_path 已处理 Windows ERROR_UNRECOGNIZED_VOLUME：能安全解析时做真实路径比较；
        # A 盘 / 网络卷不可识别时不让二次诊断覆盖原始 OSError。
        from core import resolve_path
        p = os.path.normcase(os.fspath(resolve_path(path)))
    except (OSError, TypeError, ValueError):
        return False
    for r in roots:
        try:
            r = os.path.normcase(os.fspath(resolve_path(r)))
        except (OSError, TypeError, ValueError):
            continue
        if p == r or p.startswith(r.rstrip(os.sep) + os.sep):
            return True
    return False


def _is_directory(path):
    """Return None when the volume cannot be checked; do not mistake I/O errors for a missing library."""
    try:
        return stat.S_ISDIR(os.stat(path).st_mode)
    except FileNotFoundError:
        return False
    except OSError:
        return None


DISK_IO_WINERRORS = (483, 23, 1117)   # ERROR_DEVICE_HARDWARE_ERROR / ERROR_CRC / ERROR_IO_DEVICE
APP_CONTROL_BLOCKED = (4551, 1260, 225, 226)   # 同 winproc.APP_CONTROL_BLOCKED：应用控制策略 / 组策略 / 杀毒软件拦下要起的程序（#0745b9）


def explain(error, sources=(), library=None):
    """OSError 按 errno 归码，不猜原文；「原件不见了」「去系统设置授权」只给确知是玩家这边的路径（启动器剃刀 H8，11-11）：
    sources = 这次作业的原件（导入的路径、这款登记的原件），library = 游戏库根。
    ENOENT 出在原件下，或游戏库根整个不在（外接盘没插）→ input_missing；EACCES / EPERM / EROFS 出在原件或游戏库下 → permission_denied；
    其余（私有目录的日志 / 任务记录、并发竞态删掉的临时文件、不带路径的）→ job_failed，原文与路径进 internal（坑记录：两个作业同时收日志的
    ENOENT 被说成原件缺失，玩家被指去接磁盘）"""
    if isinstance(error, Failure):
        return error
    if isinstance(error, OSError):
        if error.errno in (errno.ENOSPC, errno.EDQUOT):
            return Failure("任务所在磁盘空间不足。", code="disk_full")
        paths = [p for p in (error.filename, error.filename2) if p]
        winerror = getattr(error, "winerror", None)
        internal = f"{type(error).__name__} errno {error.errno}" + (f" WinError {winerror}" if winerror else "") + f"：{error.strerror or error}" + ("；路径 " + " → ".join(map(str, paths)) if paths else "")  # 非玩家句
        # 盘读写出了硬件 / 连接故障（D-2NH2 Windows 0.6.8：installUpdate 读游戏库 WinError 483「设备硬件出现致命错误」，原先落笼统的「本地任务失败」）：
        #   同 disk_full 是确凿的环境原因。WinError 483 设备硬件错、23 CRC 数据错、1117 设备 I/O 错；POSIX EIO（移动硬盘掉线 / 坏道）
        if winerror in DISK_IO_WINERRORS or error.errno == errno.EIO:
            return Failure("这块盘读写出错：系统报告硬件或连接故障。", code="disk_io_error", internal=internal)
        if winerror in APP_CONTROL_BLOCKED:   # 先于 EACCES：4551 / 1260 也会被映射成拒绝访问，原因不同、办法不同
            tool = __import__("ntpath").basename(str(error.filename)) if error.filename else "S2Arm 自带的工具"   # ntpath：\\ 与 / 都认
            who = {4551: "Windows 的「智能应用控制」", 1260: "这台电脑的组策略（公司 / 学校管理的电脑常见）"}.get(winerror, "杀毒软件")
            return Failure(f"{who}拦下了 S2Arm 自带的工具「{tool}」，没让它运行。", code="app_control_blocked", internal=internal)
        if error.errno in (errno.EACCES, errno.EPERM, errno.EROFS):
            if any(_under(p, [*sources, *([library] if library else [])]) for p in paths):
                return Failure("文件或目录不可访问或不可写。", code="permission_denied", internal=internal)
            return Failure("本地任务失败：S2Arm 自己的文件读写不了，请导出诊断发给我们。", internal=internal)
        if error.errno == errno.ENOENT:
            library_is_dir = _is_directory(library) if library is not None else True
            if any(_under(p, sources) for p in paths) or library_is_dir is False:
                return Failure("任务所需的文件或目录不存在。", code="input_missing", internal=internal)
            return Failure("本地任务失败：S2Arm 自己的文件找不到了，请重试；还不行请导出诊断发给我们。", internal=internal)
        return Failure("本地任务失败，请检查日志和输入。", internal=internal)
    # 不是 OSError 的（多是我们自己的代码缺陷，如 #372 / #854 Windows 报修打包 AttributeError）：上屏原句不改（旧前端按它做本地化匹配），
    #   但异常类型、原文和出在我们哪个文件哪一行进 internal——原先整个丢掉，玩家复制的错误摘要里只剩「本地任务失败」，定不了因
    return Failure("本地任务失败，请检查日志和输入。", internal=_where(error))


def _where(error):
    """「AttributeError: 'x' object has no attribute 'y'（library.py:2837 in diagnostics）」：取调用栈里最后一帧落在 S2Arm 自己源码里的位置"""
    import traceback
    text = f"{type(error).__name__}: {error}"
    ours = Path(__file__).resolve().parents[2]
    frames = [f for f in traceback.extract_tb(error.__traceback__) if _under(f.filename, [ours])]
    if frames:
        f = frames[-1]
        text += f"（{Path(f.filename).name}:{f.lineno} in {f.name}）"
    return text


# 实际阶段或命令确定分类，不从错误 prose 猜编译器/输入的根因（日志里有确凿根因字样的先由 LOG_CAUSES 判，见 classify_failure）。
# direct（直连库出库，pack/stage_direct.sh → graphics/tools/direct/direct_lib.py）：内容缺口退 3 走黄、到不了这里；其余非零按 stage_direct.sh 注
#   是「基础设施出错」——dsh_batch（.NET）翻译 + Metal 编库，归编译失败（恢复口径＝检查 Xcode / Metal 组件 / .NET SDK）。
STAGE_CODES = {
    "unpack": "content_invalid", "configure": "configuration_failed",
    "rewrite": "rewrite_failed", "sh_extract": "shader_extract_failed",
    "host": "compilation_failed", "sign": "signing_failed",
    "install": "device_install_failed", "seed": "resource_preparation_failed",
    # direct（直连库：.NET 翻译 + Metal 编库 + 打包写库）不设阶段默认码：根因日志读不到时说不清是哪一步，
    #   按 package_failed 让玩家「复制错误摘要」发给我们（11-07 维护者：判不出不猜；发版应用也已不要 .NET SDK，「检查 SDK」会误导）
}

# 出包 / 准备失败上屏的那一句（11-07 维护者：阶段 id、rc、构建号、红行原文不上屏，只进复制诊断）。
# 口径取 launchers/shared/experience.v1.json errors 该码的 title / detail：detail 是「…具体原因见本任务的错误摘要」的只用 title，
#   否则「title：detail」（单测 test_job_failure_codes.PlayerSummary 逐条对照；英文在 Mac 启动器 Localizable.json）。
PLAYER_REASON = {
    "disk_full": "磁盘空间不够：准备这个游戏需要更多空余空间。",
    "disk_io_error": "这块盘读写出错：系统报告硬件或连接故障。",
    "out_of_memory": "电脑内存不够：出包时内存用完了。",
    "rewrite_gap": "暂时出不了包：这款游戏里有程序代码我们的改写器还不支持，我们正在修。",
    "glslc_windows_unavailable": "着色器编译暂不支持：这款游戏的着色器编译目前只能在 Mac 上完成。",
    "metal_unavailable": "缺少 Metal 组件：编着色器要用的 Metal 组件没装上。",
    "metal_download_unreachable": "下不了 Metal 组件：连不上苹果的组件服务器，Metal 编译组件下载不下来。",
    "package_verify_mismatch": "资源包核对没通过：出包最后核对资源包时，有文件前后两次读出的内容对不上。多见于磁盘或内存读写不稳定，或有杀毒、云同步软件在改游戏库里的文件。",
    "ios_platform_missing": "缺少 iOS 平台组件：Xcode 的 iOS 平台还没装，打不了 iPhone 包。",
    "engine_toolchain_newer": "这版 S2Arm 不认识你的 Xcode：你的 Xcode 比这版 S2Arm 新。",
    "graphics_api_unsupported": "这款游戏暂时出不了包：没认出它用的图形接口（NVN / Vulkan / OpenGL）。请把诊断发给我们，我们会核对是不是没认出来。",
    "game_structure_unsupported": "这款游戏暂不支持：出包工具还没适配这款游戏的程序结构。请把诊断发给我们，我们按它补上。",
    "tool_missing": "缺少工具：这条链上有个工具没装。",
    "content_invalid": "文件内容不对：文件结构校验没通过，可能没下完整。",
    "volume_ops_unsupported": "存放位置所在磁盘格式不支持：这块盘不支持出包需要的文件操作（常见于 exFAT / FAT 格式的外接盘）。",
    "signing_failed": "签名没成：用你的 Apple ID 给这台 {0} 签名没成功。",   # 出包链拿不到是哪台：player_reason 填「iPhone / iPad」
    "configuration_failed": "配置推导失败。",
    "shader_extract_failed": "着色器抽取失败。",
    "compilation_failed": "编译失败。",
    "rewrite_failed": "程序改写失败。",
    "resource_preparation_failed": "资源准备失败。",
    "device_install_failed": "设备安装失败。",
    "package_failed": "出包未完成。",
    "input_missing": "找不到游戏文件：选中的游戏文件已不在原处。",
    "windows_mandatory_aslr": "Windows 的强制 ASLR 开着：出包工具起不了子进程。",
    "build_stalled": "出包卡住了：有一步很久没有任何进展，S2Arm 已把它停下。",
    "app_control_blocked": "被 Windows 拦下：Windows 的应用控制或杀毒软件没让 S2Arm 自带的工具运行。",
    "win_metal_missing": "缺少Windows版Metal工具：未找到能报告有效版本的官方Windows版Metal编译器。",
}


def player_reason(code):
    return on_device(PLAYER_REASON.get(code, PLAYER_REASON["package_failed"]))


# 按日志确凿字样归因，先于阶段映射：按本表顺序，证据里第一个命中的码胜出（盘满最先：写盘失败后面常跟一串连带错误）。
# 只收工具 / 系统原样会打的字样，不从散文猜。
LOG_CAUSES = (
    (r"WinError 206|filename or extension is too long|文件名或扩展名太长", "windows_path_too_long"),  # 非玩家句
    # 起我方自带工具被拒（D-GVDA / #1912：Windows 0.6.3，出包「准备资源」起随包预编的 Yaz0 编码器 Binaries\Prepare\codecs\yaz0enc-*.exe 报 WinError 5；
    #   同一台机器建 RomFS 目录链接（软链、junction）也报 WinError 5，普通拷文件照常）：安全软件按行为拦下，同 4551 / 1260 / 225 / 226（winproc.APP_CONTROL_BLOCKED）
    #   归 app_control_blocked，玩家拿到「去安全中心 / 杀毒软件放行安装文件夹」那句，而不是笼统的「出包未完成」。编排层子进程里出的错只剩文字，按回溯原文认：
    #   起进程（subprocess _execute_child）被拒，或建目录链接（roots._junction）被拒
    (r"in _execute_child\b[\s\S]{0,400}?\[WinError (?:5|4551|1260|225|226)\]|in _junction\b[\s\S]{0,400}?\[WinError 5\]", "app_control_blocked"),  # 非玩家句
    # 回溯没落在读到的日志里、只剩编排中止那一行时（D-QNJQ）：roots._junction 把「在 _junction」附在异常上，同一行里认
    (r"编排中止：\[WinError 5\][^\n]{0,200}在 _junction", "app_control_blocked"),  # 非玩家句
    # 编着色器库前核工具读不到 Windows 版 metal.exe 的版本（translator/dmsh_chain.py tools_now，D-TYCN）：起进程被拒 → 被 Windows 拦下；
    #   文件不在 / 起来了没打版本 → 缺少 Windows 版 Metal 工具（去苹果开发者下载页装 / 重装）。原先落笼统的「出包未完成」加一句开发者口吻的 --update-lock 提示
    (r"metal_(?:macosx|iphoneos): lock .{0,120}? / actual missing \(start blocked", "app_control_blocked"),  # 非玩家句
    (r"metal_(?:macosx|iphoneos): lock .{0,120}? / actual missing", "win_metal_missing"),  # 非玩家句
    # 解包读原件时路径不在（pack/nx_unpack 打的分类行；诊断#24fb13：原先按阶段落成 content_invalid「文件没下完整」，原件其实是挪走了 / 盘没连上）
    (r"【原因 source_missing】", "input_missing"),  # 非玩家句
    # 随包 bash 起子进程刚建就退（#1762：MSYS 原文「dofork: child -1 - forked process … died unexpectedly, retry 0, exit code 0xC0000142」，连试 6 次）：
    #   Windows「强制映像随机化（强制性 ASLR）」开着，MSYS 的 fork 要在子进程同一地址装 DLL，被系统强制换址后起不来。玩家关回系统默认即可出包
    (r"dofork: child -1 - forked process \d+ died unexpectedly, retry \d+, exit code 0xC0000142", "windows_mandatory_aslr"),
    (r"No space left on device|\bENOSPC\b|\[Errno 28\]|\berrno\s*[=:]?\s*28\b"          # POSIX / Python OSError / .NET IOException（Mac）  # 非玩家句
     r"|There is not enough space on the disk|\bDisk full\b|WinError 112|ERROR_DISK_FULL|0x80070070"   # Windows / .NET
     r"|STATUS_DISK_FULL|磁盘空间不足|【写满】", "disk_full"),                                       # Windows 退出码注（direct_lib.py 127）/ 我方字样
    # 盘读写硬件 / 连接故障（D-2NH2）：Windows 原文（WinError 483 / 23 / 1117）、POSIX EIO 原文
    (r"WinError 483\b|WinError 23\b|WinError 1117\b|\[Errno 5\] Input/output error|STATUS_DEVICE_HARDWARE_ERROR|设备硬件出现致命错误|数据错误（循环冗余检查）", "disk_io_error"),  # 非玩家句
    # 内存用完（#711：Windows 16 GB 抽取按核数开满进程池，Python 子进程 MemoryError）：Python / .NET / Windows 提交上限原样字样
    (r"(?m:^\s*MemoryError\b)|OutOfMemoryException|\[WinError 1455\]|paging file is too small|页面文件太小|Cannot allocate memory|\bENOMEM\b", "out_of_memory"),  # 非玩家句
    # 盘不支持某种文件操作（exFAT 外接盘不支持原子改名 / 克隆 / 硬链接，实测 errno 45）：出包写入的这块盘格式不对，说清换 APFS 盘（11-10 网友：存放位置设到外接盘写入失败）
    (r"\[Errno 45\] Operation not supported|\bENOTSUP\b|Operation not supported on socket", "volume_ops_unsupported"),
    # cp 被系统拒绝（拷字体等，pack/stages/sysdata.sh 红行带 cp 原文）：macOS 隐私授权没给（桌面 / 文稿 / 下载 / 外接盘）或目录没写权限 → 提示去系统设置授权
    (r"\bcp: .*: (?:Operation not permitted|Permission denied)", "permission_denied"),
    # 直连库出库前核链号对不上：旧引擎（≤ beta.4，direct_lib.sh 把 dmsh_chain.py --build-env 的 stderr 一起收进链号）遇到 metal 比锁新的 Xcode，
    #   编译器提示「note: …」被当成链号读（eb4b5c964 已修）。玩家能做的就是更新 S2Arm
    (r"链号前后不一", "engine_toolchain_newer"),  # 非玩家句
    # 直连配置推导判不出图形接口（graphics/nvn/tools/direct_profile.py；gfx_api.py 只认动态导入的 NVN / Vulkan / EGL）：
    #   同人移植多把 deko3d / Mesa 静态链进程序（#222 程序号 05F4…，不是官方 0100 段），通用 GPU 模拟路径已拆（11-07 只有直连），这类按现行设计不支持
    (r"unsupported graphics API: unknown", "graphics_api_unsupported"),
    # v0.3.1 拿不准不停 ⑪ / ⑧：物理做不下去的照停，但要说人话。直连配置推导不认本款的程序结构（main 不是 NSO / 动态符号表 / RomFS 符号链接等；图形接口那类上一行先认）
    (r"配置推导不支持本款", "game_structure_unsupported"),  # 非玩家句
    # 直连库 Metal 一份都没出（Build.cs Metal.Tally）：这台机器上的 Metal 编译器用不了
    (r"Metal 库一份都没出", "metal_unavailable"),  # 非玩家句
    # 资源包核对（pack/verify.py）：包里那份读出的 SHA 和 pack.py 记进 index 的不一样——同一个文件（NTFS 硬链接 / 克隆）两次读出不同，
    #   多见于读盘不稳定或有程序在改游戏库里的文件；核对已让这几个文件下次重算（11-13 玩家报错原先落成未归类 package_failed）
    (r"SHA 与 index 不符", "package_verify_mismatch"),  # 非玩家句
    # 直连库原生库缺导出 / 缺库（Build.cs）：S2Arm 安装不全，不是游戏内容
    (r"原生库缺件（出包机环境", "tool_missing"),  # 非玩家句
    # 改写停住：v0.3.1 起有缺口照出产物、降级继续（pack/stage_rewrite.sh 记 degrade.rewrite.tsv），日志里留着「PFRW … gaps=N」不再说明失败在改写；
    #   只有缺口加上补丁区放不下（stage_rewrite.sh 这句红行）才是改写停住。按 PFRW 认会把之后别的阶段的失败错归成改写缺口
    (r"需要的改写空间超过了能放下的上限", "rewrite_gap"),  # 非玩家句
    # Windows 出包期 GLSLC 工具模式缺失（exitCode 75）
    (r"Windows 还没有 GLSLC 工具模式|要出包期离线编 GLSLC.*Windows", "glslc_windows_unavailable"),  # 非玩家句
    # 下载 Metal 编译组件（xcodebuild -downloadComponent MetalToolchain）拿不到苹果的资源目录（诊断#f5eae9：3 台 Mac，Xcode 27A266a / 27B47；
    #   同版本 Xcode 在能连上苹果的网络里照常下得下来）：玩家这边网络 / 代理到不了苹果的组件服务器。原先上屏只剩最后一行「})」
    (r"Failed fetching catalog for assetType \(com\.apple\.MobileAsset\.MetalToolchain", "metal_download_unreachable"),
    # xcodebuild 自己的插件加载不了（CoreSimulator 等系统组件没装上），苹果原文让跑 -runFirstLaunch = Xcode 首次设置没做完（同诊断#f5eae9 一台）
    (r"xcodebuild failed to load a required plug-in", "xcode_setup_required"),
    (r"unable to find utility \"metal(?:lib)?\"|cannot execute tool '?metal(?:lib)?'?|missing Metal Toolchain"  # 非玩家句
     r"|【红】环境缺件 metal：", "metal_unavailable"),   # 后者：Windows 出包预检（pack/orch/oneclick_win.py env_items）找不到 Metal Developer Tools
    (r"is not installed\. (?:To use with Xcode, first download and install the platform|Please download and install the platform)", "ios_platform_missing"),   # xcodebuild 原文（11-07 gate-host Xcode 26.6）
    (r"unable to find utility \"|: command not found|is not recognized as an internal or external command", "tool_missing"),
    # 无进展看门狗把某一步停了（tools/nostall.py，退 124）：放在最后——同一段证据里有写满 / 内存用完等确凿原因时那才是真因，看门狗只是结果。
    #   D-PD33（Windows 0.6.7）：编批库 15 分钟没有一批编完被停，原先落「package_failed，按阶段 未识别」，玩家只看到「这一步的失败还没归类」
    (r"【红】无进展看门狗：", "build_stalled"),  # 非玩家句
)
_RED = ("【红】", "🔴", "[RED]")  # 非玩家句
# 红行里点名的子日志（如直连库 <缓存>/work.stage.N/build.log）：绝对路径（可含空格，如「Application Support」），以 .log / .out 结尾
_LOG_PATH_ANY = re.compile(r"((?:[A-Za-z]:[\\/]|/)[^\s'\"（）()，,；;]+)")   # 命中行里点名的绝对路径（核盘满用：不限 .log）
_LOG_PATH = re.compile(r"(?:(?<=[\s（(：:，,=])|^)((?:[A-Za-z]:[\\/]|/)[^（）()，,；;\"'\r\n]*?\.(?:log|out))(?=$|[\s）)，,；;:\"'])")
EVIDENCE_MAIN_BYTES = 16000   # 主日志（refresh / 一键出包输出）尾部
EVIDENCE_SUB_BYTES = 32000    # 每份子日志尾部（.NET 未处理异常连调用栈约 4–10 KB）
EVIDENCE_SUB_FILES = 4        # 最多跟几份子日志（refresh.log → build.log 这样逐层跟）


def _tail_text(path, limit):
    try:
        with open(path, "rb") as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - limit))
            return stream.read().decode("utf-8", errors="replace")
    except OSError:
        return None


def _log_paths(text, red_only=True):
    import os
    for line in text.splitlines():
        if red_only and not any(mark in line for mark in _RED):
            continue
        for m in _LOG_PATH.finditer(line):
            raw = m.group(1)
            if os.name == "nt" and re.match(r"^/[A-Za-z]/", raw):   # MSYS 风格 /c/… 换成 Windows 路径
                raw = raw[1].upper() + ":" + raw[2:]
            yield raw


def gather_evidence(log, texts=()):
    """有界读取失败证据：调用方给的原因文本 + 主日志尾 EVIDENCE_MAIN_BYTES，再跟红行（原因文本整段）里点名的子日志，
    最多 EVIDENCE_SUB_FILES 份、每份尾 EVIDENCE_SUB_BYTES，子日志的红行可再往下跟。返回 [(来源, 文本)]。"""
    from pathlib import Path
    chunks = [("原因", t) for t in texts if t]  # 非玩家句
    queue = [p for t in texts if t for p in _log_paths(t, red_only=False)]
    seen = set()
    if log:
        seen.add(str(log))
        main = _tail_text(log, EVIDENCE_MAIN_BYTES)
        if main is not None:
            chunks.append((Path(log).name, main))
            queue += list(_log_paths(main))
    followed = 0
    while queue and followed < EVIDENCE_SUB_FILES:
        path = queue.pop(0)
        if path in seen:
            continue
        seen.add(path)
        text = _tail_text(path, EVIDENCE_SUB_BYTES)
        if text is None:
            continue
        followed += 1
        chunks.append((path, text))
        queue += list(_log_paths(text))
    return chunks


# 环境类的码（盘满、缺工具）字样只是候选：工具重试、可选探测（`command -v x || …`）都会打出同样字样却与这次失败无关
#   （坑记录 11-09：Windows 出库 rc=127 被一律译成盘满，玩家照提示删了 37 GB）。启动器剃刀 H9：候选要当场核事实才定码，核不实不采信，
#   字样与未采信的理由进 internal。其余码的字样只有出错的那个工具自己会打（改写缺口、链号、图形接口、预检缺件、xcrun / xcodebuild 原文），照旧直接定码
DISK_FULL_FREE = 5 << 30   # 盘满候选的核实线：命中行点名的路径或日志所在的盘，剩余空间低于它才算真满（工具失败后常会删掉自己的临时文件，留出余量）


def _free_bytes(path):
    """path（可不存在：往上找到存在的那层）所在盘的剩余字节；读不到为 None"""
    import os
    import shutil
    p = os.path.abspath(path)
    while p and not os.path.exists(p):
        parent = os.path.dirname(p)
        if parent == p:
            break
        p = parent
    try:
        return shutil.disk_usage(p).free
    except OSError:
        return None


# 漏洞利用保护的「强制映像随机化」（winnt.h PROCESS_CREATION_MITIGATION_POLICY_FORCE_RELOCATE_IMAGES_*，位 8–9：1 = 总开、3 = 总开且要求重定位）。
#   系统设置写在 Session Manager\kernel，单程序覆盖写在 Image File Execution Options\<程序名>，两处都是 MitigationOptions（REG_BINARY，小端）
ASLR_KEYS = (r"SYSTEM\CurrentControlSet\Control\Session Manager\kernel",
             r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\bash.exe")


def _force_relocate_on(blob):
    return isinstance(blob, (bytes, bytearray)) and len(blob) >= 2 and blob[1] & 3 in (1, 3)


def _mandatory_aslr_on():
    """强制 ASLR 开着 → 依据（哪一处开着）；读得到、都没开 → None。只在 Windows 读（不需要管理员）"""
    if os.name != "nt":
        return None
    import winreg
    for key in ASLR_KEYS:
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                blob, _ = winreg.QueryValueEx(k, "MitigationOptions")
        except OSError:
            continue
        if _force_relocate_on(blob):
            return "注册表 HKLM\\" + key + " 的 MitigationOptions 强制 ASLR 开着"  # 非玩家句
    return None


def _tool_present(name):
    import shutil
    return shutil.which(name) is not None


def _confirm(code, line, log, final=False):
    """候选码的事实核对 → (是否采信, 依据)。只核环境类的码，其余直接采信。
    final：命中行就是这段证据的最后一行（让这一步退出的那个异常，如回溯末行 OSError: [WinError 112]）——盘满就是真因，事后剩余空间不作数：
      失败后临时文件已删、空间又回来了（诊断 #9ac7f7：写的时候满，事后量还剩 17 GB，原先不采信、落到「没归类」）"""
    if code == "disk_full" and "【写满】" in line:   # 非玩家句 tools/clonedir.py 写满当时量的事实（目标盘剩多少、这一项要多少）：采信，事后剩余空间不作数（#1725）
        return True, "失败当时量：" + line.split("【写满】", 1)[1]  # 非玩家句
    if code == "disk_full" and final:
        free = _free_bytes(str(Path(log).parent)) if log else None
        return True, "这一步写满了盘（失败后临时文件已删" + ("，现在剩 %.1f GB" % (free / (1 << 30)) if free is not None else "") + "）"  # 非玩家句
    if code == "disk_full":
        paths = [m.group(1) for m in _LOG_PATH_ANY.finditer(line)] + ([str(Path(log).parent)] if log else [])
        free = [(p, f) for p, f in ((p, _free_bytes(p)) for p in paths) if f is not None]
        if not free:
            return True, "读不到所在盘的剩余空间，按字样"  # 非玩家句
        path, least = min(free, key=lambda pf: pf[1])
        gib = least / (1 << 30)
        if least < DISK_FULL_FREE:
            return True, "核实：%s 所在盘只剩 %.1f GB" % (path, gib)  # 非玩家句
        return False, "所在盘还剩 %.1f GB（%s），不是盘满" % (gib, path)  # 非玩家句
    if code == "windows_mandatory_aslr":   # 0xC0000142 也可能是别的原因（桌面堆耗尽、安全软件注入）：读注册表核实强制 ASLR 真开着才采信
        on = _mandatory_aslr_on()
        if on:
            return True, "核实：" + on  # 非玩家句
        return False, "强制 ASLR 没开（或读不到）"  # 非玩家句
    if code == "tool_missing":
        m = re.search(r"([\w.+-]+): command not found", line)
        if m and _tool_present(m.group(1)):
            return False, "「%s」现在找得到，不是没装" % m.group(1)  # 非玩家句
    return True, ""


def log_cause(chunks, log=None):
    """证据里有确凿根因字样、且（环境类的码）核实了 → (码, 来源, 命中行, 核对说明)；没有 → (None, 未采信的候选说明列表)。"""
    rejected = []
    for pattern, code in LOG_CAUSES:
        for label, text in chunks:
            m = re.search(pattern, text, re.I)
            if m:
                start = text.rfind("\n", 0, m.start()) + 1
                end = text.find("\n", m.end())
                line = text[start:end if end >= 0 else None].strip()[:300]
                final = not text[end:].strip() if end >= 0 else True   # 命中行后面没有别的输出 = 让这一步退出的那个错误
                ok, why = _confirm(code, line, log, final)
                if ok:
                    return code, label, line, why
                rejected.append("候选 %s 未采信：%s 里「%s」，%s" % (code, label, line, why))  # 非玩家句
                break   # 同一个码事实已核过，别的来源同样字样不再重复核
    return None, rejected


def refresh_stop_phase(red):
    """refresh.sh 的终止红行「【红】<款> 刷新停在 <子阶段>…」→ STAGE_CODES 的阶段；未登记的子阶段给 None（保持宽码，不从更早的红行猜）。"""
    stop = re.match(r"^【红】[^\r\n]+? 刷新停在\s*(.*)", red or "")  # 非玩家句
    if not stop:
        return None
    substep = stop.group(1)
    for prefix, phase in (("直连配置推导", "configure"), ("rewrite 阶段", "rewrite"), ("抽取阶段", "sh_extract"), ("direct 阶段", "direct")):  # 非玩家句
        if substep.startswith(prefix):
            return phase
    return None


_PFRW = re.compile(r"PFRW\s+mod=(\S+)\s+need=(\d+)\s+gaps=(\d+)(?:\s+fatal=(-?\d+))?")   # 改写器统计行（runtime/rewriter/rewriter.c 打的机读格式）
_GAP_KIND = re.compile(r"0x[0-9a-fA-F]+|\b[0-9a-f]{8}\b|#-?\w+|S\d_\d_C\d+_C\d+_\d|\b[xw]\d+\b")   # 同 core.write_rewrite_digest 的归类：去掉地址、寄存器号、立即数
GAP_KINDS_MAX = 5


def cause_facts(code, log, texts=()):
    """失败的机读事实（剃刀 G5 / H9）→ dict 或 None。改写类（rewrite_gap / rewrite_failed）：同一份有界证据（gather_evidence）里
    改写器自己打的最后一条统计行 PFRW → {"module", "need", "gaps", "fatal"}；证据里有 GAP 行再加 gapKinds = 前 GAP_KINDS_MAX 类 [{"kind", "count"}]（按次数降序）。
    其余码、证据里没有统计行 → None（不猜）"""
    if code not in ("rewrite_gap", "rewrite_failed"):
        return None
    chunks = gather_evidence(log, texts)
    last, kinds = None, {}
    for _, text in chunks:
        for m in _PFRW.finditer(text):
            last = m
        for line in text.splitlines():
            if line.startswith("GAP\t"):
                kind = _GAP_KIND.sub("_", line.split("\t", 3)[-1])[:80]
                kinds[kind] = kinds.get(kind, 0) + 1
    if not last:
        return None
    facts = {"module": last.group(1), "need": int(last.group(2)), "gaps": int(last.group(3))}
    if last.group(4) is not None:
        facts["fatal"] = int(last.group(4))
    if kinds:
        facts["gapKinds"] = [{"kind": k, "count": n} for k, n in sorted(kinds.items(), key=lambda kv: (-kv[1], kv[0]))[:GAP_KINDS_MAX]]
    return facts


DISK_FULL_REASON = "「{0}」空间不够：这一步还要约 {1} GB，盘上只剩 {2} GB。"
DISK_FULL_ADVICE = "在「{0}」上腾出至少 {3} GB 后重试；游戏原件、存档和上次成功的运行版本别删。"
# 出包前预检判这块盘够、出到一半却写满：原因里说清是我们预估偏低（#444：只说「空间不够」，玩家以为是自己盘的问题，清完再试照样失败）
DISK_FULL_LOW = "「{0}」空间不够：这一步还要约 {1} GB，盘上只剩 {2} GB。出包前估这块盘要 {3} GB、判的是够用：这次是 S2Arm 预估偏低，不是这块盘本来就不够。"
_FULL_REASON = re.compile("^" + re.escape(DISK_FULL_REASON).replace(r"\{0\}", "(?P<vol>.+?)").replace(r"\{1\}", "(?P<need>[\\d.]+)").replace(r"\{2\}", "(?P<free>[\\d.]+)") + "$")


def estimate_low(summary, budget):
    """盘满的上屏原因（DISK_FULL_REASON）：这次出包的空间预检（library.check_pack_space 存在 Context.space_budget）对同名那块盘判的是够 → 换成 DISK_FULL_LOW；
    预检本就说不够（玩家点过「仍然继续」等）、没有预检、原因不是这一句 → 原样"""
    m = _FULL_REASON.match(summary or "")
    if not m or not budget:
        return summary
    for v in budget.get("volumes", ()):
        if v.get("volume") == m["vol"] and v.get("enough"):
            return DISK_FULL_LOW.format(m["vol"], m["need"], m["free"], "%.0f" % (v["needBytes"] / 1e9))
    return summary
_FULL_NOTE = re.compile(r"【写满】.*?（盘「(?P<vol>[^」]+)」）所在盘剩 (?P<free>[\d.]+) GB，.+? 要 (?P<need>[\d.]+) GB")  # 非玩家句


def player_words(code, log=None, texts=()):
    """上屏的原因与下一步 → (原因, 下一步或 None)。磁盘满且证据里有 tools/clonedir.py 当场量的读数（盘名、这一项要多少、剩多少）：
    点名哪块盘、差多少（#1725：玩家只看到笼统一句，不知道清哪块盘、清多少）；差额 = 要 − 剩，向上取整到整 GB、最少 1。没有当场读数照旧笼统两句，不编数字"""
    if code == "disk_full":
        for _, text in gather_evidence(log, texts):
            m = _FULL_NOTE.search(text)
            if m:
                import math
                need, free = float(m["need"]), float(m["free"])
                gap = max(1, math.ceil(need - free))
                return (DISK_FULL_REASON.format(m["vol"], "%.1f" % need, "%.1f" % free),
                        DISK_FULL_ADVICE.format(m["vol"], "%.1f" % need, "%.1f" % free, gap))
    return player_reason(code), None


def missing_prebuilt(env=None):
    """随包预编件清单（S2A_PREBUILT_TOOLS/prepare.json，launchers/windows/bundle_prepare_win.py：逐文件大小 + sha256）里列着、盘上却没有或大小不对的文件
    → 相对路径列表；没有清单 / 读不了 → []。只在失败归因时查（启动前不做文件校验）。D-VPXS / #1912（Windows 0.6.3）：随包的 rewriter\\s2a-rewrite.exe
    不在了，改写阶段退回现编、报「CMake 未就绪」；同一台机器上另一个随包工具起进程被拒（D-GVDA）——杀毒软件隔离 / 清空了随包工具"""
    tools = (env or os.environ).get("S2A_PREBUILT_TOOLS")
    if not tools:
        return []
    root = Path(tools)
    try:
        files = json.loads((root / "prepare.json").read_text(encoding="utf-8"))["files"]
        items = list(files.items())
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return []
    gone = []
    for rel, entry in items:
        path = root / str(rel).replace("\\", "/")
        try:
            if not path.is_file() or (isinstance(entry, list) and entry and path.stat().st_size != entry[0]):
                gone.append(str(rel))
        except OSError:
            gone.append(str(rel))
    return gone


# 日志里有这些确凿结论时，随包件缺不抢归因（classify_failure）。win_metal_missing：Metal 编译器是玩家自己装的苹果 Metal Developer Tools，不在随包清单里，
#   随包件缺解释不了它起不来（#2278 / D-HA5D Windows 0.6.10：metal.exe 退出 0xC0000135，同时随包 rcodesign 记成缺 → 原先归 app_control_blocked，玩家关了防护照样失败）
UNRELATED_TO_PREBUILT = ("disk_full", "graphics_api_unsupported", "win_metal_missing")


def prebuilt_reason(rel, env=None):
    """随包件缺的一项写清是哪种：「<路径>（不在）」/「<路径>（大小 实际 / 应为 字节）」；读不到清单就只写路径（诊断说明用）"""
    tools = (env or os.environ).get("S2A_PREBUILT_TOOLS")
    try:
        want = json.loads((Path(tools) / "prepare.json").read_text(encoding="utf-8"))["files"][rel][0]
        path = Path(tools) / str(rel).replace("\\", "/")
        return f"{rel}（大小 {path.stat().st_size} / 应为 {want} 字节）" if path.is_file() else f"{rel}（不在）"  # 非玩家句
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        return str(rel)


def classify_failure(phase, log, texts=()):
    """Mac 准备（build_tool_failure）与 iPhone 打包（ipa.oneclick_failure，Mac / Windows 同一份）共用：
    日志确凿根因优先，其次阶段映射，判不出 package_failed。返回 (码, 写进 internal 的归因说明)。"""
    chunks = gather_evidence(log, texts)
    read = "；".join("%s（尾 %d 字节）" % (label, len(text.encode("utf-8"))) for label, text in chunks if label != "原因") or "无日志"  # 非玩家句
    gone = missing_prebuilt()
    shown = "、".join(prebuilt_reason(rel) for rel in gone[:6])
    hit = log_cause(chunks, log)
    # 日志确凿的结论与随包件无关时认日志，随包件缺只记进说明：
    #   ① 写满（log_cause 已按写满当时的量数 / 回溯末行 / 剩余空间核实）——盘满能让解出来的随包件大小不对，随包件缺却写不满盘
    #     （D-6ZDD Windows 0.6.7：写 game.img 时 [Errno 28]，随包 rcodesign 大小不对，原先先认随包件、玩家看到「被 Windows 拦下」）
    #   ② 判不出图形接口（纯 Python 的 gfx_api.py 读游戏程序得出的结论，不经任何随包工具）——D-TW4Z Windows 0.6.9：日志写着
    #     「unsupported graphics API: unknown」，同时 rcodesign 记成缺，原先归 app_control_blocked，玩家去安全中心找不到任何拦截记录
    if gone and hit[0] in UNRELATED_TO_PREBUILT:
        code, label, line, why = hit
        return code, "归因 %s：%s 里「%s」%s；随包预编件同时缺 / 大小不对 %d 个（%s），与这条结论无关，只记下；读过 %s" % (code, label, line, "（" + why + "）" if why else "", len(gone), shown, read)  # 非玩家句
    if gone:   # 随包工具不见了：后面各阶段的报错（退回现编缺编译器、起不来）都是它的连带，先认它
        return "app_control_blocked", "归因 app_control_blocked：随包预编件缺 / 大小不对 %d 个（%s），多半被杀毒软件隔离；读过 %s" % (len(gone), shown, read)  # 非玩家句
    if hit[0]:
        code, label, line, why = hit
        return code, "归因 %s：%s 里「%s」%s；读过 %s" % (code, label, line, "（" + why + "）" if why else "", read)  # 非玩家句
    code = STAGE_CODES.get(phase, "package_failed")
    return code, "归因 %s：按阶段 %s；%s读过 %s" % (code, phase or "未识别", "".join(r + "；" for r in hit[1]), read)  # 非玩家句


def tool_reason(log):
    """有界读取真实输出；原文在 Context 产生诊断时统一脱敏。"""
    if log:
        try:
            with open(log, "rb") as stream:
                stream.seek(0, 2)
                stream.seek(max(0, stream.tell() - 16000))
                lines = stream.read().decode("utf-8", errors="replace").splitlines()
            # 作业层自己往工具日志里追加的记录（[s2a] 开头，如收掉残留进程）不是工具的报错，不当失败原因（#711：真报错被它盖住）
            nonempty = [line.strip() for line in lines if line.strip() and not line.strip().startswith("[s2a] ")]
            red = [line for line in nonempty if any(mark in line for mark in _RED)]  # #1411：读取器打的是「🔴」，原先只认「【红】」，摘要取到末行「第 0 段数据 通过」
            if nonempty:
                return (red or nonempty)[-1][-1500:]
        except OSError:
            pass
    return "工具未提供错误详情，请保留本阶段日志。"


CAUSE_BYTES = 2400   # 失败原文行按字节封顶（UTF-8 按字节截、不截半个字）


def cause_line(log):
    """这一步失败的原文：日志末尾最后一行红行；它是 refresh 的终止转述行（「【红】<款> 刷新停在 <阶段>…」）就取它前一行红行——
    那是失败阶段自己的红行，带失败子命令的名字、退出码与输出原文（如直连库「…出库失败 rc=1，[直连库] 🔴 …：dsh_batch.py 退出码 1：<原文>」）。
    没有红行 → None。#1725 / #1730 / #1744：判不出原因落 package_failed 时，错误摘要只剩「出包未完成。」，原文只在日志里"""
    if not log:
        return None
    try:
        with open(log, "rb") as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - 16000))
            lines = stream.read().decode("utf-8", errors="replace").splitlines()
    except OSError:
        return None
    red = [l.strip() for l in lines if l.strip() and not l.strip().startswith("[s2a] ") and any(mark in l for mark in _RED)]
    if not red:
        return None
    stop = lambda l: "刷新停在" in l  # 非玩家句
    if stop(red[-1]):
        # 终止转述行之前、上一次终止行之后（同一轮刷新里）最后一行非终止的红行；没有就不另给（旧轮的红行不归给本轮）
        prev = [l for l in red[:-1]]
        cut = max((i for i, l in enumerate(prev) if stop(l)), default=-1)
        mine = [l for l in prev[cut + 1:] if not stop(l)]
        if not mine:
            return None
        line = mine[-1]
    else:
        line = red[-1]
    b = line.encode("utf-8")
    return line if len(b) <= CAUSE_BYTES else b[:CAUSE_BYTES].decode("utf-8", errors="ignore") + "…"


def last_output_lines(log, limit=5):
    """日志末尾最有用的几行（红行优先，保持原顺序），每行截到 300 字；脱敏由调用方（Context.redact）做"""
    if not log:
        return []
    try:
        with open(log, "rb") as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - 16000))
            lines = [line.strip() for line in stream.read().decode("utf-8", errors="replace").splitlines() if line.strip()]
    except OSError:
        return []
    reds = [i for i, line in enumerate(lines) if "【红】" in line or "error" in line.lower()][-2:]  # 非玩家句
    keep = sorted(set(reds) | set(range(max(0, len(lines) - (limit - len(reds))), len(lines))))
    return [lines[i][-300:] for i in keep]


def failure_summary(stage, exit_code, lines):
    """错误摘要里的失败事实（英文标签，中英文玩家转发都能读；原文已脱敏）。前端原样显示这段"""
    out = ["Failed stage: " + (stage or "unknown")]
    if exit_code is not None:
        out.append("Exit code: %s" % exit_code)
    if lines:
        out += ["Last output:"] + ["  " + line for line in lines]
    return "\n".join(out)


def build_tool_failure(args, milestone, log):
    """只供 Mac build 的非零退出分支调用；其余 operation 的 run 契约不变。"""
    from pathlib import Path
    executable = Path(args[0]).name
    script = Path(args[1]).name if len(args) > 1 else ""
    phase = None
    reason = tool_reason(log)
    if executable == "codesign":
        phase = "sign"
    elif executable == "make" and "host" in args[1:]:
        phase = "host"
    elif executable in ("bash", "sh"):
        if script == "seed_root.sh":
            phase = "seed"
        elif script == "refresh.sh":
            # 只采用本次 refresh 的终止红行（日志末条红行），不允许 host/unpack/sign 等上一条命令的标签参与本次归因。
            phase = refresh_stop_phase(reason) or "refresh"
    elif (
        executable == "nx_unpack" and milestone == "unpack"
    ):  # 读取器现为自包含单文件直执行（原先经 dotnet 跑 dll，executable 是 dotnet）
        phase = "unpack"
    code, why = classify_failure(phase, log)
    said, advice = player_words(code, log)
    cause = cause_line(log)   # 失败阶段自己的红行（带子命令原文）：终止转述行之外另记一行，错误摘要按「失败原文：」取（report.build）
    more = "\n失败原文：" + cause if cause and cause != reason else ""  # 非玩家句
    # 上屏只放玩家话术；阶段 / rc / 构建号 / 红行原文进 internal → 诊断 detail（「复制错误摘要」带走）；机读事实进 facts（前端按字段显示）
    return Failure(said, code=code, advice=advice, internal="Mac 准备失败（" + (phase or "未识别阶段") + "）：" + reason + more + "\n" + why,  # 非玩家句
                   facts=cause_facts(code, log))
