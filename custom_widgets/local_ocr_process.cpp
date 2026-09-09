#include "local_ocr_process.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

struct LocalOcrProcess::Native {
#ifdef Q_OS_WIN
    HANDLE job = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    bool attributesInitialized = false;
    STARTUPINFOEXW startup = { };
    ~Native()
    {
        if (attributesInitialized)
            DeleteProcThreadAttributeList(attributes);
        if (attributes)
            HeapFree(GetProcessHeap(), 0, attributes);
        if (job)
            CloseHandle(job);
    }
#endif
};

namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("LocalMetadata", text);
}
}

LocalOcrProcess::LocalOcrProcess() : native(std::make_unique<Native>())
{
}

LocalOcrProcess::~LocalOcrProcess()
{
    QString ignored;
    finishTree(&ignored);
#ifdef Q_OS_WIN
    setCreateProcessArgumentsModifier({ });
#endif
}

bool LocalOcrProcess::startOcr(const QString &program, const QStringList &arguments, QString *error)
{
    if (launched) {
        *error = tr("This OCR process has already been used.");
        return false;
    }
    launched = true;
#ifdef Q_OS_WIN
    auto fail = [&] {
        *error = tr("Could not isolate the OCR process (Windows error %1).").arg(GetLastError());
        return false;
    };
    native->job = CreateJobObjectW(nullptr, nullptr);
    if (!native->job)
        return fail();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { };
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(native->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return fail();
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size)
        return fail();
    native->attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
    if (!native->attributes) {
        *error = tr("Could not allocate OCR process attributes.");
        return false;
    }
    if (!InitializeProcThreadAttributeList(native->attributes, 1, 0, &size))
        return fail();
    native->attributesInitialized = true;
    // Windows 10+ assigns the job inside CreateProcess, before any child code.
    // https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute
    if (!UpdateProcThreadAttribute(native->attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                   &native->job, sizeof(native->job), nullptr, nullptr))
        return fail();
    setCreateProcessArgumentsModifier([this](CreateProcessArguments *args) {
        native->startup.StartupInfo = *args->startupInfo;
        native->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        native->startup.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        native->startup.StartupInfo.wShowWindow = SW_HIDE;
        native->startup.lpAttributeList = native->attributes;
        args->startupInfo = &native->startup.StartupInfo;
        args->flags |= EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
    });
#endif
    start(program, arguments, QIODevice::ReadOnly);
    if (!waitForStarted(5000)) {
        *error = tr("Could not start local OCR: %1").arg(errorString());
        return false;
    }
    return true;
}

bool LocalOcrProcess::finishTree(QString *error)
{
#ifdef Q_OS_WIN
    if (!native->job)
        return true;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = { };
    auto query = [&] { return QueryInformationJobObject(native->job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr); };
    if (!query() || (info.ActiveProcesses && !TerminateJobObject(native->job, 1))) {
        *error = tr("Could not terminate the OCR process tree (Windows error %1).").arg(GetLastError());
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while (true) {
        if (!query()) {
            *error = tr("Could not verify OCR process cleanup (Windows error %1).").arg(GetLastError());
            return false;
        }
        if (!info.ActiveProcesses)
            break;
        if (timer.elapsed() >= 5000) {
            *error = tr("OCR process cleanup timed out.");
            return false;
        }
        if (state() != NotRunning)
            waitForFinished(20);
        else
            QThread::msleep(20);
    }
    if (state() != NotRunning && !waitForFinished(1000)) {
        *error = tr("Could not finish the local OCR process.");
        return false;
    }
    return true;
#else
    if (state() == NotRunning)
        return true;
    kill();
    if (waitForFinished(5000))
        return true;
    *error = tr("Could not finish the local OCR process.");
    return false;
#endif
}
