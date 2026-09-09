#ifndef LOCAL_OCR_PROCESS_H
#define LOCAL_OCR_PROCESS_H

#include <QProcess>

#include <memory>

// One invocation, owned by the calling OCR thread. Windows descendants are
// assigned to a non-inherited kill-on-close job at process creation.
class LocalOcrProcess : public QProcess
{
public:
    LocalOcrProcess();
    ~LocalOcrProcess() override;
    bool startOcr(const QString &program, const QStringList &arguments, QString *error);
    // Confirm tree termination before cleanup, returning results or CPU retry.
    bool finishTree(QString *error);

private:
    struct Native;
    std::unique_ptr<Native> native;
    bool launched = false;
};
#endif
