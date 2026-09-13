# Local OCR validation without a library database

The Windows validation workflow also publishes `manga-local-ocr-probe-<commit>`.
Its `local_metadata_test.exe` has three explicit diagnostic modes in addition to
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
the optional diagnostic argument accepts 1–6 pages per end to reproduce earlier
audits. The normal inspector is limited to 1–3 per end. Do not expand the window
automatically when candidates are absent; use filename/catalog review. Short
works are deduplicated and the selection is recorded in `pages.json`. Historical
expanded audits stay separate from a frozen comparison corpus. The probe writes
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

This override does not redirect a library database: `LibraryPaths` still resolves
`<library-root>/.yacreaderlibrary/library.ydb` and its covers below the selected
library root. For save/import tests, use a separate test library root with
synthetic or independently copied fixtures. Do not add the operating library to
an isolated-settings GUI and assume its database has also been isolated. The
read-only page/candidate probes do not need a library database.

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

## Durable storage validation

At code 9e41aa08a655d93c10ae546ee93cc4f18bdea31e,
[Windows validation](https://github.com/leemegihan/yacreader/actions/runs/34352707801)
passed all 13 CTest suites and retained the existing 17 worker, 8 staged-neural
and 21 installed-OCR checks. Downloaded-artifact validation now additionally covers
the cache and job store: 8 OCR/cache and 15 job-store passes, without skips.
Prerequisite runs (44 passed/9 skipped, then 47 passed/6 skipped) are not substitutes
for those required package checks.

[General validation](https://github.com/leemegihan/yacreader/actions/runs/34352712223)
passed all build targets and Windows x64's 13 CTest suites. An earlier revision's
ARM64 dependency download/extraction failed before compilation; the final ARM64
job succeeded. Fork signing/notarization/publishing skips do not constitute a
signed release.

The new store/cache tests use synthetic data only. They exercise independent
processes, abrupt exit with an uncommitted SQLite write, stale ownership, clock
changes, damaged records, locked writes and competing immutable-cache writes.
The private extracted runtime also passed the storage/cache regressions, bounded
input rejection and repeated process scheduling checks. Actual saved-response
round trips and candidate replay are distinct from OCR inference benchmarks.
Separate synthetic device checks distinguished real GPU execution from CPU fallback.

The job store does not execute OCR, validate the truth of identity candidates,
stop OS processes or authorize library writes. Its receipts are not a substitute
for revalidating cached artifacts. Full setting/transform snapshots, an executor,
explicit invalidation and UI recovery remain integration work. Continue using an
independent test library root; app-settings isolation alone does not relocate
library.ydb or covers. See [ocr-job-store.md](ocr-job-store.md).

The personal development target is Windows x64. A subsequent external launcher
pilot verified termination of an owned Qt OCR controller and its observed CUDA
descendant on both job-handle close and wrapper crash. It used synthetic input
and did not connect Job Objects to application cancellation. The isolated GUI
startup retry passed; an earlier normal exit before the check remains an
unreproduced observation, not a proven fix.

Windows OCR invocations now own their descendants through creation-time Job
Object assignment. Cancellation and timeout confirm cleanup before a CPU retry.
Progress JSON remains atomic: transient Windows replacement-sharing failures are
retried within a bounded interval, and the C++ reader releases its handle before
callbacks. Tests include real Windows delete-sharing contention and owner crash.
Local actual-GPU ownership verification used the new CI diagnostic executable
with a pinned worker and a prior verified dependency archive after the complete
new runtime transfer timed out. Keep that composition distinct from CI's new
installer/runtime checks and from real-library accuracy or timing measurements.


## Isolated inspector session validation

The explicit `YACREADER_DATA_DIR` profile also scopes reader/library local IPC.
Reader and library processes with the same normalized profile share their
endpoint; different profiles use separate endpoints. Without that override the
existing application endpoint remains unchanged. Synthetic socket tests never
connect to or remove the operating profile endpoint. This does not relocate a
library database, disable its HTTP server or authorize opening the real library.
Continue to use a separate test library and disable the test HTTP server.

The opt-in `inspectorSessionNeural` test uses only packaged synthetic Korean and
Japanese pages and a temporary library. It calls the actual runner, pauses after
one durable receipt, clicks the inspector continuation button and then opens a
fresh inspector instance. It checks device labels, retained original timings,
unchanged raw cache on reopening, preserved user edits and no automatic external
lookup or metadata save. The packaged CPU check and local physical GPU check are
separate requirements. Batched inference may compute an unrecorded second page
before cancellation; one persisted receipt does not mean only one page was
inferred. This is functional validation, not a speed benchmark or real-work
accuracy measurement.

The opt-in `privateSelectedInspector` diagnostic accepts one explicit private
case manifest through `YACREADER_PRIVATE_INSPECTOR_CASE`. It refuses an existing
result file or job database, requires a copied-library marker and matching source
SHA-256, and reads only the specified library row. It clicks the actual inspector
read and saved-result buttons with automatic catalog lookup disabled; no save is
requested. Its private report retains original timings, receipt hashes, candidates,
requested/actual device and completion/restoration status. No real manifest,
source file, filename, path, OCR response or truth label belongs in Git or CI.
The driver must use identical CPU8/GPU inputs, separate fresh profiles, and preserve
failed records. This is one selected work per invocation, not a library scanner.


When the private manifest and named diagnostic are explicitly selected, the
diagnostic defaults `QTEST_FUNCTION_TIMEOUT` to 1050000 milliseconds before QtTest
starts. An explicit caller value is preserved, and ordinary tests keep their
normal default. The private reference driver also sets this value and uses its
own bounded 1100-second process deadline. QtTest's default five-minute function watchdog
covers both OCR and completed-review reopening; a longer QTRY wait does not
raise that watchdog. See the [Qt Test execution guidance](https://doc.qt.io/qt-6/qtest-overview.html).
This changes only the diagnostic deadline, not the worker deadline or the
first/last-three-page cap. Preserve any original timeout result. A timed retry
uses a separate fresh profile/cache and a new output directory; do not count a
failed run as a successful comparison or reuse its partial cache for cold-run
timing. Cached-review correctness is reported separately from fresh inference.

The diagnostic writes a bounded private `.stages.json` sidecar on observed UI
status changes, retaining the last stage even if a watchdog aborts the test.
Read and reopen have separate relative clocks. The 100ms sampling can miss short
stages and adds tracing overhead; use it to locate waits, not as a kernel timing
or a replacement for the uninstrumented comparison. The sidecar and timelines
are private results and must not be committed or uploaded to CI.


Runtime inventory reports bounded progress for application, model, shared, CPU,
optional GPU and final-stability phases without publishing filenames. File/byte
updates occur at most every 250ms, with an immediate update on phase changes.
The inventory is still fully hashed; this is responsiveness/diagnostic feedback,
not a hash cache or a claimed speed improvement. Callbacks precede the final
stability pass. Private status traces group count-only changes into the same
stage so frequent progress does not exhaust the bounded timeline before OCR.


For an explicit `runtimeSnapshotDeployed` diagnostic with its private output path,
`YACREADER_RUNTIME_READERS=1` through `16` selects the bounded hash-reader limit.
Compare full manifests and settings fingerprints as well as wall time from the
same runtime bytes. This option does not select OCR CPU threads, execute OCR,
change its settings snapshot, or enable a metadata-only runtime cache. Report
warm-cache and application-level measurements separately from hash prototypes.
