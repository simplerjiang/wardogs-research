# 归档说明 / Archival notes

## 快照 / Snapshot

Date: 2026-11-17 (Asia/Singapore).

`source/wardogsE` reflects local `main` at `7c73400a4873c0c86f54fa78e5eeba238c52e193`. The controlled-memory-render and reference-parity worktrees point to that same commit. The separate reconstruction snapshot reflects `09114169ce5ba61f18c28091da54c0b64b102a32`.

The original private `wardogsv3` remote was fetched before packaging. It had `main` and `wardog`, with no `dev`; `origin/main` was `b3f36636f95db5ed613e90e61ae6564b89e9b3e4`. A dedicated `codex/github-public-20261117` worktree was created from that remote main. The public archive is initialized with a separate Git history so that unrelated private history is not copied automatically.

`source/wardogsv3` captures the current local files, including uncommitted changes. `source/kong-ui` captures the retained UI worktree at `ffe738b`. These are preserved snapshots, not a claim that all experiments were integrated or accepted.

## 包含与排除 / Included and excluded

All selected application source files are copied without source-code edits. The complete FGTRACK collection already contains the earlier `FGTRACK-20261026-01` investigation, so that earlier directory is not duplicated as a second download. Both Kong UI exports and the later attribution collections are retained in Release assets.

The exclusion inventory records each omitted file. Runtime browser profiles, credentials containers, local build outputs, cache files, local shortcuts, and whole-machine filesystem directory inventories are not publication content. Public PEM/DER certificates are retained; these do not contain signing private keys.

Existing manifests inside historical archives describe the original exports and may still list files excluded from this public snapshot. Use this archive's `FILE-MANIFEST.json` and `EXCLUSIONS.json` for the current publication scope. Existing historical documents are preserved as dated records, including their original paths and pending-work statements.

## 完整性 / Integrity

`FILE-MANIFEST.json` records a SHA-256 and byte size for every source file and every file included in a Release ZIP. Locations beginning with `source/` are repository paths. Locations beginning with `releases/` identify an archive followed by its ZIP entry path.

`RELEASE-ASSETS.json` lists each ZIP's size, entry count, and SHA-256. The Release also includes `SHA256SUMS.txt`, covering every ZIP and the exported file manifest. Archive integrity checks do not establish software functionality, authorization independence, or driver compatibility.
