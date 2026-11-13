# Documentation

## For players

- [Quick start](../README.md#quick-start): download, build, play.
- [Graphics guide](graphics.md): the presets and what each option does, texture packs, what helps when the game is slow.
- [Troubleshooting and FAQ](troubleshooting.md): SmartScreen, where files are, updating, build failures, stutter.
- [Roadmap](roadmap.md): what each version shipped and what comes next.
- [Changelog](../CHANGELOG.md).

## For developers

- [Development](development.md): building from source, how it works, the repository layout, the research setup.
- [`recomp/README.md`](../recomp/README.md): the recompile, build and test commands, and every runtime option.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md): how to contribute, the checks a change must pass, adding a language.

## Development history

How the ports were made, phase by phase. Pages marked (Hebrew) are the original development notes; images of the game
are not in the repository, so those pages name them in a short note instead.

| Page | Topic |
|---|---|
| [feasibility-static-recomp.md](dev-history/feasibility-static-recomp.md) (Hebrew) | Feasibility study for static recompilation |
| [phase1-prototype.md](dev-history/phase1-prototype.md) (Hebrew) | The recompiler prototype |
| [phase2-threads-fs.md](dev-history/phase2-threads-fs.md) (Hebrew) | Thread scheduler and file system |
| [phase3-vfpu.md](dev-history/phase3-vfpu.md) (Hebrew) | VFPU (vector unit) translation |
| [phase4-ge.md](dev-history/phase4-ge.md) (Hebrew) | GE display lists and the software renderer |
| [phase5-window-input.md](dev-history/phase5-window-input.md) (Hebrew) | Window, input, getting in-game |
| [phase6-gpu-renderer.md](dev-history/phase6-gpu-renderer.md) (Hebrew) | Hardware renderer on SDL3 GPU |
| [renderer-decision.md](dev-history/renderer-decision.md) (Hebrew) | Decision: a hardware renderer for 60 FPS |
| [phase7-audio.md](dev-history/phase7-audio.md) (Hebrew) | Sound: sceAudio, SAS, ATRAC3+ |
| [phase8-movies.md](dev-history/phase8-movies.md) (Hebrew) | Movies: PSMF, H.264, ATRAC3+ |
| [phase9-saves-replay.md](dev-history/phase9-saves-replay.md) (Hebrew) | Saves, input recording and replay |
| [phase10-ghost-of-sparta.md](dev-history/phase10-ghost-of-sparta.md) (Hebrew) | Ghost of Sparta |
| [phase11-level-tour.md](dev-history/phase11-level-tour.md) (Hebrew) | Automated tour of every Chains of Olympus level |
| [phase12-gos-level-tour.md](dev-history/phase12-gos-level-tour.md) (Hebrew) | Automated tour of every Ghost of Sparta level |
| [phase13-graphics.md](dev-history/phase13-graphics.md) (Hebrew) | Graphics upgrades |
| [phase14-plan.md](dev-history/phase14-plan.md) | Plan for the one-download launcher |
| [phase14-launcher.md](dev-history/phase14-launcher.md) | One-file launcher acceptance |
| [phase15-portable-toolchain.md](dev-history/phase15-portable-toolchain.md) | Building without Visual Studio |
| [progress.md](dev-history/progress.md) (Hebrew) | Progress log |

Images in `docs/images/` show only the launcher, never the game.
