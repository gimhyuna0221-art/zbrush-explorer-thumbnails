# Contributing

Describe the problem and the expected result in an issue or pull request. Keep fixes focused, and include the commands you ran and their results. State separately whether you checked the decoder, the COM provider, or actual File Explorer behavior.

## Build and test

Building requires Windows x64, Visual Studio C++ tools with a Windows SDK, and PowerShell. The scripts find an existing C++ toolchain through `vswhere`; they do not install dependencies. Users of a prebuilt package do not need the C++ build tools.

From the repository root, run:

```powershell
./scripts/Build-Package.ps1 -Version 0.2.0
```

The default command builds the provider, runs the synthetic test host, then builds the installer and package. The package is written to `dist/ZBrush-Explorer-Thumbnails-0.2.0-win-x64.zip`, with its checksum in `dist/SHA256SUMS.txt`. Building the package does not run the installer or register a Shell extension.

Packaging refuses to overwrite an existing release output. Preserve that output and use a clean checkout or a new version for another package; use the provider-only commands below while iterating on code.

For provider-only development, run:

```powershell
./scripts/Build-Provider.cmd
if ($LASTEXITCODE -ne 0) { throw "Provider build failed." }
& ./build/tests/ProviderTests.exe ./build/provider/ZBrushThumbnailProvider.dll
if ($LASTEXITCODE -ne 0) { throw "Provider tests failed." }
```

`Build-Provider.cmd` builds the DLL and test host; it does not run the tests automatically.

The test host loads the built DLL directly and supplies generated image data in memory. It checks decoded pixels, supported image layouts, malformed input rejection, bounded stream reads, COM behavior, and bitmap orientation. It does not require ZBrush or use private model files, registration, or interactive UI. Passing these tests does not establish that a particular Windows Explorer installation will activate the provider or refresh its cache correctly.

The GitHub Actions workflow uses `windows-latest`, runs the same build and tests, and uploads only the package ZIP and checksum after success. Actions are pinned to full commit hashes. The workflow has read-only repository permissions and creates no release or tag. Workflow artifacts are build outputs for review, not a claim that a release has been signed or manually tested in Explorer.

## Changes and test data

- Keep model access read-only and bounded. Unknown or malformed formats should fail without changing the source file.
- Preserve `IInitializeWithStream` and normal thumbnail isolation. Automated tests must not register the provider, launch the installer, change security settings, or restart Explorer.
- Add small synthetic cases for parser and rendering changes. Do not contribute Maxon models, extracted product icons, private artwork, recovery files, or images captured from someone's work.
- Keep third-party notices with redistributed code. Follow `THIRD_PARTY_NOTICES.txt` and identify the source and license of any new dependency or asset.
- If a manual integration test is needed, use an explicitly disposable sample and record it separately from the automated tests. Do not ask another contributor to overwrite a working model to reproduce a thumbnail issue.
- Before changing an Action pin, verify the release and full commit in the official `actions/checkout` or `actions/upload-artifact` repository. Keep credential persistence disabled and repository permissions read-only.

## Reporting a problem without sharing private files

Use the bug report form. Include the package version, Windows version and architecture, ZBrush version if relevant, generic reproduction steps, and a short error code or sanitized message. Use names such as `sample.ztl` in place of real project names.

Do not upload `.ZTL`, `.ZPR`, recovery files, raw diagnostic logs, or screenshots containing artwork or personal information. Remove usernames, full paths, customer names, and project names from any text you share. A synthetic reproduction or a written description is sufficient to start an investigation.
