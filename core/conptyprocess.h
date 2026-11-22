#ifndef CONPTYPROCESS_H
#define CONPTYPROCESS_H

#include "iptyprocess.h"

#ifdef Q_OS_WIN

#include <windows.h>
#include <process.h>

#include <QMutex>
#include <QThread>

#include <atomic>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
typedef VOID *HPCON;
#define TOO_OLD_WINSDK
#else
#define KPTY_HAVE_CONPTY_SDK
#endif

class WindowsContext
{
public:
    using CreatePseudoConsolePtr = HRESULT (WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
    using ResizePseudoConsolePtr = HRESULT (WINAPI *)(HPCON, COORD);
    using ClosePseudoConsolePtr = VOID (WINAPI *)(HPCON);

    WindowsContext() = default;
    ~WindowsContext();

    bool init();
    QString lastError() const;

    CreatePseudoConsolePtr createPseudoConsole = nullptr;
    ResizePseudoConsolePtr resizePseudoConsole = nullptr;
    ClosePseudoConsolePtr closePseudoConsole = nullptr;

private:
    HMODULE m_kernel32 = nullptr;
    QString m_lastError;
};

class PtyBuffer : public QIODevice
{
    friend class ConPtyProcess;
    Q_OBJECT

public:
    PtyBuffer();
    ~PtyBuffer() override;

    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *data, qint64 len) override;
    bool isSequential() const override;
    qint64 bytesAvailable() const override;

    void append(const QByteArray &data);

private:
    QByteArray m_readBuffer;
    mutable QMutex m_mutex;
};

class ConPtyProcess : public IPtyProcess
{
public:
    ConPtyProcess();
    ~ConPtyProcess() override;

    bool startProcess(const QString &shellPath, QStringList environment, qint16 cols, qint16 rows) override;
    bool startProcess(const QString &program, const QStringList &arguments, const QString &workingDirectory,
                      QStringList environment, qint16 cols, qint16 rows);
    bool resize(qint16 cols, qint16 rows) override;
    bool kill() override;
    PtyType type() override;
    QString dumpDebugInfo() override;
    QIODevice *notifier() override;
    QByteArray readAll() override;
    qint64 write(const QByteArray &byteArray) override;
    bool isAvailable() override;
    void moveToThread(QThread *targetThread) override;

    bool isRunning() const override;
    int exitCode() const;

private:
    HRESULT createPseudoConsoleAndPipes(HPCON *pty, HANDLE *input, HANDLE *output, qint16 cols, qint16 rows);
    HRESULT initializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEX *startupInfo, HPCON pty);
    void closeHandles();
    static QString resolveExecutable(const QString &program);

    WindowsContext m_winContext;
    PtyBuffer m_buffer;
    HPCON m_ptyHandler = nullptr;
    HANDLE m_hPipeIn = INVALID_HANDLE_VALUE;
    HANDLE m_hPipeOut = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION m_processInfo{};
    STARTUPINFOEX m_startupInfo{};
    QThread *m_readThread = nullptr;
    QThread *m_processThread = nullptr;
    mutable QMutex m_lifecycleMutex;
    std::atomic_bool m_running{false};
    std::atomic_int m_exitCode{-1};
};

#endif // Q_OS_WIN

#endif // CONPTYPROCESS_H
