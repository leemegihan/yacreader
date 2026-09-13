#include "local_ocr_preflight.h"

namespace LocalOcrPreflight {
std::optional<Prepared> read(const QString &libraryRoot, qulonglong comicInfoId,
                             const QString &sourcePath, int perEnd, const QString &applicationPath,
                             const LocalMetadata::OcrOptions &options, QString *error,
                             const LocalMetadata::Cancellation &cancel, const LocalMetadata::Progress &progress)
{
    if (error)
        error->clear();
    const auto fail = [&](const QString &reason) -> std::optional<Prepared> {
        if (error)
            *error = reason;
        return std::nullopt;
    };
    const auto flag = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
    if (perEnd < 1 || perEnd > 3 || !options.neural)
        return fail(QStringLiteral("Selected durable OCR requires neural mode and at most three pages per end."));
    if (flag->load())
        return fail(QStringLiteral("Selected OCR preparation cancelled."));
    const auto pulse = [&](int step, const QString &text) {
        if (progress)
            progress(step, 4, text);
        return !flag->load();
    };
    try {
        if (!pulse(0, QStringLiteral("선택한 라이브러리 항목 확인 중")))
            return fail(QStringLiteral("Selected OCR preparation cancelled."));
        const auto before = LocalOcrLibrary::read(libraryRoot, comicInfoId, sourcePath, error);
        if (!before)
            return std::nullopt;
        if (!pulse(1, QStringLiteral("OCR 실행 환경 확인 중")))
            return fail(QStringLiteral("Selected OCR preparation cancelled."));
        const auto runtime = LocalOcrRuntime::measure(applicationPath, options, flag, error, progress);
        if (!runtime)
            return std::nullopt;
        if (!pulse(2, QStringLiteral("선택한 앞뒤 페이지를 불러오는 중")))
            return fail(QStringLiteral("Selected OCR preparation cancelled."));
        const auto source = LocalOcrSource::read(before->sourcePath, perEnd, flag, error);
        if (!source)
            return std::nullopt;
        if (!pulse(3, QStringLiteral("선택한 라이브러리 항목 다시 확인 중")))
            return fail(QStringLiteral("Selected OCR preparation cancelled."));
        const auto after = LocalOcrLibrary::read(before->libraryRoot, comicInfoId, before->sourcePath, error);
        if (!after)
            return std::nullopt;
        if (before->generation != after->generation || before->comicId != after->comicId || before->comicInfoId != after->comicInfoId || before->libraryRoot != after->libraryRoot || before->sourcePath != after->sourcePath || source->manifest["path"].toString() != after->sourcePath)
            return fail(QStringLiteral("Selected library identity changed during OCR preparation."));
        if (flag->load())
            return fail(QStringLiteral("Selected OCR preparation cancelled."));
        OcrJobs::Spec spec;
        spec.libraryGeneration = after->generation;
        spec.comicId = after->comicId;
        spec.sourceSnapshot = source->fingerprint;
        spec.sourceContext = { { "path", source->manifest["path"] }, { "sourceKind", source->manifest["sourceKind"] }, { "libraryRoot", after->libraryRoot } };
        spec.settingsSnapshot = runtime->settingsSnapshot;
        spec.settingsFingerprint = runtime->settingsFingerprint;
        spec.totalPages = source->source.pageCount;
        for (const auto &page : source->source.pages)
            spec.pages.append(page.number);
        // No callback follows the final identity check: the caller receives this
        // snapshot directly and must revalidate again at a later resume/save.
        return Prepared { *after, *source, *runtime, spec };
    } catch (...) {
        return fail(QStringLiteral("Selected OCR preparation failed unexpectedly."));
    }
}
}
