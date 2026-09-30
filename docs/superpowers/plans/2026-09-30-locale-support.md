# Issues 22 and 24: game language and host locale compatibility

Approved direction: retain automatic game language selection, add an independent
game language selector, and prevent unsupported host locales from aborting startup.

## Design

- Keep strict Xbox mapping functions for callers that need validation. Resolve host
  values at the runtime boundary with English / US fallbacks and explicit logging.
- Share the six disc language codes (en, ja, de, fr, es, it) between the launcher
  and runtime. Local assets include E/J/G/F/S/I font and dialogue packs.
- Persist `game_language=auto` separately from launcher `language`. Explicitly
  pass `SFR_GAME_LANGUAGE=auto` or a supported code, overriding inherited values.
- Put Game language in the Game tab with English and Traditional Chinese help.
  Apply on restart; retain all old launcher settings and original game selection.
- Unknown manual values revert to automatic. Unsupported host values retain raw
  diagnostic information while selecting the documented fallback.

## Implementation and validation

1. Add failing host resolution and launcher round-trip/environment regressions.
2. Implement host boundary resolution, shared language choices, and runtime logs.
3. Wire the launcher dropdown and update both README language instructions.
4. Build runtime/launcher, run targeted then full native tests. Check Spanish
   startup and original game language selection against English/automatic runs.
5. Prepare an isolated local test package; preserve existing settings/saves.
   Review the diff and report evidence and remaining hardware/platform limits.

No release publication, Issue replies or closure are part of this change.
