# Wardogs / FGTRACK Source and Extracted Materials Archive

[中文介绍](README.md) · [Download all archives](https://github.com/simplerjiang/wardogs-research/releases/tag/archive-20261117)

This repository collects local Wardogs projects, extracted FGTRACK components and research records, and the Kong Assistant UI revision. The snapshot date is **November 17, 2026, Asia/Singapore**.

Source snapshots are available directly in Git. Larger executable samples, public certificates, assembly listings, static-analysis exports, and historical evidence are provided as Release assets. “Source” refers to the local implementation projects and extracted web templates; **this archive does not claim to contain the complete original source code of closed-source third-party software such as FGTRACK**.

## Source layout

| Path | Contents and provenance |
| --- | --- |
| [source/wardogsE](source/wardogsE) | Current CoreWD project snapshot: overlay, kernel driver, historical mapper, controlled memory/render tests, and existing documentation. Source commit: `7c73400a4873c0c86f54fa78e5eeba238c52e193`. |
| [source/wardogsE-reference](source/wardogsE-reference) | Retained reference-reconstruction experiment, including `reconstruction/`. Source commit: `09114169ce5ba61f18c28091da54c0b64b102a32`. Its presence does not imply integration into the main project. |
| [source/wardogsv3](source/wardogsv3) | WardogLauncher C++ reconstruction, test driver, self-test tools, case configuration, and overlay code. This captures the local files, including changes not yet committed upstream. |
| [source/kong-ui](source/kong-ui) | Launcher/menu HTML and CSS, extracted original templates, UI build scripts, design assets, and existing UI verification records from the `codex/kong-ui-20261113` working copy. |

Each project retains its original documentation and build scripts. Hardcoded VS/SDK/WDK locations and historical evidence paths reflect their original environments. This archival task did not rebuild or execute the software.

## Extracted materials and downloads

The [archive-20261117 Release](https://github.com/simplerjiang/wardogs-research/releases/tag/archive-20261117) provides:

| Asset | Contents |
| --- | --- |
| `wardogs-sources-20261117.zip` | A complete snapshot of this repository's `source/` directory. |
| `fgtrack-complete-20261026.zip` | The complete materials collection: original investigation archive, preserved software samples, public certificates, FgFrame assembly/render evidence, historical loading records, provenance inputs, and static UI exports. |
| `fgtrack-outer-20261113.zip` | The unmodified extracted outer assistant, `FgGear32404.exe`, with provenance and identity records. The number in the filename is not a verified product version. |
| `kong-assistant-20261113.zip` | Earlier Kong Assistant UI derivative, UI source package, and existing acceptance records. |
| `kong-assistant-20261114.zip` | Revised UI derivative, source package, usage document, and existing acceptance screenshots. |
| `fgtrack-attribution-20261026.zip` | Follow-up attribution research, static audits, assembly comparisons, checkpoint records, and analysis scripts, excluding whole-machine filesystem inventories. |
| `FILE-MANIFEST.json` / `SHA256SUMS.txt` | Per-file identities and SHA-256 checksums for the downloadable archives. |

Archives use ZIP format. A normal `git clone` retrieves the repository files and does not automatically download Release assets.

## Verification limits

- Source copies and SHA-256 identities were checked during packaging; ZIP contents and published assets receive integrity checks. These checks establish archival integrity only.
- Existing CoreWD records include controlled user-mode memory and rendering tests. This publication adds no new proof of kernel loading, complete game functionality, or long-term stability.
- Extraction established file identity and byte-for-byte copying. It did not establish complete standalone operation outside the original authorization and launch chain.
- Kong Assistant records primarily cover UI behavior and the static extent of changes. Full authorized startup, game integration, and native menu capture remain incompletely verified.
- Historical files may describe different dates, machines, or branches. Read their scope and evidence boundaries individually; earlier observations are not guarantees for every current environment.

## Provenance, exclusions, and rights

See the [file manifest](docs/FILE-MANIFEST.json), [asset inventory](docs/RELEASE-ASSETS.json), [exclusion inventory](docs/EXCLUSIONS.json), and [archival notes](docs/ARCHIVE-NOTES.md). Git internals, WebView login/Cookie/test profiles, local build output, Python caches, shortcuts, private credential containers, and whole-machine filesystem inventories are excluded. The original local materials have not been deleted.

Third-party software, drivers, certificates, SDKs, icons, and other assets remain subject to their original rights. This repository does not apply a blanket MIT or other open-source license to those materials. Existing license notices are retained with their assets. Public accessibility does not itself grant unrestricted redistribution or relicensing rights.
