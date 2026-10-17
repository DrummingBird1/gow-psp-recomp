# Security policy

## Supported versions

Only the [latest release](https://github.com/DrummingBird1/gow-psp-recomp/releases/latest) gets fixes.

## Reporting a vulnerability

Please report security problems privately through
[GitHub's private vulnerability reporting](https://github.com/DrummingBird1/gow-psp-recomp/security/advisories/new),
not in a public issue. Include the version, what you found and how to reproduce it. You will get an answer as soon
as possible.

Relevant areas: the launcher's downloads (all pinned to SHA-256 hashes), how it starts the game, and parsing of disc
images and save files in the runtime.

## Checking your download

Each release lists the SHA-256 of the ZIP and of `GoWLauncher.exe`, and from v1.1.5 attaches `SHA256SUMS.txt`.
Only download the launcher from this repository's releases.
