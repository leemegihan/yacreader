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
