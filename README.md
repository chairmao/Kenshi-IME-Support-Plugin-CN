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
