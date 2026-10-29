# Testing and compatibility

Tests use generated data, not redistributed ZBrush models, logos, or private work.

## What has been established

An earlier development prototype was tested on Windows x64 with ZBrush 2023.2.2. Disposable ZTL files saved through Tool > Save As and ZPR projects saved through File > Save changed their embedded preview and the corresponding Explorer thumbnail. This describes a prior prototype, not a completed deployment test of public 0.2.0.

The public package has its own provider identity and reusable per-user installer. Its exact release must be distinguished from source compilation, synthetic tests and direct DLL calls. At documentation preparation time, public 0.2.0 has not been deployed and verified end to end in Explorer. Build/test results should be recorded for the exact released binaries.

## Synthetic provider tests

`tests/ProviderTests.cpp` exercises generated supported headers, dimensions and RGB data, plus malformed input. Meaningful checks include:

- Known RGB output, correct dimensions and orientation, opaque alpha and bitmap ownership.
- Legal short reads, bounded prefix reads and restoration of the original stream position.
- Rejection of invalid signatures/layouts, malformed RLE, truncation, invalid dimensions and oversized lengths.
- COM interface/lifetime behavior without self-registration or an Explorer restart.

Synthetic data proves these parser and interface properties. It does not prove that every real ZBrush version writes the same format, or that Explorer repaints every cached image immediately.

## Installer lifecycle review

The essential sequence is Install, repeated Install, Remove, repeated Remove, then a fresh Install. Verify exact stable hashes, one valid active journal, preserved previous journals, correct handler resolution and unchanged effective open associations. A later installation must not be removed by an older completed journal.

Conflict, malformed-journal, interrupted-operation and file-copy failure cases should be tested with isolated synthetic state or injected failures. Never corrupt a live installation, change another application's handler, fill a disk, disable protection, or terminate a user's application merely to test a failure path.

The installer refuses preexisting handlers, differing immutable payloads, reparse/nonlocal paths and uncertain ownership. Unknown or incomplete states are preserved for inspection. Removing the registration retains package files and journals. This is a conservative journaled workflow, not a crash-atomic registry/filesystem transaction.

## Explorer and save tests

Use only self-owned disposable samples and a separate safe ZBrush document/session. Keep original and recovery files outside the test.

For an automatic-refresh observation, establish the visible baseline, change the owned file, and observe before F5, navigation, explicit thumbnail extraction or Shell notifications. A subsequent diagnostic can populate the thumbnail cache and must not be mistaken for an automatic update.

`WTS_FORCEEXTRACTION | WTS_REQUIRESURROGATE` requests are different from proof that a new surrogate extracted the image. Record returned cache flags, pixel results and actual Explorer observations separately. Cached pixels or direct provider calls alone do not demonstrate a fresh isolated extraction.

QuickSave/AutoSave, crash recovery, move/rename behavior, reboot persistence, all file versions and every cache state remain unverified unless a release explicitly records those tests. A valid thumbnail is not proof that the complete model is intact.

## Public-tree audit checklist

- No personal paths, account SID, private file names, recovery data, credentials or copied development journals/history.
- No extracted vendor artwork, vendor/user model fixtures, or private screenshots.
- No hard-coded authorized-user identity; paths resolve for the current user through the Windows known-folder API.
- No administrator requirement, default-open binding edits, security/isolation override, process termination or destructive cleanup.
- Public synthetic illustrations are identified as synthetic; no illustration is presented as an unperformed Explorer or ZBrush test.
- Release files match their recorded hashes, required license notices are included, and the package contains only intentional distributable files.

This checklist is a review plan, not evidence that every item or runtime test has already passed.
