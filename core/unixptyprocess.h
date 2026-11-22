#ifndef UNIXPTYPROCESS_H
#define UNIXPTYPROCESS_H

#include "iptyprocess.h"
#include <QProcess>
#include <QSocketNotifier>


// support for build with MUSL on Alpine Linux
#ifndef _PATH_UTMPX
#include <sys/time.h>
# define _PATH_UTMPX	"/var/log/utmp"
#endif

class ShellProcess : public QProcess
{
    friend class UnixPtyProcess;
    Q_OBJECT
public:
    ShellProcess( )
        : QProcess( )
        , m_handleMaster(-1)
        , m_handleSlave(-1)
    {
        setProcessChannelMode(QProcess::SeparateChannels);
        setChildProcessModifier([this]() { setupPtyChildProcess(); });
    }

    void emitReadyRead()
    {
        emit readyRead();
    }

protected:
    void setupPtyChildProcess();

private:
    int m_handleMaster, m_handleSlave;
    QString m_handleSlaveName;
};

class UnixPtyProcess : public IPtyProcess
{
public:
    UnixPtyProcess();
    ~UnixPtyProcess() override;

    bool startProcess(const QString &shellPath, QStringList environment, qint16 cols, qint16 rows) override;
    bool resize(qint16 cols, qint16 rows) override;
    bool kill() override;
    PtyType type() override;
    QString dumpDebugInfo() override;
    QIODevice *notifier() override;
    QByteArray readAll() override;
    qint64 write(const QByteArray &byteArray) override;
    bool isAvailable() override;
    bool isRunning() const override;
    void moveToThread(QThread *targetThread) override;

private:
    ShellProcess m_shellProcess;
    QSocketNotifier *m_readMasterNotify = nullptr;
    QByteArray m_shellReadBuffer;

};

#endif // UNIXPTYPROCESS_H
