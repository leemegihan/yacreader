# Local OCR validation without a library database

The Windows validation workflow also publishes `manga-local-ocr-probe-<commit>`.
Its `local_metadata_test.exe` has two explicit diagnostic modes in addition to
the existing regression tests. Run it beside the matching application's Qt
DLLs/plugins and `utils/7z.dll`, in a separate validation installation.
These modes never call database save or external catalog lookup functions.

## Read the application's actual sampled pages

```powershell
.\local_metadata_test.exe --local-ocr-pages <source-comic-or-image-folder> <new-output-directory>
```

The output directory must not exist. Its parent must already exist, outside
the original source directory. This uses the application's decoder, natural
ordering, front/back three-page sampling and OCR preprocessing. It writes
`pages.json` and prepared PNGs into the new directory only.

## Classify already-read text with the application's candidate parser

```powershell
.\local_metadata_test.exe --local-ocr-candidates <manifest.json> <new-report.json>
```

The version-1 manifest contains 1–12 pages:

```json
{
  "version": 1,
  "pages": [
    {"number": 1, "size": [1200, 800], "result": "<absolute-local-worker-result.json>"}
  ]
}
```

Each result is an unmodified version-1 neural worker response for that page.
The report contains title, author and publisher suggestions with their evidence.
Filename hints are deliberately omitted so recognizing a name and assigning its
role can be evaluated separately. Existing output files are refused. Invalid
manifests/results fail before a report is written. Exit codes: 0 success, 2
invalid input/destination, 3 page/reading/output failure. Failed page extraction
may leave a partial new output directory; inspect it and use another new
directory for a retry.

Keep real manifests, pages, OCR text, candidates, source paths and human-reviewed
answers outside Git and CI artifacts. Only the diagnostic executable and
synthetic regression fixtures belong in public build artifacts.

## Measurement rules

- Freeze the source selection, prepared page hashes, model/worker versions,
  language and preprocessing before comparing devices.
- Record requested and returned devices and fallback warnings. A CUDA build or
  successful CPU fallback does not establish real GPU inference.
- Compare CPU 4/8/16 threads and GPU on identical prepared pixels; rotate run
  order and record initialization, recognition and total process time separately.
- Device-to-device transcription agreement is not accuracy. Report detection
  misses, character errors and role-assignment errors against reviewed evidence.
- If no original image-folder works exist, identify any extracted folder copies
  as derived fixtures. Do not count them as independently sourced folder works.
- Leave unverified titles/authors and difficulty classifications for review.
  Never convert high OCR confidence into asserted identity.

## Optional worker evidence

Pass `--diagnostics` to the offline worker for local error triage. The default app invocation is unchanged. Each page adds `diagnostics.passes`, with detected boxes and per-language/per-variant recognition candidates, including rejected low-confidence text. Diagnostic text is capped at 160 characters with `textTruncated`; actual OCR output is unchanged.

Pass coordinates map to the prepared page as `x / scale[0] + origin[0]` and `y / scale[1] + origin[1]`. Full-page origin/scale are [0, 0]/[1, 1]; credit retries record their crop origin and scale. Evidence resets for every page, including decode failures.

A reviewed text region with no overlapping detection is a detection miss. A detected region with wrong or rejected text is a recognition/selection issue. Correct names missing from appropriate candidates are role-assignment issues. Boxes and confidence alone do not establish correctness.

## Safe validation deployment

Use the `manga-validated-runtime-<commit>` artifact to unpack the already-installed, hash-checked runtime into a separate local directory. It includes QtTest and synthetic fixtures, excludes uninstall executables, and does not register file associations or run upgrade hooks. It is a validation artifact, not a replacement for the normal installer. The normal installer and its installed OCR/GUI checks remain mandatory in Windows CI. Running the installer with a different directory on a PC with an existing installation can still invoke its old-version uninstaller.

Shared `作者・サークル名`/`著者・サークル名` credits produce unconfirmed author and publisher alternatives; they do not prove a person's identity, autofill author fields or support inferred cover titles. Explicit `発行サークル` remains publisher-only. Separate slash-delimited values in the ambiguous shared field are left for manual review.

## Isolated application settings and native device checks

Set `YACREADER_DATA_DIR` to an absolute, dedicated validation directory before launching the unpacked validation apps. Per-application settings/logs and shared settings/plugins then stay below that directory. With the variable unset, normal settings locations are unchanged. Relative override paths fail explicitly rather than falling back to the user's active settings. Never point this directory at original media or an operating library.

For the explicitly local `localNeuralDevice` test, set `YACREADER_EXPECT_NEURAL_DEVICE` to `cpu` in a validation package without the optional GPU runtime, then to `gpu:0` after staging the isolated NVIDIA runtime. Both invocations request GPU through the application's C++ wrapper and require actual synthetic JP/KR text plus the expected returned device/warning. An absent variable skips this optional physical-device check in ordinary CI; mandatory packaged CPU and GPU-less fallback checks remain separate.
