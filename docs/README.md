# Documentation

Most players only need the [README](../README.md) and the [troubleshooting and FAQ](troubleshooting.md) page. The rest
documents how the ports were made, phase by phase. Pages marked (Hebrew) are the original development notes.

## For players

- [Troubleshooting and FAQ](troubleshooting.md): SmartScreen, where files are, updating, build failures, stutter.
- [Graphics options](phase13-graphics.md) (Hebrew): internal resolution, MSAA, HDR, frame rates, aspect ratios, texture packs.
- [Launcher acceptance test](phase14-launcher.md): what was tested for the one-file launcher.
- [Building without Visual Studio](phase15-portable-toolchain.md): the portable toolchain the launcher downloads.
- [Changelog](../CHANGELOG.md).

## Development history

| Page | Topic |
|---|---|
| [feasibility-static-recomp.md](feasibility-static-recomp.md) (Hebrew) | Feasibility study for static recompilation |
| [phase1-prototype.md](phase1-prototype.md) (Hebrew) | The recompiler prototype |
| [phase2-threads-fs.md](phase2-threads-fs.md) (Hebrew) | Thread scheduler and file system |
| [phase3-vfpu.md](phase3-vfpu.md) (Hebrew) | VFPU (vector unit) translation |
| [phase4-ge.md](phase4-ge.md) (Hebrew) | GE display lists and the software renderer |
| [phase5-window-input.md](phase5-window-input.md) (Hebrew) | Window, input, getting in-game |
| [phase6-gpu-renderer.md](phase6-gpu-renderer.md) (Hebrew) | Hardware renderer on SDL3 GPU |
| [renderer-decision.md](renderer-decision.md) (Hebrew) | Decision: a hardware renderer for 60 FPS |
| [phase7-audio.md](phase7-audio.md) (Hebrew) | Sound: sceAudio, SAS, ATRAC3+ |
| [phase8-movies.md](phase8-movies.md) (Hebrew) | Movies: PSMF, H.264, ATRAC3+ |
| [phase9-saves-replay.md](phase9-saves-replay.md) (Hebrew) | Saves, input recording and replay |
| [phase10-ghost-of-sparta.md](phase10-ghost-of-sparta.md) (Hebrew) | Ghost of Sparta |
| [phase11-level-tour.md](phase11-level-tour.md) (Hebrew) | Automated tour of every Chains of Olympus level |
| [phase12-gos-level-tour.md](phase12-gos-level-tour.md) (Hebrew) | Automated tour of every Ghost of Sparta level |
| [phase13-graphics.md](phase13-graphics.md) (Hebrew) | Graphics upgrades |
| [phase14-plan.md](phase14-plan.md) | Plan for the one-download launcher |
| [phase14-launcher.md](phase14-launcher.md) | One-file launcher acceptance |
| [phase15-portable-toolchain.md](phase15-portable-toolchain.md) | Building without Visual Studio |
| [progress.md](progress.md) (Hebrew) | Progress log |

Images in `docs/images/` show only the launcher, never the game.
