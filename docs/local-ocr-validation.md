# Local OCR validation without a library database

The Windows validation workflow also publishes `manga-local-ocr-probe-<commit>`.
Its `local_metadata_test.exe` has two explicit diagnostic modes in addition to
the existing regression tests. Run it beside the matching application's Qt
DLLs/plugins and `utils/7z.dll`, in a separate validation installation.
These modes never call database save or external catalog lookup functions.

## Read the application's actual sampled pages

```powershell
.\local_metadata_test.exe --local-ocr-pages <source-comic-or-image-folder> <new-output-directory> [pages-per-end]
```

The output directory must not exist. Its parent must already exist, outside
the original source directory. This uses the application's decoder, natural
ordering and OCR preprocessing. Sampling defaults to front/back three pages;
the optional final argument accepts 1–6 pages per end, matching the inspector's
bounded range. Short works are deduplicated and the selected value is recorded
in `pages.json`. Keep expanded-page discovery separate from a frozen comparison
corpus. It writes
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


## Keep the actual worker job and package intact

The page probe uses the same prepareOcrImage path as the application, including
white compositing, bounded scaling, a border and grayscale conversion. Freeze
these emitted PNGs for comparisons; a different image library's preprocessing
is a separate experiment.

For --manifest, every image and result must be directly inside the manifest's
job directory. Copy prepared images into a new private job directory, or link
only private prepared copies. Never place job files next to original media.

Validation artifacts must include hidden runtime metadata such as
paddlex/.version. The workflow downloads its own uploaded artifact and runs OCR
from those downloaded files so a successful pre-upload installation alone
cannot hide packaging omissions.

On Windows, set a Unicode YACREADER_DATA_DIR in the launching process and use
the normal Windows Qt platform for local checks. For a blank GUI smoke test,
set libraryConfig/SERVER_ON=false in the isolated YACReaderLibrary.ini first.
QML_DISK_CACHE_PATH can select a private QML cache, and
QT_DISABLE_SHADER_DISK_CACHE=1 disables automatic shader disk storage during
the smoke test. See the [Qt QML cache documentation](https://doc.qt.io/qt-6.5/qmldiskcache.html)
and [Qt graphics cache documentation](https://doc.qt.io/qt-6/qquickgraphicsconfiguration.html).

Treat performance totals as the stated measurement interval, not full import
time. Record first-run initialization separately; runtime bytecode and OS caches
can change startup costs. Keep CPU/GPU comparison details and real-work review
results local.

## Filled-circle credit separators

Explicit roles may use a colon or a filled-circle separator (● or ◉).
Publisher and printer roles remain separate from authors, and shared
person/circle labels remain unconfirmed alternatives. Separators inside values
are preserved. A role word embedded in dialogue or concatenated directly with
a name does not become a labelled identity candidate.

Bracketed roles such as `[Circle] Example Studio` require a value on the same
OCR line. A standalone bracketed label is not linked to the following line;
that line may be a social-media heading rather than a name. Automatic language
selection can still prefer an incomplete transcription. Inspect both model
readings in private diagnostics or rerun with an explicitly chosen language;
recognition confidence alone does not establish complete or correct text.

## Partial cross-language readings

The score winner remains the primary OCR line. When that line is ASCII-only
and another language has a high-scoring reading containing the same token plus
at least two additional characters with its CJK script, the worker retains up
to two distinct alternatives for review. This heuristic identifies possible
truncation, not correctness, and does not add inference passes.

Version-1 line objects may include an optional `alternatives` array. Each
entry has text, confidence, language and the same box as its parent. The client
bounds and validates these records separately and accepts older responses
without the field. Retry crops map both primary and alternative boxes back to
the prepared page. Primary text and aggregate confidence exclude alternatives.

The inspector displays primary and alternate readings together. Same-line
explicit credits can yield unlabelled review candidates. General alternative
parsing does not borrow adjacent primary text. A separate constrained path
handles a slash-only primary whose alternate is an ordered publisher/author
label: independently identified colophon context, an immediately following
aligned same-language name pair, and high scores are all required. Shared
person/circle labels and incomplete or intervening rows are excluded. These
proposals cannot infer cover titles, autofill fields or start automatic lookup.
Always compare both the role order and names with the original.

CJK disagreements may also be retained when both readings score at least 85
and the gap is at most 8. A slash, colon or middle-dot-only primary can retain
an expansion containing that delimiter under the same score/gap limits.
These are review-volume limits, not calibrated correctness probabilities.
Primary text and the number of inference calls remain unchanged.

Compact joined circle banners near the top of an opening page may contribute
one unconfirmed publishing proposal. The complete two-part value is preserved;
the delimiter does not establish which part is a person or a circle. Automatic
lookup excludes publisher hints without strong evidence. Explicit lookup
carrying tentative publishing hints opens review even with a filled title.

## Filename fallback replay

The inspector now defaults to, and is bounded at, three pages per end. The explicit local page
diagnostic can still reproduce historical 1–6-page audits; it does not change the product fallback
policy. Do not automatically widen the fixed sample when no page identity candidate is found.

The explicit --local-ocr-fallback mode accepts the same private result manifest as
--local-ocr-candidates, with additional sourcePath and optional libraryRoot strings. It returns
page and filename suggestions plus a filenameFallback flag, without reading additional comic
pages, accessing a library DB or making a network request. Use a fresh output path. The original
OCR-only probe continues to exclude filename hints.

Keep failed/unfinished OCR separate from absent page candidates. Synthetic UI tests verify that
filename proposals open review rather than triggering HTTP, do not prefill confirmed fields,
preserve user edits and cannot replace existing page evidence. Actual filename-derived queries
and results remain private; having a usable search lead is not a catalog identity match.
