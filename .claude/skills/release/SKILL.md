---
name: release
description: Create a new BookBook version and publish it. Use when the user says things like "create a new version and publish it", "cut a release", "release this", or "publish a new version": commits what is uncommitted, picks the version, tags, builds, pushes, creates the GitHub release and publishes the firmware to Azure so units can update over the air.
---

# Release BookBook

The user saying this is authorization for the whole chain: commit, tag, push, GitHub release, and publishing the
firmware to the Azure container. Do not stop to ask about each step. Do stop if a step fails, and say plainly what
did and did not happen.

The version string inside the firmware comes from `git describe` at build time, so the commit and the tag must
exist **before** the build, and the tree must be clean. `tools/release.ps1` enforces the order.

## Steps

1. **Look at the state.** `git status --short`, `git tag -l --sort=-v:refname | head -3`, `git log <last tag>..HEAD --oneline`.
   Fetch tags first (`git fetch --tags`): releases are sometimes created on GitHub from elsewhere.
2. **Commit anything uncommitted** (add files by name, never `-A`; nothing from `secrets/`). Write the message like the
   earlier ones: a summary line, then bullets saying what and why. End with the attribution line the harness gives.
   If the tree is already clean, skip this.
3. **Choose the version.** Minor bump (v1.3.0 -> v1.4.0) when there is a new user-visible feature, patch bump
   (v1.3.0 -> v1.3.1) for fixes and tweaks. If the user names a version ("as v2.0.0"), use it. Say which you chose
   and why, in one line.
4. **Write the release notes** to a file in the scratchpad, in the style of the earlier releases (`gh release view
   <tag>`): plain-language bullets a member or family could follow, no internal jargon, about the user-visible
   change. Title: a short phrase; the script prefixes "Librarian vX.Y.Z: ".
5. **Do a dry run first if anything is unusual** (`-DryRun` builds and verifies but pushes nothing); otherwise run:

       .\tools\release.ps1 -Version vX.Y.Z -Title "short phrase" -NotesFile <notes file>

   It tags, reconfigures and builds, checks the image reports the version, pushes master and the tag, creates the
   release, then publishes `bookbook.bin` and `bookbook.json`. If it fails before the push, nothing is public.
6. **Report:** the version, the release URL, the firmware hash and size, and that a unit picks it up when asked
   "is there an update?" then "update yourself". If a step failed, say which step, and what is already public.

## Notes

- Building needs ESP-IDF 5.5 (`tools/idf.ps1` sets it up). Publishing uses the SAS URL in `secrets/sdkconfig.secrets`.
- The board's serial port is not needed. Do not reset or reflash a unit as part of this.
- Regenerate `docs/conversation-rules.md` (`python tools/rules_doc.py`) if the prompt in `main/brain.cpp` changed,
  and commit it before releasing.
