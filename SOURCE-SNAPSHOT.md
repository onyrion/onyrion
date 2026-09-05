# Source snapshot provenance

Generated: `2026-09-05T21:28:20Z`

## Core

HEAD: `9cb37a698b05fd45297177f2d7cd31f297e36668`

```text
 M include/action.h
 M include/config.h
 M include/layer_shell.h
 M include/server.h
 M include/window.h
 M src/action.c
 M src/config.c
 M src/input.c
 M src/layer_shell.c
 M src/window.c
```

## Shell

HEAD: `cec69f6fc853a0b57cfd4766982caaec1adaa12c`

```text

```

Snapshot policy:

- current working-tree bytes of tracked files are copied;
- deleted tracked files remain absent;
- untracked non-ignored files are a hard blocker for this prepare script;
- ignored build/cache output is excluded;
- nested `.git` metadata is excluded;
- source repositories are not modified.
