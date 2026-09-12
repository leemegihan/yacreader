#include "yacreader_archive_inspector_dialog.h"

#include "ocr_page_view.h"
#include "yacreader_global.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
bool usableEvidence(const LocalMetadata::Suggestion &candidate, const LocalMetadata::Result &result, bool automatic, double minimum)
{
    for (const auto &evidence : candidate.evidence) {
        if (evidence.confidence < minimum || (automatic && !evidence.labelled))
            continue;
        for (const auto &page : result.pages) {
            if (page.number == evidence.page && page.error.isEmpty() && !page.reading.uncertainLanguage && (!automatic || (!page.reading.reviewRequired && page.kind != LocalMetadata::PageKind::Unknown)))
                return true;
        }
    }
    return false;
}
}

YACReaderArchiveInspectorDialog::YACReaderArchiveInspectorDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("앞·뒤 페이지로 작가·제목 찾기 (로컬 OCR)"));
    resize(1100, 780);
    fileLabel = new QLabel(this);
    fileLabel->setTextFormat(Qt::PlainText);
    fileLabel->setWordWrap(true);
    statusLabel = new QLabel(this);
    statusLabel->setTextFormat(Qt::PlainText);
    statusLabel->setWordWrap(true);
    pageLimit = new QSpinBox(this);
    pageLimit->setRange(1, 3);
    pageLimit->setValue(3);
    pageLimit->setSuffix(tr("장씩 (앞 / 뒤)"));
    pageLimit->setToolTip(tr("앞뒤 최대 3장에서 후보를 찾고, 없으면 파일명·외부 메타데이터 대조로 이어갑니다."));
    language = new QComboBox(this);
    language->addItem(tr("자동 · 페이지마다 일본어/한국어 비교"), "auto");
    language->addItem(tr("일본어 + 영어"), "jpn+eng");
    language->addItem(tr("일본어 세로쓰기"), "jpn_vert+eng");
    language->addItem(tr("한국어 + 영어"), "kor+eng");
    language->addItem(tr("영어"), "eng");
    language->addItem(tr("일본어 + 한국어 + 영어"), "jpn+kor+eng");
    quality = new QComboBox(this);
    quality->addItem(tr("정밀 모델 (느림)"), true);
    quality->addItem(tr("빠른 모델"), false);
    if (QFileInfo::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-neural/worker.py")))
        quality->addItem(tr("영역 탐지 OCR (시험 · 후보 직접 확인)"), 2);
    performance = new QComboBox(this);
    performance->addItem(tr("CPU 균형 · 최대 8스레드"), 8);
    performance->addItem(tr("CPU 여유 · 최대 4스레드"), 4);
    performance->addItem(tr("CPU 최대 · 최대 16스레드"), 16);
    if (QFileInfo::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/ocr-neural-gpu/runtime/python.exe")))
        performance->addItem(tr("NVIDIA GPU · 실패 시 CPU"), -1);
    performance->setToolTip(tr("영역 탐지 OCR의 처리 장치를 선택합니다. 스레드를 늘려도 항상 빨라지지는 않습니다."));
    ocrButton = new QPushButton(tr("페이지 글자 읽기"), this);
    cancelButton = new QPushButton(tr("중지"), this);
    resumeButton = new QPushButton(tr("저장 결과 열기 / 이어 읽기"), this);
    resumeButton->setToolTip(tr("같은 작품·페이지·OCR 설정의 완료 결과를 열거나 정상적으로 중지한 작업을 이어 읽습니다. 실패한 작업은 자동 재시작하지 않습니다."));
    auto *controls = new QHBoxLayout;
    controls->addWidget(pageLimit);
    controls->addWidget(language);
    controls->addWidget(quality);
    controls->addWidget(ocrButton);
    controls->addWidget(cancelButton);

    pageList = new QListWidget(this);
    pageList->setMaximumWidth(170);
    imageLabel = new OcrPageView(this);
    auto *imageScroll = new QScrollArea(this);
    imageScroll->setWidget(imageLabel);
    textLayout = new QComboBox(this);
    textLayout->addItem(tr("흩어진 글자"), 11);
    textLayout->addItem(tr("판권란 / 글자 블록"), 6);
    textLayout->addItem(tr("한 줄"), 7);
    textLayout->addItem(tr("세로 글자 블록"), 5);
    rotation = new QComboBox(this);
    rotation->addItem(tr("회전 없음"), 0);
    rotation->addItem(tr("시계 방향 90°"), 90);
    rotation->addItem(tr("반시계 방향 90°"), -90);
    rotation->addItem(tr("180°"), 180);
    invert = new QCheckBox(tr("흰 글자 반전"), this);
    threshold = new QCheckBox(tr("불균일한 배경 보정"), this);
    regionButton = new QPushButton(tr("선택 영역 다시 읽기"), this);
    auto *regionControls = new QHBoxLayout;
    regionControls->addWidget(textLayout);
    regionControls->addWidget(rotation);
    regionControls->addWidget(invert);
    regionControls->addWidget(threshold);
    regionControls->addWidget(regionButton);
    pageText = new QPlainTextEdit(this);
    pageText->setReadOnly(true);
    pageInfo = new QLabel(this);
    pageInfo->setTextFormat(Qt::PlainText);
    pageInfo->setWordWrap(true);
    pageText->setPlaceholderText(tr("OCR 결과는 여기에 표시됩니다. 필요한 글자를 선택해 제목·작가 칸에 복사할 수 있습니다."));
    candidateList = new QListWidget(this);
    candidateList->setWordWrap(true);
    candidateList->setMaximumHeight(160);
    auto *textPanel = new QWidget(this);
    auto *textPanelLayout = new QVBoxLayout(textPanel);
    textPanelLayout->addWidget(new QLabel(tr("선택한 페이지의 글자 (오인식 가능)"), this));
    textPanelLayout->addWidget(pageInfo);
    textPanelLayout->addWidget(pageText, 1);
    textPanelLayout->addWidget(new QLabel(tr("후보를 두 번 클릭하면 아래 입력칸에 복사됩니다."), this));
    textPanelLayout->addWidget(candidateList);
    auto *splitter = new QSplitter(this);
    splitter->addWidget(pageList);
    splitter->addWidget(imageScroll);
    splitter->addWidget(textPanel);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 2);
    titleEdit = new QLineEdit(this);
    authorEdit = new QLineEdit(this);
    publisherEdit = new QLineEdit(this);
    auto *form = new QFormLayout;
    form->addRow(tr("확인한 제목"), titleEdit);
    form->addRow(tr("확인한 작가"), authorEdit);
    form->addRow(tr("발행자·서클 후보 (작가와 구분)"), publisherEdit);
    autoSearch = new QCheckBox(tr("명확한 판권 후보를 찾으면 E-Hentai 자동 조회 (제목·이름 힌트 전송)"), this);
    autoSearch->setChecked(true);
    overwrite = new QCheckBox(tr("기존 제목·작가를 입력한 값으로 덮어쓰기 (기본: 빈 항목만 채움)"), this);
    saveButton = new QPushButton(tr("확인한 정보 저장"), this);
    searchButton = new QPushButton(tr("메타데이터 대조…"), this);
    searchButton->setToolTip(tr("제목·작가·이름 힌트로 외부 메타데이터를 조회합니다. 이미지는 전송하지 않습니다."));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(saveButton, QDialogButtonBox::ActionRole);
    buttons->addButton(searchButton, QDialogButtonBox::ActionRole);
    auto *privacy = new QLabel(tr("이미지는 PC 안에서만 처리합니다. 후보는 추정이며 자동 저장하지 않습니다. 원본과 읽던 위치는 변경하지 않습니다."), this);
    privacy->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(fileLabel);
    layout->addWidget(privacy);
    layout->addLayout(controls);
    auto *deviceControls = new QHBoxLayout;
    deviceControls->addWidget(new QLabel(tr("영역 OCR 처리 장치"), this));
    deviceControls->addWidget(performance);
    deviceControls->addWidget(resumeButton);
    deviceControls->addStretch();
    layout->addLayout(deviceControls);
    layout->addWidget(statusLabel);
    layout->addWidget(splitter, 1);
    layout->addWidget(new QLabel(tr("이미지에서 제목이나 작가명 부분을 드래그한 뒤 다시 읽어 보세요. 선택하지 않으면 현재 페이지를 읽습니다."), this));
    layout->addLayout(regionControls);
    layout->addLayout(form);
    layout->addWidget(overwrite);
    layout->addWidget(autoSearch);
    layout->addWidget(buttons);

    connect(quality, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool neural = quality->currentData().toInt() == 2;
        textLayout->setEnabled(!neural);
        threshold->setEnabled(!neural);
        performance->setEnabled(neural);
        resumeButton->setEnabled(neural && !activeWorker && !sourcePath.isEmpty());
    });
    connect(ocrButton, &QPushButton::clicked, this, [this] { start(true); });
    connect(resumeButton, &QPushButton::clicked, this, [this] { startSaved(LocalOcrSession::Action::Continue); });
    connect(regionButton, &QPushButton::clicked, this, &YACReaderArchiveInspectorDialog::recognizeRegion);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        cancel();
        setBusy(bool(activeWorker));
        cancelButton->setEnabled(false);
        statusLabel->setText(activeWorker ? tr("중지 요청했습니다. 작업 정리가 끝날 때까지 기다려 주세요.") : tr("중지했습니다. 다시 실행할 수 있습니다."));
    });
    connect(pageList, &QListWidget::currentRowChanged, this, &YACReaderArchiveInspectorDialog::showPage);
    connect(candidateList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const int row = candidateList->row(item);
        if (row < 0 || row >= result.suggestions.size())
            return;
        const auto &suggestion = result.suggestions.at(row);
        (suggestion.field == LocalMetadata::Suggestion::Title ? titleEdit : suggestion.field == LocalMetadata::Suggestion::Author ? authorEdit
                                                                                                                                  : publisherEdit)
                ->setText(suggestion.value);
        for (int i = 0; i < result.pages.size(); ++i) {
            if (result.pages.at(i).number == suggestion.page)
                pageList->setCurrentRow(i);
        }
    });
    connect(saveButton, &QPushButton::clicked, this, &YACReaderArchiveInspectorDialog::save);
    connect(searchButton, &QPushButton::clicked, this, [this] { requestSearch(false); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, &YACReaderArchiveInspectorDialog::cancel);
    setBusy(false);
}

YACReaderArchiveInspectorDialog::~YACReaderArchiveInspectorDialog()
{
    cancel();
}

void YACReaderArchiveInspectorDialog::cancel()
{
    pendingPreview = false;
    if (cancellation)
        cancellation->store(true);
}

QThread *YACReaderArchiveInspectorDialog::createWorker(const LocalMetadata::Cancellation &flag,
                                                       const std::function<void()> &work, const std::function<void()> &completed, bool deliverCancelled)
{
    if (activeWorker)
        return nullptr;
    auto *thread = QThread::create(work);
    activeWorker = thread;
    connect(thread, &QThread::finished, this, [this, flag, completed, deliverCancelled] {
        activeWorker.clear();
        if (pendingPreview) {
            pendingPreview = false;
            start(false);
            return;
        }
        setBusy(false);
        if (flag != cancellation)
            return;
        if (flag->load() && !deliverCancelled) {
            statusLabel->setText(tr("중지했습니다. 다시 실행할 수 있습니다."));
            return;
        }
        completed();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    return thread;
}

void YACReaderArchiveInspectorDialog::setBusy(bool busy)
{
    ocrButton->setEnabled(!busy && !sourcePath.isEmpty());
    resumeButton->setEnabled(!busy && !sourcePath.isEmpty() && quality->currentData().toInt() == 2);
    cancelButton->setEnabled(busy);
    pageLimit->setEnabled(!busy);
    language->setEnabled(!busy);
    quality->setEnabled(!busy);
    performance->setEnabled(!busy && quality->currentData().toInt() == 2);
    textLayout->setEnabled(!busy && quality->currentData().toInt() != 2);
    rotation->setEnabled(!busy);
    invert->setEnabled(!busy);
    threshold->setEnabled(!busy && quality->currentData().toInt() != 2);
    imageLabel->setEnabled(!busy);
    regionButton->setEnabled(!busy && !result.pages.isEmpty());
    saveButton->setEnabled(!busy && !sourcePath.isEmpty());
    searchButton->setEnabled(!busy && !sourcePath.isEmpty());
}

void YACReaderArchiveInspectorDialog::inspectComic(const QString &path, qulonglong id)
{
    cancel();
    cancellation.reset(); // Preserve a new selection error when old cleanup finishes.
    libraryPath = path;
    comicInfoId = id;
    savedSelection.reset();
    savedSourceFingerprint.clear();
    titleEdit->clear();
    authorEdit->clear();
    publisherEdit->clear();
    pageInfo->clear();
    automaticSearchDone = false;
    filenameFallback = false;
    searchButton->setText(tr("메타데이터 대조…"));
    overwrite->setChecked(false);
    result = { };
    pageList->clear();
    candidateList->clear();
    imageLabel->clear();
    pageText->clear();
    QString error;
    sourcePath = LocalMetadata::comicPath(path, id, &error);
    fileLabel->setText(sourcePath);
    setBusy(bool(activeWorker));
    if (sourcePath.isEmpty()) {
        statusLabel->setText(error);
        return;
    }
    if (activeWorker) {
        pendingPreview = true;
        cancelButton->setEnabled(false);
        statusLabel->setText(tr("이전 작업을 정리한 뒤 선택한 작품을 불러옵니다."));
        return;
    }
    start(false);
}

void YACReaderArchiveInspectorDialog::start(bool ocr)
{
    if (ocr && ocrOptions().neural) {
        startSaved(LocalOcrSession::Action::Start);
        return;
    }
    if (sourcePath.isEmpty() || activeWorker)
        return;
    cancel();
    cancellation = std::make_shared<std::atomic_bool>(false);
    const auto flag = cancellation;
    const auto source = sourcePath;
    const auto root = libraryPath;
    const int count = pageLimit->value();
    const auto options = ocrOptions();
    const auto output = std::make_shared<LocalMetadata::Result>();
    setBusy(true);
    statusLabel->setText(ocr ? tr("PC에서 앞·뒤 페이지를 읽는 중… 페이지당 시간이 걸릴 수 있습니다. 중지 버튼으로 취소할 수 있습니다.") : tr("앞·뒤 페이지를 불러오는 중…"));
    QElapsedTimer elapsed;
    elapsed.start();
    auto relay = std::shared_ptr<OcrProgressRelay>(new OcrProgressRelay, [](OcrProgressRelay *value) { value->deleteLater(); });
    auto stageText = std::make_shared<QString>(statusLabel->text());
    connect(relay.get(), &OcrProgressRelay::changed, this, [this, flag, stageText](int completed, int total, const QString &stage) {
        if (!flag->load() && flag == cancellation)
            *stageText = tr("%1 / %2장 완료 · %3").arg(completed).arg(total).arg(stage);
    });
    auto *ticker = new QTimer(this);
    connect(ticker, &QTimer::timeout, this, [this, flag, stageText, elapsed, ticker] {
        if (flag->load() || flag != cancellation) {
            ticker->stop();
            ticker->deleteLater();
            return;
        }
        statusLabel->setText(*stageText + tr(" · 경과 %1초").arg(elapsed.elapsed() / 1000));
    });
    ticker->start(500);
    // Worker owns copied values only; cancelled/stale results never touch the UI.
    auto *thread = createWorker(flag, [source, root, count, options, flag, output, ocr, relay] {
        const LocalMetadata::Progress progress = [relay](int completed, int total, const QString &stage) { emit relay->changed(completed, total, stage); };
        *output = ocr ? LocalMetadata::analyze(source, count, options, flag, progress) : LocalMetadata::readPages(source, count, flag);
        output->suggestions = LocalMetadata::suggest(output->pages, source, root); }, [this, output, ocr, elapsed] {
        showResult(*output, ocr);
        if (ocr)
            statusLabel->setText(statusLabel->text() + tr(" · 총 %1초").arg(elapsed.elapsed() / 1000));
        setBusy(false);
        if (ocr)
            requestSearch(true); });
    connect(thread, &QThread::finished, ticker, &QTimer::stop);
    connect(thread, &QThread::finished, ticker, &QObject::deleteLater);
    thread->start();
}

void YACReaderArchiveInspectorDialog::startSaved(LocalOcrSession::Action action)
{
    if (sourcePath.isEmpty() || activeWorker || !ocrOptions().neural)
        return;
    cancel();
    cancellation = std::make_shared<std::atomic_bool>(false);
    const auto flag = cancellation;
    LocalOcrSession::Request request;
    request.libraryRoot = libraryPath;
    request.comicInfoId = comicInfoId;
    request.sourcePath = sourcePath;
    request.perEnd = pageLimit->value();
    request.applicationPath = QCoreApplication::applicationFilePath();
    request.dataRoot = QDir(YACReader::getCommonSettingsPath()).filePath(QStringLiteral("local-ocr/v1"));
    request.options = ocrOptions();
    const auto output = std::make_shared<LocalOcrSession::Outcome>();
    setBusy(true);
    statusLabel->setText(tr("선택한 작품과 저장된 OCR 기록을 확인하는 중…"));
    QElapsedTimer elapsed;
    elapsed.start();
    auto relay = std::shared_ptr<OcrProgressRelay>(new OcrProgressRelay, [](OcrProgressRelay *value) { value->deleteLater(); });
    connect(relay.get(), &OcrProgressRelay::changed, this, [this, flag](int completed, int total, const QString &stage) {
        if (!flag->load() && flag == cancellation)
            statusLabel->setText(total > 0 ? tr("%1 / %2장 완료 · %3").arg(completed).arg(total).arg(stage) : stage);
    });
    auto *thread = createWorker(flag, [request, action, flag, output, relay] {
        const LocalMetadata::Progress preparation = [relay](int, int, const QString &stage) { emit relay->changed(0, 0, stage); };
        const LocalMetadata::Progress recognition = [relay](int completed, int total, const QString &stage) { emit relay->changed(completed, total, stage); };
        *output = LocalOcrSession::run(request, action, flag, preparation, recognition); }, [this, output, elapsed, count = request.perEnd] { showSavedResult(*output, elapsed.elapsed(), count); }, true);
    thread->start();
}

void YACReaderArchiveInspectorDialog::showSavedResult(const LocalOcrSession::Outcome &output, qint64 elapsedMs, int perEnd)
{
    savedSelection = output.library;
    savedSourceFingerprint = output.sourceFingerprint;
    savedPerEnd = perEnd;
    showResult(output.metadata, output.complete);
    if (output.state == OcrJobs::State::Paused) {
        statusLabel->setText(tr("정상적으로 중지했습니다. 같은 설정에서 저장 결과 열기 / 이어 읽기를 누르면 남은 페이지를 처리합니다."));
        return;
    }
    if (!output.error.isEmpty()) {
        statusLabel->setText(output.error);
        return;
    }
    if (output.restored)
        statusLabel->setText(statusLabel->text() + tr(" · 저장 결과 %1장 복원 · 이번 열기 %2초 (페이지 시간은 원래 OCR 기록)").arg(output.cachedPages).arg(elapsedMs / 1000));
    else
        statusLabel->setText(statusLabel->text() + tr(" · 기존 결과 %1장 재사용 · 이번 처리 %2초").arg(output.cachedPages).arg(elapsedMs / 1000));
    // Opening a completed result must not issue an external lookup again.
    if (output.complete && !output.restored)
        requestSearch(true);
}

void YACReaderArchiveInspectorDialog::showResult(const LocalMetadata::Result &value, bool ocrComplete)
{
    result = value;
    pageList->clear();
    candidateList->clear();
    int failures = 0;
    for (const auto &page : result.pages) {
        pageList->addItem(tr("%1 / %2 · %3%4").arg(page.number).arg(result.pageCount).arg(LocalMetadata::pageKindName(page.kind), page.error.isEmpty() && page.reading.error.isEmpty() ? QString() : tr(" (오류)")));
        if (!page.error.isEmpty() || !page.reading.error.isEmpty())
            ++failures;
    }
    for (const auto &candidate : result.suggestions) {
        const auto field = candidate.field == LocalMetadata::Suggestion::Title ? tr("제목") : candidate.field == LocalMetadata::Suggestion::Author ? tr("작가")
                                                                                                                                                   : tr("발행자·서클");
        const auto source = LocalMetadata::suggestionSource(candidate) + (candidate.page > 0 && !candidate.labelled ? tr(" · 추정") : QString());
        auto *item = new QListWidgetItem(QStringLiteral("[%1 · %2] %3").arg(field, source, candidate.value), candidateList);
        item->setToolTip(candidate.reason);
    }
    // Only an unambiguous labelled credit can prefill a blank review field.
    // Never promote a filename, dialogue line, or conflicting OCR reading.
    for (const auto field : { LocalMetadata::Suggestion::Title, LocalMetadata::Suggestion::Author, LocalMetadata::Suggestion::Publisher }) {
        QStringList values;
        for (const auto &candidate : result.suggestions) {
            const bool needsReview = std::any_of(candidate.evidence.cbegin(), candidate.evidence.cend(), [&](const LocalMetadata::SuggestionEvidence &evidence) {
                return std::any_of(result.pages.cbegin(), result.pages.cend(), [&](const LocalMetadata::Page &page) {
                    return page.number == evidence.page && (page.reading.reviewRequired || page.reading.uncertainLanguage || !page.error.isEmpty());
                });
            });
            if (!needsReview && candidate.field == field && candidate.labelled && candidate.page > 0 && (candidate.confidence < 0 || candidate.confidence >= 45) && !values.contains(candidate.value, Qt::CaseInsensitive))
                values.append(candidate.value);
        }
        auto *edit = field == LocalMetadata::Suggestion::Title ? titleEdit : field == LocalMetadata::Suggestion::Author ? authorEdit
                                                                                                                        : publisherEdit;
        if (edit->text().isEmpty() && values.size() == 1)
            edit->setText(values.first());
    }
    const bool pageCandidate = std::any_of(result.suggestions.cbegin(), result.suggestions.cend(), [](const LocalMetadata::Suggestion &candidate) { return candidate.page > 0; });
    const bool readingFailure = std::any_of(result.pages.cbegin(), result.pages.cend(), [](const LocalMetadata::Page &page) { return !page.reading.error.isEmpty(); });
    filenameFallback = ocrComplete && !result.pages.isEmpty() && result.error.isEmpty() && failures == 0 && !readingFailure && !pageCandidate;
    searchButton->setText(filenameFallback ? tr("파일명으로 메타데이터 대조…") : tr("메타데이터 대조…"));
    statusLabel->setText(!result.error.isEmpty() ? result.error : filenameFallback ? tr("선택한 앞뒤 페이지에서 식별 후보를 찾지 못했습니다. 페이지를 더 늘리지 않고 파일명·이름 힌트로 메타데이터를 대조해 주세요.")
                                                                                   : tr("%1장 확인, %2장 오류. 후보와 원본을 대조해 사용할 정보를 선택해 주세요.").arg(result.pages.size()).arg(failures));
    int preferred = 0;
    for (int i = 0; i < result.pages.size(); ++i) {
        if (result.pages.at(i).kind == LocalMetadata::PageKind::Colophon) {
            preferred = i;
            break;
        }
    }
    if (!result.pages.isEmpty())
        pageList->setCurrentRow(preferred);
}

void YACReaderArchiveInspectorDialog::showPage(int row)
{
    imageLabel->clear();
    pageText->clear();
    pageInfo->clear();
    if (row < 0 || row >= result.pages.size())
        return;
    const auto &page = result.pages.at(row);
    imageLabel->setPage(page.image);
    pageInfo->setText(tr("%1 · %2 · 인식된 글자 점수 %3 (누락 영역은 평가되지 않음)%4")
                              .arg(LocalMetadata::pageKindName(page.kind), page.reading.language,
                                   page.reading.confidence < 0 ? tr("없음") : QString::number(page.reading.confidence, 'f', 0),
                                   page.reading.uncertainLanguage ? tr(" · 언어 판정 불확실") : QString()));
    if (page.reading.reviewRequired)
        pageInfo->setText(pageInfo->text() + tr(" · 영역 탐지 OCR 시험 결과 — 후보를 직접 선택해 주세요."));
    if (!page.reading.device.isEmpty())
        pageInfo->setText(pageInfo->text() + tr(" · %1 · 읽기 %2초 / 모델 준비 %3초").arg(page.reading.device == "gpu:0" ? tr("NVIDIA GPU") : tr("CPU")).arg(page.reading.elapsedMs / 1000.0, 0, 'f', 1).arg(page.reading.initializationMs / 1000.0, 0, 'f', 1));
    if (!page.reading.warning.isEmpty())
        pageInfo->setText(pageInfo->text() + QStringLiteral(" · ") + page.reading.warning);
    QString displayText = page.error.isEmpty() ? page.text : page.error + QStringLiteral("\n\n") + page.text;
    if (!page.reading.alternatives.isEmpty()) {
        displayText += tr("\n\n다른 언어 판독 · 미확정 (원본과 대조해 주세요)");
        for (const auto &alternative : page.reading.alternatives) {
            const auto primary = std::find_if(page.reading.lines.cbegin(), page.reading.lines.cend(), [&](const LocalMetadata::TextLine &line) { return line.bounds == alternative.bounds; });
            if (primary != page.reading.lines.cend())
                displayText += tr("\n기본 판독: %1").arg(primary->text);
            displayText += tr("\n대안 (%1 · 점수 %2): %3\n").arg(alternative.language == "jpn" ? tr("일본어") : tr("한국어"), QString::number(alternative.confidence, 'f', 0), alternative.text);
        }
    }
    pageText->setPlainText(displayText);
}

LocalMetadata::OcrOptions YACReaderArchiveInspectorDialog::ocrOptions() const
{
    auto options = LocalMetadata::defaultOcrOptions();
    options.neural = quality->currentData().toInt() == 2;
    options.gpu = performance->currentData().toInt() == -1;
    options.cpuThreads = options.gpu ? 8 : performance->currentData().toInt();
    options.language = language->currentData().toString();
    options.vertical = options.language.startsWith("jpn_vert");
    options.segmentation = textLayout->currentData().toInt();
    options.rotation = rotation->currentData().toInt();
    options.invert = invert->isChecked();
    options.adaptiveThreshold = threshold->isChecked();
    if (!options.neural && !options.dataPath.isEmpty()) {
        const QDir ocrRoot(QFileInfo(options.executable).absolutePath());
        const auto requested = ocrRoot.filePath(quality->currentData().toBool() ? "tessdata_best" : "tessdata");
        // Do not silently label a fast-model result as a precise-model result.
        options.dataPath = requested;
    }
    return options;
}

void YACReaderArchiveInspectorDialog::recognizeRegion()
{
    const int row = pageList->currentRow();
    if (row < 0 || row >= result.pages.size() || activeWorker)
        return;
    cancel();
    cancellation = std::make_shared<std::atomic_bool>(false);
    const auto flag = cancellation;
    const auto options = ocrOptions();
    const auto region = imageLabel->selectedRegion();
    const QImage image = region.isEmpty() ? result.pages.at(row).image : result.pages.at(row).image.copy(region);
    const auto output = std::make_shared<LocalMetadata::Page>();
    setBusy(true);
    statusLabel->setText(tr("선택한 글자를 다시 읽는 중… 기존 페이지 인식 결과에 추가합니다."));
    auto *thread = createWorker(flag, [image, options, flag, output] {
        output->reading = LocalMetadata::recognizePage(image, options, flag);
        output->text = output->reading.text;
        output->error = output->reading.error; }, [this, row, output] {
        auto updated = result;
        auto &page = updated.pages[row];
        if (!output->text.isEmpty())
            page.text = (page.text + QStringLiteral("\n---\n") + output->text).trimmed();
        page.error = output->error;
        const auto oldLines = page.reading.lines;
        const bool reviewRequired = page.reading.reviewRequired || output->reading.reviewRequired;
        page.reading = output->reading;
        page.reading.lines = oldLines + output->reading.lines;
        page.reading.reviewRequired = reviewRequired;
        // A crop and a full page use different coordinate systems.
        for (auto &line : page.reading.lines)
            line.bounds = QRect();
        page.kind = LocalMetadata::classifyPage(page.text, page.number);
        updated.suggestions = LocalMetadata::suggest(updated.pages, sourcePath, libraryPath);
        showResult(updated);
        pageList->setCurrentRow(row);
        setBusy(false);
        requestSearch(true); });
    thread->start();
}

void YACReaderArchiveInspectorDialog::requestSearch(bool automatic)
{
    if (automatic && (!autoSearch->isChecked() || automaticSearchDone))
        return;
    if (automatic) {
        QStringList titles;
        for (const auto &candidate : result.suggestions) {
            if (candidate.field != LocalMetadata::Suggestion::Title || candidate.page <= 0)
                continue;
            if (!titles.contains(candidate.value))
                titles.append(candidate.value);
        }
        if (titles.size() != 1)
            return;
        bool supported = false;
        for (const auto &candidate : result.suggestions)
            supported = supported || (candidate.field == LocalMetadata::Suggestion::Title && usableEvidence(candidate, result, true, 50));
        if (!supported)
            return;
    }
    auto title = titleEdit->text().trimmed();
    auto author = authorEdit->text().trimmed();
    bool proposed = false;
    // An explicit click can take unambiguous OCR candidates to a review form.
    // It does not silently send unreviewed neural/layout candidates to a site.
    auto propose = [&](LocalMetadata::Suggestion::Field field, QString &value) {
        if (!value.isEmpty())
            return true;
        QStringList candidates;
        for (const auto &candidate : result.suggestions) {
            if (candidate.field == field && usableEvidence(candidate, result, false, 50) && !candidates.contains(candidate.value, Qt::CaseInsensitive))
                candidates.append(candidate.value);
        }
        if (candidates.size() > 1)
            return false;
        if (candidates.size() == 1) {
            value = candidates.first();
            proposed = true;
        }
        return true;
    };
    if (!automatic && (!propose(LocalMetadata::Suggestion::Title, title) || !propose(LocalMetadata::Suggestion::Author, author))) {
        statusLabel->setText(tr("서로 다른 제목 또는 작가 후보가 있습니다. 사용할 후보를 두 번 클릭하거나 직접 입력한 뒤 대조해 주세요."));
        return;
    }
    // Filename candidates are a separate, explicitly reviewed search route.
    // Never turn missing/failed OCR into confirmed evidence or an HTTP request.
    const bool pageCandidate = std::any_of(result.suggestions.cbegin(), result.suggestions.cend(), [](const LocalMetadata::Suggestion &candidate) { return candidate.page > 0; });
    if (!automatic && !pageCandidate) {
        QStringList filenameTitles;
        for (const auto &candidate : result.suggestions)
            if (candidate.field == LocalMetadata::Suggestion::Title && candidate.page == 0 && !filenameTitles.contains(candidate.value, Qt::CaseInsensitive))
                filenameTitles.append(candidate.value);
        if (title.isEmpty() && filenameTitles.size() == 1) {
            title = filenameTitles.first();
            proposed = true;
        }
        if (filenameFallback)
            proposed = true;
    }
    QStringList hints, publishers;
    for (const auto &candidate : result.suggestions) {
        if (candidate.field == LocalMetadata::Suggestion::Author && candidate.page == 0 && !hints.contains(candidate.value))
            hints.append(candidate.value);
        if (candidate.field == LocalMetadata::Suggestion::Publisher && usableEvidence(candidate, result, automatic, 50) && !publishers.contains(candidate.value)) {
            publishers.append(candidate.value);
            // Tentative publisher/circle hints also need review, even when a
            // title was already filled from independent explicit evidence.
            if (!usableEvidence(candidate, result, true, 50))
                proposed = true;
        }
    }
    if (!automatic && !publisherEdit->text().trimmed().isEmpty() && !publishers.contains(publisherEdit->text().trimmed()))
        publishers.prepend(publisherEdit->text().trimmed());
    if (title.isEmpty() && author.isEmpty() && hints.isEmpty()) {
        if (!automatic)
            statusLabel->setText(tr("대조할 제목이나 작가 후보를 선택해 주세요."));
        return;
    }
    automaticSearchDone = true;
    const auto path = libraryPath;
    const auto id = comicInfoId;
    QStringList summary;
    for (const auto &candidate : result.suggestions) {
        const auto selected = candidate.field == LocalMetadata::Suggestion::Title ? title : candidate.field == LocalMetadata::Suggestion::Author ? author
                                                                                                                                                 : QString();
        if (!selected.isEmpty() && candidate.value.compare(selected, Qt::CaseInsensitive) == 0)
            summary.append(tr("%1: %2 (%3%4)").arg(candidate.field == LocalMetadata::Suggestion::Title ? tr("제목") : tr("작가"), candidate.value, LocalMetadata::suggestionSource(candidate), candidate.labelled ? QString() : tr(" · 미확정")));
    }
    const int pages = result.pageCount;
    reject();
    emit titleSearchRequested(path, id, title, author, pages, hints, publishers, !proposed, summary.join(QLatin1Char('\n')));
}

void YACReaderArchiveInspectorDialog::save()
{
    QString error;
    std::optional<LocalMetadata::SaveGuard> guard;
    if (savedSelection)
        guard = LocalMetadata::SaveGuard { savedSelection->generation, savedSelection->comicId, savedSourceFingerprint, savedPerEnd };
    if (!LocalMetadata::save(libraryPath, comicInfoId, sourcePath, titleEdit->text(), authorEdit->text(), overwrite->isChecked(), result.suggestions, &error, guard ? &*guard : nullptr)) {
        statusLabel->setText(error);
        return;
    }
    const auto path = libraryPath;
    const auto id = comicInfoId;
    accept();
    emit metadataSaved(path, id);
}
