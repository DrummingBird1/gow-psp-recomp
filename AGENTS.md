# Repository instructions for coding agents

## Project scope

This repository contains an Android ARM64 port of two PSP God of War recompilations:
- Chains of Olympus: `UCUS98653`
- Ghost of Sparta: `UCUS98737`

The supported Android product is the **Dual APK**: both games are selected from one launcher and use separate native libraries and per-game storage. The user has confirmed that both games run in the same installed app on their device.

## Build and workflow policy

- Primary workflow: `.github/workflows/android-dual-game.yml`.
- Legacy single-game workflows are manual diagnostics only; do not add broad push triggers to them.
- `.github/workflows/android-fiber-check.yml` is a focused compile check for ARM64 fiber/runtime changes.
- Do not claim an Actions artifact is a published release. The in-app updater reads GitHub Releases, not Actions artifacts.
- Increment the launcher `VERSION` only when preparing the matching published release tag and APK.

## Copyright and sensitive game data

Never commit, upload, or distribute game ISOs, CSOs, EBOOT/PRX files, `image.bin`, decrypted game data, memory dumps, or other copyrighted game content. Do not put such files in repository releases or public artifacts. CI may consume user-provided private disc links through GitHub Actions secrets only when the workflow is explicitly designed for that purpose. Verify that packaging outputs contain only the intended app/runtime assets.

## Safe Git operations

- Never force-push, rewrite history, or delete existing branches/tags.
- Inspect the current branch and working tree before changes.
- Prefer small, reviewable commits with clear messages.
- Do not change runtime/gameplay code when the task is documentation or workflow organization.
- Do not claim device validation unless the user actually tested the resulting build.

## Reporting

Be concise and report:
1. files changed,
2. commit and branch,
3. workflow run result,
4. artifact/release distinction,
5. any remaining device-side check.
