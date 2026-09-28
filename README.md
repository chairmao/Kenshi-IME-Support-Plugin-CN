# IME (Input Method Editor) Support for Kenshi
---
This mod adds Japanese IME (Input Method Editor) support to Kenshi, allowing players using Japanese input methods such as Microsoft's / Google's IME to type Japanese characters directly into in-game text fields. Without this mod, Japanese text input is not functional in Kenshi, forcing users to type in an external application like Notepad and copy-paste the result.

---
## What it does
When you type using a Japanese IME, the mod
- intercepts the composition input
- waits for you to confirm your selection
- injects the finalized characters directly into the active MyGUI text field. 

**Standard Latin keyboard input is completely unaffected.**

## Compatibility
- Works with the current version of Microsoft IME, but if you're having issues, set Microsoft IME to the previous version for compatibility.
- Works with TSF-based IME's like Google Input Tools (Google IME)

This should also work with other languages that use IMEs, like Chinese and Korean, though this has not been personally tested.

## Compilation
To compile this mod, you will need the following dependencies:
- Microsoft Visual C++ 2010 Build Tools
- KenshiLib

## Notes
This mod was made in response to feedback from Japanese-speaking players who were unable to type native characters into the game, and I'd like to thank user @Momayo for helping facilitate communication with Kenshi's Japanese-speaking community.

---
# 原项目 / Original project

本仓库是下面这个原项目的分支（fork），**原作者是 Genpretz**，原项目 git 地址：

**`https://github.com/Genpretz/Kensh_IME_Support_Plugin.git`**
（网页：<https://github.com/Genpretz/Kensh_IME_Support_Plugin>）

上游提交历史完整保留（`git log` 可查），原始 `LICENSE` 与作者署名均未改动。

This repository is a fork of **[Genpretz/Kensh_IME_Support_Plugin](https://github.com/Genpretz/Kensh_IME_Support_Plugin)**
&mdash; original git address: `https://github.com/Genpretz/Kensh_IME_Support_Plugin.git`.
The upstream history is preserved as-is.

## 本分支的改动 / Changes in this fork

面向**中文输入法**（微软拼音、搜狗等；日文/韩文同样受益）的稳定性修复：

1. **候选上屏不再丢字/重复上屏**：一次 IME 提交会经多条窗口消息送达，原实现会把同一个字符注入两次或多条消息互相顶掉。
   现在用"双消息记账"（`RememberChar` / `ConsumeTwin`，250 ms 提交保护窗口）保证一次提交只上屏一次。
2. **数字与空格改为延迟注入**：这两个键既是游戏按键又可能属于 IME 候选选择，冲突时会互相抢占。
   现在先入队，由 80 ms 计时器（`IME_FLUSH_TIMER_ID` / `IME_FLUSH_DELAY_MS`）在**游戏线程**上经子类化窗口过程刷出；
   若随后发生了真正的字符提交，则用 `PurgeQueuedDigitsAndSpaces()` 丢弃这些暂存键，避免把候选选择键误打进文本框。
3. **新增"最近一次 CJK 上屏后 5 秒内仍视为 IME 使用中"的判定**（`IME_CJK_ACTIVE_MS`），降低用纯 ASCII 判断导致的误判。
4. **可开关的诊断日志**（`IME_DIAG_LOG`，默认 0 关闭；早前开启时曾导致丢字，故默认不写日志）。
5. **构建可移植性**：依赖属性表（KenshiLib / boost）改为**条件导入**，仓库内附一份 `KenshiMods.Deps.props` 副本；
   若依赖放在别处，改该文件里的 `KenshiDepsRoot` 一行，或 `msbuild /p:KenshiLibDir=... /p:BoostDir=...`。
6. 新增 `IME_Support_Plugin.sln`，方便直接用 VS 打开。

改动文件：`src/ime_hook.cpp`（+583/-26）、`Kenshi IME Support Plugin.vcxproj`、新增 `KenshiMods.Deps.props`、`IME_Support_Plugin.sln`。
具体改动内容与时间见 git 提交历史。

## 许可证 / License

原项目为 **GNU GPL-3.0**；本分支**同样以 GPL-3.0 发布**，完整许可证见 `LICENSE`。
分发时请保留原作者署名与许可证，并说明本分支的修改（即本文件本节）。

