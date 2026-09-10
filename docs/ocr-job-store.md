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


## Read-only neural runtime measurement

LocalOcrRuntime::measure builds the complete version-1 settings snapshot from the
selected OcrOptions and measured package files. It hashes the application and
worker, checks all nine installed model files against models.json with exact
required paths and no duplicate entries, and inventories CPU plus any installed
GPU interpreter tree. Shared application files/plugins also enter each package
fingerprint. The retained manifests contain relative paths, byte lengths and
SHA256 values; the settings snapshot contains resolved absolute runtime paths.
They are private local evidence, never build artifacts or telemetry.

Missing GPU is explicit null. A present but incomplete addon, linked/unreadable
entry, cancellation, bound violation or observed file/directory change refuses
the measurement. The model lock declaration alone cannot stand in for installed
bytes. Python bytecode is included because it can execute: a legitimate first
warmup may change that inventory and must trigger remeasurement, not silent reuse.
The adapter reads files only; it neither imports Python nor runs CPU/GPU inference.

This adapter currently supports the neural Windows package, not arbitrary external
Tesseract installations. Its file fingerprint does not attest binary trust,
CUDA availability, OS/driver stability, or absence of a hostile filesystem race.
Keep workers stopped while taking/rechecking an inventory, compare the saved
fingerprint before resume, and preserve a mismatch for explicit review. No
automatic inspector restart or cache/receipt write is authorized by this helper.

Synthetic tests change actual temporary model, worker, interpreter, bytecode,
shared-plugin and addon files and verify fingerprint separation/refusal. The
opt-in runtimeSnapshotDeployed diagnostic writes its measurement outside the
isolated application directory; its application hash identifies the diagnostic
executable, not the GUI app. It performs no OCR and is not a GPU success test.

## Cache-before-receipt connection

LocalOcrPersistence binds a supplied, validated settings measurement and neural
page evidence to an existing job's selected page. It derives the immutable cache
identity from the exact PNG hash, preparation geometry/options and the interpreter
that handled that attempt. A GPU interpreter that internally used CPU still uses
the GPU package identity; a separate CPU retry uses the CPU package identity.
Requested/actual devices and selected-page positions cannot be silently relabelled.

recordPage validates the snapshot/job/page binding, saves the exact raw cache
payload first, then asks the store for a lease-fenced receipt. The production clock
is sampled after cache publication and after acquiring SQLite's write transaction,
so a lease that expired while waiting for disk/DB access cannot use a stale
pre-wait timestamp. The store's fixed-time overload remains for logical-time callers;
production persistence uses recordPageWhenCurrent. Cancellation or a rejected lease
may leave immutable cache data without a DB receipt. Preserve this artifact and
explicitly resume with a fresh lease; no completed page may replace different
committed evidence.

The helper never finishes a job. Even all saved pages leave it Running until the
executor records the separate batch outcome; failed worker exit must stay Failed,
not become candidate-free success. Source identity/revalidation, stopped-runtime
measurement, GPU ownership and validated cache reload are still caller obligations.
This is not an inspector scheduler or a license to save unreviewed metadata.

Geometry enters the preprocessing fingerprint; the existing cache stores raw OCR
and prepared dimensions, not a full original-page overlay. On resume, recreate and
verify preparation from the saved options/source before mapping cached regions.
Synthetic tests cover cache refusal, mismatched settings, changed committed bytes,
late/cancelled receipts, sparse page positions and unchanged raw data across an
abrupt child exit after cache publication. A real SQLite writer holds the lock
while the test clock advances beyond expiry, proving the post-lock clock boundary.
These changes require Windows CI and exact downloaded-probe validation.

## Current clocks for all lease lifecycle operations

The production claimWhenCurrent, heartbeatWhenCurrent, finishWhenCurrent,
failWhenCurrent and interruptExpiredWhenCurrent methods sample a nonthrowing clock
after BEGIN IMMEDIATE acquires the write lock, matching recordPageWhenCurrent.
Fixed-time overloads delegate with a constant clock for existing logical-time
callers; a production executor must use the current-clock entry points.

A blocked claim must not start with a lease already expired because of DB waiting.
A blocked heartbeat must not revive an expired lease, nor may finish/fail use an
old timestamp to change its state. Expiry scanning also samples after the lock,
so an old pre-lock time cannot masquerade as a backwards clock jump against a
newer owner. Empty clocks and invalid sampled times are refused transactionally.

Five synthetic contention cases use another SQLite writer and advance a logical
clock while it holds the real write lock. They cover claim, renewal, completion,
failure and interruption, including rollback and retained receipts. No worker,
original media or library database is involved. This still does not serialize
physical GPU access or connect an executor to the inspector.


## Verified boundary and next integration

Code 9d88c0db passed Windows run34443926773 including all 13 CTest suites,
installed OCR, downloaded persistence/process checks and 23 job-store checks.
The exact local diagnostic passed persistence (5) and job-store (23) totals with
no failures/skips. These are synthetic persistence tests, not new OCR accuracy.

The measured runtime adapter was separately checked against actual private CPU/GPU
package files, twice with identical snapshots and independently checked hashes.
Full file inventory involves substantial disk I/O; do not run it once per page.
Design a stopped-worker deployment/session boundary, explicit resume remeasurement
and invalidation policy before caching inventory results. PNG metadata is part
of exact input bytes: regenerated pixels alone do not establish the same cache key.

Next connect validated per-page delivery to the helper, preserving sparse retry
index mapping and separate batch failure. Construct the Store on its executor
thread and use current-clock lifecycle APIs. Revalidate source/page identity and
cached artifacts; retain corrupt or mismatched evidence for explicit recovery.
Physical GPU ownership must outlive an individual dialog and must verify old
worker exit before handing off. Only then add explicit restart/review UI in a
separate test library. No automatic metadata save or full-library execution.

## Revalidating selected source pages and durable cache receipts

LocalOcrSource::read accepts one absolute folder/archive and first/last one to
three pages. It reuses the existing decoder, retains exact selected encoded-page
SHA256 values and the complete naturally sorted image-entry list, then compares
source location, kind, entry list and file stamps before/after reading. Duplicate
archive entry names, unreadable selected pages, cancellation and changed sources
are refused. The private fingerprint includes location, kind, selection, all page
names and selected raw hashes. It does not hash or OCR the unselected interior
pages. This is a captured input identity, not a filesystem lock for later writes;
explicit resume must capture again and compare before using the old job.

LocalOcrPersistence::restorePage is read-only. Given a checked Job, newly prepared
geometry/exact PNG hash and a fresh runtime measurement, it matches the durable
receipt key against possible requested CPU/GPU package identities. A GPU package
returning CPU remains distinct from the CPU package. The cache envelope, parser,
actual device, language and raw response digest must also match. Missing receipts,
changed inputs/settings, corrupt caches and cancellation return an error without
changing evidence. Restoring a blank response from a failed job does not turn
that job into a successful or candidate-free session.

Synthetic tests cover folder/archive ordering, same pixels with different encoded
metadata, interior-page rename, duplicate entries, invalid pages, all three
package/device combinations and damaged cache preservation. The synthetic worker
fault suite also connects each accepted live page delivery through cache/save,
SQLite receipt and validated reload. Runtime measurements in this suite are
synthetic fixtures, not claims of actual GPU inference. Windows validation for
this integration is pending. A production owned executor and explicit UI resume
remain subsequent work.

## Cooperating Windows NVIDIA worker ownership

Neural GPU requests now acquire the Windows resource nvidia-0 before process
creation. A named mutex serializes cooperating app instances in the same Windows
session; its named Job retains the worker tree for handoff checks. Acquisition
and cleanup waits are bounded and cancellable. A busy or unverified old resource
is an execution error before GPU start, not authorization for a CPU retry.

WAIT_ABANDONED transfers mutex ownership but does not establish worker exit.
Only the corresponding named OCR Job is eligible for abandoned-owner termination;
its active-process count must reach zero before a new worker is assigned. Normal
cleanup closes the Job handle before releasing the mutex. Later cleanup calls on
an old object cannot act on a Job reused by a new worker. Because Windows mutexes
are recursive, a thread-local guard also requires the previous invocation to
explicitly finish even when its root process exited naturally.

This coordinates these cooperating OCR requests, not unrelated GPU software or
older installations without this protocol. Synthetic tests use unique resource
names and keep Job/mutex handles open to leave real descendants alive after an
owner crash, then verify handoff cleanup. They also cover cancellation while
waiting, normal handoff, same-thread reentry and natural-exit reuse. No NVIDIA
kernels are needed for these ownership tests. Full Windows/package validation for
this integration is pending; a durable inspector executor/restart UI is still
subsequent work.

Primary Windows semantics: [Job objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
and [mutex wait results](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject).

## Executing one already-claimed selected work

LocalOcrExecutor::executeClaimed connects the source/runtime adapters, validated
receipt reload, neural runner, page sink and job state. It must run on the Store's
owning thread. The caller still verifies the current library generation, captures
the source, measures a stopped runtime and explicitly claims/resumes the job.
This function does not enqueue jobs, measure files, open a library DB or provide
restart UI. Those caller/UI boundaries remain required before production use.

It checks the saved source/selection/settings, prepares exact input identities,
restores only matching receipts and runs only the missing selected images. Two
index mappings compose: CPU retry maps to the missing-image vector, then the
executor maps to the full selected plan before cache and DB persistence. Each
page's cache-hit flag and total cache-read time are separate from original cached
OCR device/timing. Candidate text is reparsed from validated raw evidence; an
invalid or divergent runner display string cannot create a candidate.

A current-clock heartbeat fences the start and progress. Worker cancellation uses
pauseWhenCurrent, which checks the lease after the SQLite write lock, so an old
worker cannot pause a successor. Receipt failure, ownership loss, invalid results
and nonzero exits cannot enter review. Even all cached receipts are insufficient
to infer successful completion of an earlier interrupted/failed session; this
case keeps the work for explicit review without automatically repeating all OCR.
Only a successful, fully persisted execution enters page or filename review.
Metadata still requires user review/save through the existing workflow.

Synthetic executor cases cover fresh/mixed-cache processing, page versus filename
review, cancellation, lease expiry and stale owners, changed source/settings,
missing delivery, corrupt cache, changed prepared input, exceptions and failure
after all outputs. The store also tests stale-worker pause and real SQLite lock
contention for pause. These new integration tests await Windows/package validation.


Completion evidence must also equal the page accepted by the persistence sink.
A valid but different raw response, digest or requested/actual device is rejected
and cannot supply candidate text. This protects the final review state from a
runner reporting a different result after persistence. Isolated synthetic-worker
integration covers one cached page plus two live pages, sparse CPU retry, exact
receipt reload, cancellation and nonzero exit after all output. Device labels in
this fault fixture are simulated; it does not execute NVIDIA kernels.

The neural worker is launched with Python -B as well as isolated mode, preventing
import-time bytecode writes from changing the measured deployed runtime. The
opt-in actual neural executor test compares runtime identity before and after a
first page, then explicitly resumes the second synthetic CJK page. It measures
CPU and physical GPU separately; no real library images are included in CI.
