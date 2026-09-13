#ifndef LIBRARY_MAINTENANCE_LOCK_H
#define LIBRARY_MAINTENANCE_LOCK_H
#include <QString>

#include <memory>
class QLockFile;
class LibraryMaintenanceLock
{
public:
    explicit LibraryMaintenanceLock(const QString &libraryPath);
    ~LibraryMaintenanceLock();

    bool tryLock(bool removeStaleLock = false);
    QString errorString() const;
    QString holderInfo() const;
    bool holderIsRunningLocally() const;

private:
    bool tryLockFile(QLockFile &lock, bool removeStaleLock);
    void captureLockInfo(QLockFile &lock);

    std::unique_ptr<QLockFile> maintenanceLock;
    std::unique_ptr<QLockFile> legacyRepairLock;
    QString failedLockPath;
    QString currentHolderInfo;
    bool currentHolderIsRunningLocally { false };
};

#endif
