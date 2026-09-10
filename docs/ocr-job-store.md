# Durable OCR job store

The `ocr_job_store` Qt Core/Sql library supplies persistent bookkeeping for a
future OCR executor. It is built and tested independently of GUI widgets and the
library database. It is not yet connected to the inspector, a job-list UI or automatic cache
reuse, and does not enable full-library execution.

## Storage boundary

Construct and destroy each Store on its owning thread. Open an explicit absolute
`ocr-jobs.sqlite` path in an already-created, separate application-data directory.
Do not pass a library root or operating database. Existing foreign databases,
symlinks and unknown schema versions are refused before schema/WAL changes.
SQLite application_id and user_version identify the file. WAL and synchronous
FULL preserve committed transactions across the tested process-crash boundary;
this is not a hardware power-loss guarantee.

Registration is idempotent over a SHA256 of the complete canonical specification:
library generation, comic ID, source snapshot, source context (path/library root/
kind), complete settings snapshot and its fingerprint, total page count and ordered
selected pages. The caller computes the source fingerprint; settingsFingerprint()
validates and hashes the complete settings snapshot. At most six distinct ascending pages from the first/last three are allowed.
The store does not enumerate or open original media.

## State and ownership

Only queued jobs can be claimed. A successful claim generates a new random token
and increments the attempt count. Page commits, heartbeats, failure and review
completion require the matching job/token/owner and an unexpired lease.
Transactional checks and writes use BEGIN IMMEDIATE. Late workers cannot replace
committed evidence or finish another worker's job.

Pause, cancel and explicit resume invalidate ownership while retaining committed
receipts. Lease expiry or a clock jump behind acquisition moves running work to
interrupted, never directly back to queued. Failed, cancelled and interrupted
work cannot become candidate-free review through expiry. The executor must
verify the previous OS process exited before resume/claim; these DB tokens do
not stop a process or serialize physical GPU use across different jobs.
There are no automatic retries in this layer.

Completion requires receipts for every selected page and routes to page review
or filename review according to the caller's validated candidate analysis.
These states authorize neither catalog requests nor library writes.
A receipt records page number, cache key, result SHA256 and actual device
(`cpu` or `gpu:0`). Same-value replay is idempotent; replacement is refused.

## What is deliberately not a cache

Receipts reference validated, durably stored artifacts; they do not contain OCR
text and are not sufficient evidence that a cached result still exists or is
valid. The future executor must validate result JSON with the existing neural
parser, check artifact hash/version/settings/device and only then reuse it.
An invalid artifact needs explicit invalidation/reprocessing support before
executor integration. No automatic receipt reuse exists in the job store.

The settings key must distinguish worker/model/package versions, image
preprocessing, requested device and thread/language settings. Actual CPU fallback
must remain CPU evidence. Candidate derivation needs separate source/page/parser
context and must not confuse a filename rename with an OCR model change.

## Validation and remaining integration

`ocr_job_store_test` uses temporary synthetic databases only. It covers identity
changes, bounded plans, persisted pause/resume, incomplete completion, failure/
cancellation, expiry and clock changes, immutable receipts, foreign/future DB
refusal, corrupt-record refusal, locked-write rollback, wrong-thread rejection
and two independent claimant processes.
A child exits immediately during an uncommitted fourth-page insertion: three
committed receipts survive, the fourth rolls back, a stale child write is
rejected and explicit recovery can finish the remaining pages.

The Windows workflow retains existing OCR/build/install checks and additionally
runs this suite before the full build and from the downloaded runtime artifact.
This is a bookkeeping test, not actual OCR process cleanup or GPU inference.

Next: validate and persist completed OCR page payloads, then connect a shared
executor with OS worker cleanup and resource ownership, explicit recovery and
review UI. Only a separate test library and a small fixed sample may be used for
integration until a wider run is explicitly authorized.

## Validated page artifact cache

The separate local_ocr_cache module saves immutable raw neural responses through
QSaveFile, with direct-write fallback disabled and a per-entry QLockFile. The
identity includes prepared PNG SHA256 and dimensions, preprocessing fingerprint,
worker/model/package manifest hashes, language, CPU threads, requested device and
actual device. All identity components are required. A requested GPU result that
actually ran on CPU has a different key and cannot satisfy an actual-GPU lookup.

Load checks the envelope version and complete identity, canonical base64 bytes,
result SHA256, size and the existing parseNeuralReading parser (including geometry,
language and errors). Review-required state and original raw bytes are preserved.
Empty successful pages may be cached; failed/invalid responses cannot. Different
bytes and damaged existing entries are refused, preserving evidence for explicit
recovery instead of silently overwriting it. This detects accidental corruption;
hashes are not an authenticity signature for an attacker who can rewrite the cache.

The caller must compute the fingerprints from actual inputs and installed model/
package manifests, verify cache.save succeeded, then commit its job receipt.
The module does not calculate source fingerprints, schedule OCR or transparently
intercept the inspector. Candidate/filename context remains outside the image key
and must be recomputed after retrieval.

Synthetic tests cover identity separation, geometry/error/corruption refusal,
immutable bytes, empty-page success, review preservation and CPU/GPU distinction.
Two synchronized child processes also race different valid bytes for the same
cache identity; exactly one writer commits and the other cannot replace it.
The private diagnostic executable additionally accepts
`--local-ocr-cache manifest.json NEWreport.json` for at most six saved responses.
Each page supplies an absolute result path and a complete identity object using
the cache envelope field names. The report and its new .cache directory must not
exist. This probe validates and round-trips existing results; it never runs OCR
or establishes that the caller's claimed engine fingerprints are true.

## Executor integration prerequisites

The job specification now requires a recoverable options/environment snapshot.
The executor bridge must build it from resolved options and measured installed files,
then remeasure and compare its fingerprint before offering application restart/resume. The cache records prepared dimensions and a preprocessing fingerprint;
full original-to-prepared transforms must accompany completion payloads before
drawing cached regions over an original page.

The GPU wrapper now preserves validated completed pages in memory and retries
only failed/unread pages on CPU after verified worker cleanup. Its separate
execution status preserves cancellation, cleanup failure and nonzero exit after
all outputs. Durable page callbacks and cache-before-receipt writes still need
integration; do not treat these in-memory completion flags as stored evidence. Cache elapsedMs is historical inference data,
not a fresh cache-hit duration.


## Recoverable settings snapshot (version 1)

New job databases use schema 2. Schema 1 stored only an opaque settings hash and
cannot reconstruct the missing options. Such databases are refused without
migration or deletion; preserve them as evidence and create a separate new test
database. No operating library database is involved.

The private settingsSnapshot contains exactly version, options and environment.
options records every current OcrOptions field: neural, gpu, cpuThreads, executable,
dataPath, language, vertical, segmentation, rotation, invert, adaptiveThreshold
and timeoutMs. Values have strict types, supported language/layout values,
quarter-turn rotation and bounded thread counts. No omitted fields receive defaults.

environment includes platform (windows-x64), applicationSha256,
preprocessingRevision, cpu and gpu. A runtime object contains absolute executable
and dataPath, executableSha256, workerSha256, modelManifestSha256 and
packageManifestSha256. Tesseract uses a null worker hash and its resolved option
paths must match the CPU runtime. GPU is either a complete neural runtime or
explicit null for an unavailable addon. Installing an addon later changes the
snapshot; it cannot silently change the old job's execution settings.

settingsFingerprint() returns empty for malformed/unsupported snapshots and
SHA256 of Qt's compact canonical JSON otherwise. Registration and reads verify
that this equals the stored fingerprint, in addition to the whole job hash.
Unknown fields and versions fail closed so newly introduced settings cannot be
silently ignored. App bytes and the preprocessing revision distinguish code that
can change decoding/preparation or interpretation. Both available CPU/GPU runtimes
are recorded, including fallback dependencies.

These are caller-supplied measurements, not an attestation performed by the
store. The module does not open executables, models or original pages and does
not restore OcrOptions into the inspector yet. Manifest fingerprints must describe
the actual files, not just a downloaded lockfile. Before resume the executor must
remeasure the environment, validate source/cache evidence and refuse a mismatch.
Prepared-page geometry/transforms and physical GPU scheduling remain separate work.

Synthetic checks cover non-default settings across close/reopen and lease recovery,
absent GPU and Tesseract snapshots, missing fields, type/range/version errors,
stale fingerprints, changed runtime hashes and preservation of an older database.
Snapshot code 9873b369 passed Windows run34377207382, including 18 downloaded
job-store checks. The exact new diagnostic executable also passed all 18 checks
locally with zero skips in a composed deployment using previously verified Qt/SQL
dependencies. This was a synthetic temporary-DB check, not OCR inference or a
byte-for-byte validation of the complete new runtime artifact.

## Next executor boundary (planned, not implemented)

The storage and in-memory recovery checks do not yet create a durable executor.
Introduce the following connection in small steps before exposing restart UI:

1. Capture exact prepared PNG bytes, SHA256, dimensions, input dimensions and an
   input-to-prepared transform. Record the complete preprocessing version. The
   input is the decoded image shown by the inspector; file EXIF/scaled decode and
   any crop offset are additional context, not an identity transform. Cache-hit
   regions must not be drawn on an original page until that mapping is validated.
2. Capture the raw parsed response before its temporary directory disappears,
   with original selected-page index and actual device. A valid blank page counts
   as completed data. The separate batch status still controls session success;
   valid page files cannot turn a failed worker into candidate-free completion.
3. Build the saved settings snapshot from resolved runtime paths and actual file
   measurements. Verify manifests against the files they describe, including CPU
   fallback dependencies. Recheck source, settings and cache evidence on resume;
   a saved fingerprint is not proof that installed files remain unchanged.
4. Save the validated immutable cache payload first, then record its DB receipt
   with the current lease. A failed cache write cannot create a receipt. A late
   result after lease expiry may leave an orphan cache artifact but cannot advance
   the job. An invalid cached artifact requires explicit recovery/invalidation;
   never overwrite an immutable receipt to conceal changed evidence.
5. Persist execution failure separately even when all page artifacts are present.
   Explicit recovery must acknowledge the prior failed attempt and verified worker
   exit; a cached-page count alone must not erase that failure. Lease fencing also
   does not serialize physical GPU workers across independent app instances.
6. Connect the inspector only after the above contracts pass synthetic tests.
   Use a separate test library for import/save tests: YACREADER_DATA_DIR isolates
   settings but does not by itself isolate a library DB or its covers.

Acceptance cases include quarter-turn/scaled/cropped geometry round trips,
unchanged prepared pixels, one valid blank page, sparse outputs, raw JSON/hash
mismatch, a failed cache write, a stale lease, crash after cache save but before
receipt, changed runtime files and a worker failure after all outputs. Retained
GPU pages must stay unchanged when CPU handles the remaining pages. These tests
must not enumerate more original works, expand the first/last-three-page limit,
start whole-library processing or authorize automatic metadata saves.


## Prepared geometry and raw neural evidence

The in-memory RecognitionBatch now has optional per-page NeuralPageEvidence.
The production neural runner retains exact raw JSON (bounded to 4 MiB), its hash,
the hash of the exact PNG sent to the worker, preparation geometry and requested/
actual device. PNG bytes are encoded once, written and hashed, then released per
page. Preparation reports the decoded input size, final size, preprocessing
revision and Qt's adjusted input-to-prepared transform. Existing prepared pixels
and Reading bounds are preserved; the latter still drive candidate interpretation.

Coordinates start at the decoded input, after EXIF/scaled decoding. A crop is
additional caller context: mapOcrBoundsToPage requires a matching decoded page size
and crop rectangle, rejects padding-only/out-of-range regions and maps pixel edges
for presentation. It does not reconstruct original encoded-file coordinates or
connect overlays to the inspector yet. Actual integer scaling ratios and
QImage::trueMatrix preserve quarter-turn translations and rounding.

Valid blank responses retain evidence. Failed or invalid responses cannot supply
valid page evidence. Recovery preserves completed GPU payloads unchanged and
remaps CPU retry evidence to the original selected index. requestedDevice records
the per-attempt device (cpu for a CPU retry); the user's original GPU preference
belongs in the job settings. A valid payload does not erase a failed session,
cancellation or cleanup failure. Tesseract and legacy injected runners may omit
neural evidence. Hash validation detects accidental mismatches, not authenticity.

This is still in-memory retention: it survives temporary-directory cleanup but
not an app crash. Raw callbacks, measured runtime snapshots, cache-before-receipt
writes, physical GPU scheduling and durable inspector resume remain separate.
Synthetic geometry and worker-failure tests cover mapping, sparse retries, blank
and malformed responses and exact raw bytes after temporary cleanup. Windows CI
and local downloaded-artifact verification must pass before calling this change
validated. No original works or private OCR outputs are included in these tests.
