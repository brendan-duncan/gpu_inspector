---
description: Compare two GPU Inspector captures, before and after a change
argument-hint: "<before capture> <after capture>"
---

Compare two GPU Inspector captures of the same application: `$ARGUMENTS` names the capture before a
change, then the one after. Each is a path or an open capture's id.

1. **Find the two captures.** If `$ARGUMENTS` does not name two, call `list_captures`. Propose the
   two most recent files GPU Inspector saved (older one as "before"), and ask the user to confirm.
2. **Compare.** Call `compare_captures` with `before` and `after`.
3. **Check the captures are comparable.** Call `get_capture_summary` on each for context: same
   frame content, both profiled, similar pass structure. If one was not profiled, or the pass lists
   barely match, say what that means for the comparison.
4. **Report.**
   - The frame time and GPU time change.
   - The passes that got faster or slower, largest change first, with overdraw and fragments per
     primitive where they moved.
   - Frame Issues that appeared or went away.
   - Validation changes.

   Changes of a few percent between captures of a live application are usually noise: say so rather
   than crediting the change. If the change was meant to fix something specific, say whether the
   number it should have moved did.
