#include "library_maintenance_lock.h"

#include "yacreader_global.h"

#include <QDir>
#include <QLockFile>
#include <QSysInfo>
using namespace YACReader;
LibraryMaintenanceLock::LibraryMaintenanceLock(const QString &libraryPath)
    : maintenanceLock(std::make_unique<QLockFile>(QDir(LibraryPaths::libraryDataPath(libraryPath)).filePath("maintenance.lock"))), legacyRepairLock(std::make_unique<QLockFile>(QDir(LibraryPaths::libraryDataPath(libraryPath)).filePath("repair.lock")))
{
    maintenanceLock->setStaleLockTime(0);
    legacyRepairLock->setStaleLockTime(0);
}

LibraryMaintenanceLock::~LibraryMaintenanceLock() = default;

bool LibraryMaintenanceLock::tryLockFile(QLockFile &lock, bool removeStaleLock)
{
    if (lock.tryLock())
        return true;
    if (removeStaleLock && lock.removeStaleLockFile() && lock.tryLock())
        return true;
    captureLockInfo(lock);
    failedLockPath = lock.fileName();
    return false;
}

bool LibraryMaintenanceLock::tryLock(bool removeStaleLock)
{
    failedLockPath.clear();
    currentHolderInfo.clear();
    currentHolderIsRunningLocally = false;
    if (!tryLockFile(*maintenanceLock, removeStaleLock))
        return false;
    if (!tryLockFile(*legacyRepairLock, removeStaleLock)) {
        maintenanceLock->unlock();
        return false;
    }
    return true;
}

void LibraryMaintenanceLock::captureLockInfo(QLockFile &lock)
{
    qint64 pid = 0;
    QString hostname;
    QString appname;
    if (lock.getLockInfo(&pid, &hostname, &appname)) {
        currentHolderInfo = QString("%1 (PID %2) on %3").arg(appname).arg(pid).arg(hostname.isEmpty() ? QString("unknown host") : hostname);
        currentHolderIsRunningLocally = !hostname.isEmpty() && hostname == QSysInfo::machineHostName();
    }
}

QString LibraryMaintenanceLock::errorString() const
{
    return QString("Another maintenance operation is using this library (%1)%2")
            .arg(failedLockPath, currentHolderInfo.isEmpty() ? QString() : QString(": %1").arg(currentHolderInfo));
}

QString LibraryMaintenanceLock::holderInfo() const
{
    return currentHolderInfo;
}

bool LibraryMaintenanceLock::holderIsRunningLocally() const
{
    return currentHolderIsRunningLocally;
}
