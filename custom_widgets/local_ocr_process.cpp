#include "local_ocr_process.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSet>
#include <QThread>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
// Windows mutexes are recursive. An earlier invocation must explicitly finish
// before another one on the same thread can reuse its named Job, even if its
// root process has already exited naturally.
thread_local QSet<QString> ownedResources;
#endif
}

struct LocalOcrProcess::Native {
#ifdef Q_OS_WIN
    HANDLE job = nullptr;
    HANDLE resourceMutex = nullptr;
    bool resourceOwned = false;
    QString resourceKey;
    void releaseResource()
    {
        if (resourceOwned) {
            ReleaseMutex(resourceMutex);
            ownedResources.remove(resourceKey);
            resourceOwned = false;
        }
        if (resourceMutex) {
            CloseHandle(resourceMutex);
            resourceMutex = nullptr;
        }
    }
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
        releaseResource();
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

bool LocalOcrProcess::startOcr(const QString &program, const QStringList &arguments, QString *error,
                               const QString &resource, const std::shared_ptr<std::atomic_bool> &cancel)
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
    bool abandoned = false;
    QString jobName;
    if (!resource.isEmpty()) {
        bool valid = resource.size() <= 64;
        for (const QChar c : resource)
            valid = valid && ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'-');
        if (!valid) {
            *error = tr("Invalid OCR resource key.");
            return false;
        }
        if (ownedResources.contains(resource)) {
            *error = tr("This thread must finish its previous OCR resource before reuse.");
            return false;
        }
        native->resourceKey = resource;
        const auto name = QStringLiteral("Local\\YACReader.OCR.v1.") + resource;
        const auto mutexName = name + QStringLiteral(".mutex");
        jobName = name + QStringLiteral(".job");
        native->resourceMutex = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(mutexName.utf16()));
        if (!native->resourceMutex)
            return fail();
        QElapsedTimer wait;
        wait.start();
        while (!native->resourceOwned) {
            if (cancel && cancel->load()) {
                *error = tr("Cancelled while waiting for OCR resource ownership.");
                return false;
            }
            const DWORD state = WaitForSingleObject(native->resourceMutex, 50);
            if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) {
                native->resourceOwned = true;
                ownedResources.insert(resource);
                abandoned = state == WAIT_ABANDONED;
            } else if (state == WAIT_FAILED) {
                return fail();
            } else if (wait.elapsed() >= 5000) {
                *error = tr("Another OCR worker still owns this resource.");
                return false;
            }
        }
    }
    // Keep the new handle separate until an old named job has no active
    // processes. A failed contender must never terminate the current owner.
    HANDLE createdJob = CreateJobObjectW(nullptr, jobName.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(jobName.utf16()));
    if (!createdJob)
        return fail();
    auto closeUnownedJob = qScopeGuard([&] { if (createdJob) CloseHandle(createdJob); });
    if (!resource.isEmpty()) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = { };
        auto query = [&] { return QueryInformationJobObject(createdJob, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr); };
        if (!query())
            return fail();
        // Abandonment transfers the mutex, not proof of worker termination.
        // Only this named OCR job is eligible for abandoned-owner cleanup.
        if (abandoned && info.ActiveProcesses && !TerminateJobObject(createdJob, 1))
            return fail();
        QElapsedTimer cleanup;
        cleanup.start();
        while (info.ActiveProcesses) {
            if (cancel && cancel->load()) {
                *error = tr("Cancelled while verifying previous OCR resource cleanup.");
                return false;
            }
            if (cleanup.elapsed() >= 5000) {
                *error = tr("Previous OCR resource processes have not exited.");
                return false;
            }
            QThread::msleep(20);
            if (!query())
                return fail();
        }
    }
    native->job = createdJob;
    createdJob = nullptr;
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
#else
    if (!resource.isEmpty()) {
        *error = tr("Shared OCR resource ownership requires Windows.");
        return false;
    }
#endif
    if (cancel && cancel->load()) {
        *error = tr("Cancelled before starting OCR.");
        return false;
    }
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
    if (!native->job) {
        native->releaseResource();
        return true;
    }
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
    // Drop the job handle before handing off the mutex. Later calls/destruction
    // must not query or terminate a new worker that reused the same named job.
    CloseHandle(native->job);
    native->job = nullptr;
    native->releaseResource();
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
