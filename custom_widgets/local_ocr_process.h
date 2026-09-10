#ifndef LOCAL_OCR_PROCESS_H
#define LOCAL_OCR_PROCESS_H

#include <QProcess>

#include <atomic>
#include <memory>

// One invocation, owned by the calling OCR thread. Windows descendants are
// assigned to a non-inherited kill-on-close job at process creation.
class LocalOcrProcess : public QProcess
{
public:
    LocalOcrProcess();
    ~LocalOcrProcess() override;
    // An optional Windows resource key serializes cooperating OCR instances.
    // Production uses nvidia-0; tests use a unique private synthetic key. Waits
    // are bounded/cancellable. Ownership is released only after tree cleanup.
    bool startOcr(const QString &program, const QStringList &arguments, QString *error,
                  const QString &resource = { }, const std::shared_ptr<std::atomic_bool> &cancel = { });
    // Confirm tree termination before cleanup, returning results or CPU retry.
    bool finishTree(QString *error);

private:
    struct Native;
    std::unique_ptr<Native> native;
    bool launched = false;
};
#endif
