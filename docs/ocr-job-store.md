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
kind), settings fingerprint, total page count and ordered selected pages.
The source and settings fingerprints must already have been calculated by the
caller. At most six distinct ascending pages from the first/last three are allowed.
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
The private diagnostic executable additionally accepts
`--local-ocr-cache manifest.json NEWreport.json` for at most six saved responses.
Each page supplies an absolute result path and a complete identity object using
the cache envelope field names. The report and its new .cache directory must not
exist. This probe validates and round-trips existing results; it never runs OCR
or establishes that the caller's claimed engine fingerprints are true.
