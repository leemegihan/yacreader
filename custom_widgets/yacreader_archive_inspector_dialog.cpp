#include "yacreader_archive_inspector_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
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
#include <QVBoxLayout>

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
    pageLimit->setRange(1, 6);
    pageLimit->setValue(3);
    pageLimit->setSuffix(tr("장씩 (앞 / 뒤)"));
    language = new QComboBox(this);
    language->addItem(tr("일본어 + 한국어 + 영어"), "jpn+kor+eng");
    language->addItem(tr("일본어 세로쓰기"), "jpn_vert+eng");
    language->addItem(tr("한국어 + 영어"), "kor+eng");
    language->addItem(tr("영어"), "eng");
    ocrButton = new QPushButton(tr("페이지 글자 읽기"), this);
    cancelButton = new QPushButton(tr("중지"), this);
    auto *controls = new QHBoxLayout;
    controls->addWidget(pageLimit);
    controls->addWidget(language);
    controls->addWidget(ocrButton);
    controls->addWidget(cancelButton);

    pageList = new QListWidget(this);
    pageList->setMaximumWidth(170);
    imageLabel = new QLabel(this);
    imageLabel->setAlignment(Qt::AlignCenter);
    auto *imageScroll = new QScrollArea(this);
    imageScroll->setWidgetResizable(true);
    imageScroll->setWidget(imageLabel);
    pageText = new QPlainTextEdit(this);
    pageText->setReadOnly(true);
    pageText->setPlaceholderText(tr("OCR 결과는 여기에 표시됩니다. 필요한 글자를 선택해 제목·작가 칸에 복사할 수 있습니다."));
    candidateList = new QListWidget(this);
    candidateList->setWordWrap(true);
    candidateList->setMaximumHeight(160);
    auto *textPanel = new QWidget(this);
    auto *textLayout = new QVBoxLayout(textPanel);
    textLayout->addWidget(new QLabel(tr("선택한 페이지의 글자 (오인식 가능)"), this));
    textLayout->addWidget(pageText, 1);
    textLayout->addWidget(new QLabel(tr("후보를 두 번 클릭하면 아래 입력칸에 복사됩니다."), this));
    textLayout->addWidget(candidateList);
    auto *splitter = new QSplitter(this);
    splitter->addWidget(pageList);
    splitter->addWidget(imageScroll);
    splitter->addWidget(textPanel);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 2);
    titleEdit = new QLineEdit(this);
    authorEdit = new QLineEdit(this);
    auto *form = new QFormLayout;
    form->addRow(tr("확인한 제목"), titleEdit);
    form->addRow(tr("확인한 작가"), authorEdit);
    overwrite = new QCheckBox(tr("기존 제목·작가를 입력한 값으로 덮어쓰기 (기본: 빈 항목만 채움)"), this);
    saveButton = new QPushButton(tr("확인한 정보 저장"), this);
    searchButton = new QPushButton(tr("제목으로 AniList 검색…"), this);
    searchButton->setToolTip(tr("누르면 입력한 제목을 AniList에 전송합니다. 페이지 이미지는 전송하지 않습니다."));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(saveButton, QDialogButtonBox::ActionRole);
    buttons->addButton(searchButton, QDialogButtonBox::ActionRole);
    auto *privacy = new QLabel(tr("이미지는 PC 안에서만 처리합니다. 후보는 추정이며 자동 저장하지 않습니다. 원본과 읽던 위치는 변경하지 않습니다."), this);
    privacy->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(fileLabel);
    layout->addWidget(privacy);
    layout->addLayout(controls);
    layout->addWidget(statusLabel);
    layout->addWidget(splitter, 1);
    layout->addLayout(form);
    layout->addWidget(overwrite);
    layout->addWidget(buttons);

    connect(ocrButton, &QPushButton::clicked, this, [this] { start(true); });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        cancel();
        setBusy(false);
        statusLabel->setText(tr("중지했습니다. 다시 실행할 수 있습니다."));
    });
    connect(pageList, &QListWidget::currentRowChanged, this, &YACReaderArchiveInspectorDialog::showPage);
    connect(candidateList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const int row = candidateList->row(item);
        if (row < 0 || row >= result.suggestions.size())
            return;
        const auto &suggestion = result.suggestions.at(row);
        (suggestion.field == LocalMetadata::Suggestion::Title ? titleEdit : authorEdit)->setText(suggestion.value);
        for (int i = 0; i < result.pages.size(); ++i) {
            if (result.pages.at(i).number == suggestion.page)
                pageList->setCurrentRow(i);
        }
    });
    connect(saveButton, &QPushButton::clicked, this, &YACReaderArchiveInspectorDialog::save);
    connect(searchButton, &QPushButton::clicked, this, [this] {
        if (titleEdit->text().trimmed().isEmpty()) {
            statusLabel->setText(tr("검색할 제목을 먼저 입력해 주세요."));
            return;
        }
        const auto path = libraryPath;
        const auto id = comicInfoId;
        const auto title = titleEdit->text().trimmed();
        reject();
        emit titleSearchRequested(path, id, title);
    });
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
    if (cancellation)
        cancellation->store(true);
}

void YACReaderArchiveInspectorDialog::setBusy(bool busy)
{
    ocrButton->setEnabled(!busy && !sourcePath.isEmpty());
    cancelButton->setEnabled(busy);
    pageLimit->setEnabled(!busy);
    language->setEnabled(!busy);
    saveButton->setEnabled(!busy && !sourcePath.isEmpty());
    searchButton->setEnabled(!busy && !sourcePath.isEmpty());
}

void YACReaderArchiveInspectorDialog::inspectComic(const QString &path, qulonglong id)
{
    cancel();
    libraryPath = path;
    comicInfoId = id;
    titleEdit->clear();
    authorEdit->clear();
    overwrite->setChecked(false);
    result = { };
    pageList->clear();
    candidateList->clear();
    imageLabel->clear();
    pageText->clear();
    QString error;
    sourcePath = LocalMetadata::comicPath(path, id, &error);
    fileLabel->setText(sourcePath);
    setBusy(false);
    if (sourcePath.isEmpty()) {
        statusLabel->setText(error);
        return;
    }
    start(false);
}

void YACReaderArchiveInspectorDialog::start(bool ocr)
{
    if (sourcePath.isEmpty())
        return;
    cancel();
    cancellation = std::make_shared<std::atomic_bool>(false);
    const auto flag = cancellation;
    const auto source = sourcePath;
    const int count = pageLimit->value();
    auto options = LocalMetadata::defaultOcrOptions();
    options.language = language->currentData().toString();
    options.vertical = language->currentIndex() == 1;
    const auto output = std::make_shared<LocalMetadata::Result>();
    setBusy(true);
    statusLabel->setText(ocr ? tr("PC에서 앞·뒤 페이지를 읽는 중… 페이지당 시간이 걸릴 수 있습니다. 중지 버튼으로 취소할 수 있습니다.") : tr("앞·뒤 페이지를 불러오는 중…"));
    // Worker owns copied values only; cancelled/stale results never touch the UI.
    auto *thread = QThread::create([source, count, options, flag, output, ocr] {
        *output = ocr ? LocalMetadata::analyze(source, count, options, flag) : LocalMetadata::readPages(source, count, flag);
        if (!ocr)
            output->suggestions = LocalMetadata::suggest(output->pages, source);
    });
    connect(thread, &QThread::finished, this, [this, flag, output] {
        if (flag->load() || flag != cancellation)
            return;
        setBusy(false);
        showResult(*output);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void YACReaderArchiveInspectorDialog::showResult(const LocalMetadata::Result &value)
{
    result = value;
    pageList->clear();
    candidateList->clear();
    int failures = 0;
    for (const auto &page : result.pages) {
        pageList->addItem(tr("%1 / %2 페이지%3").arg(page.number).arg(result.pageCount).arg(page.error.isEmpty() ? QString() : tr(" (오류)")));
        if (!page.error.isEmpty())
            ++failures;
    }
    for (const auto &candidate : result.suggestions) {
        const auto field = candidate.field == LocalMetadata::Suggestion::Title ? tr("제목") : tr("작가");
        const auto source = candidate.page > 0 ? tr("%1페이지").arg(candidate.page) : tr("이름 힌트 · 낮은 신뢰");
        auto *item = new QListWidgetItem(QStringLiteral("[%1 · %2] %3").arg(field, source, candidate.value), candidateList);
        item->setToolTip(candidate.reason);
    }
    statusLabel->setText(!result.error.isEmpty() ? result.error : tr("%1장 확인, %2장 오류. 후보가 없거나 부정확하면 페이지 글자를 확인해 직접 입력해 주세요.").arg(result.pages.size()).arg(failures));
    if (!result.pages.isEmpty())
        pageList->setCurrentRow(0);
}

void YACReaderArchiveInspectorDialog::showPage(int row)
{
    imageLabel->clear();
    pageText->clear();
    if (row < 0 || row >= result.pages.size())
        return;
    const auto &page = result.pages.at(row);
    imageLabel->setPixmap(QPixmap::fromImage(page.image).scaled(650, 950, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pageText->setPlainText(page.error.isEmpty() ? page.text : page.error + QStringLiteral("\n\n") + page.text);
}

void YACReaderArchiveInspectorDialog::save()
{
    QString error;
    if (!LocalMetadata::save(libraryPath, comicInfoId, sourcePath, titleEdit->text(), authorEdit->text(), overwrite->isChecked(), result.suggestions, &error)) {
        statusLabel->setText(error);
        return;
    }
    const auto path = libraryPath;
    const auto id = comicInfoId;
    accept();
    emit metadataSaved(path, id);
}
