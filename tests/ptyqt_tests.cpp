#include <QTest>
#include "ptyqt.h"
#ifdef Q_OS_WIN
#include "conptyprocess.h"
#endif
#include <QProcessEnvironment>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QThread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <string>
#include <QTimer>
#include <QDateTime>
#include <cstdio>

#ifdef Q_OS_WIN
#ifndef _WINDEF_
typedef unsigned long DWORD;
#endif
#endif

#define WINPTY_DBG_SERVER_NAME "winpty-debugserver.exe"
#define WINPTY_AGENT_NAME "winpty-agent.exe"

//increase it for visual control each shell
#define DEBUG_SLEEP_SEC 1

// Qt's default Windows message handler may write to the console/debugger
// instead of the redirected standard-error stream captured by CTest.
// Route test diagnostics explicitly to stderr so CI output is deterministic.
void testMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const char *typeName = "unknown";
    switch (type) {
    case QtDebugMsg: typeName = "debug"; break;
    case QtInfoMsg: typeName = "info"; break;
    case QtWarningMsg: typeName = "warning"; break;
    case QtCriticalMsg: typeName = "critical"; break;
    case QtFatalMsg: typeName = "fatal"; break;
    }

    const QByteArray utf8Message = message.toUtf8();
    std::fprintf(stderr, "[Qt %s] ", typeName);
    std::fwrite(utf8Message.constData(), 1, static_cast<size_t>(utf8Message.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void sleepByEventLoop(int seconds)
{
    QEventLoop sleepLoop;
    QTimer sleepTimer;
    QObject::connect(&sleepTimer, &QTimer::timeout, &sleepLoop, &QEventLoop::quit);
    sleepTimer.setInterval(seconds * 1000);
    sleepTimer.setSingleShot(true);
    sleepTimer.start();
    sleepLoop.exec();
}

#ifdef Q_OS_WIN
DWORD findProcessId(const QString &processName, int parentProcessId = 0)
{
    PROCESSENTRY32W processInfo{};
    processInfo.dwSize = sizeof(processInfo);

    HANDLE processesSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processesSnapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    if (Process32FirstW(processesSnapshot, &processInfo))
    {
        do
        {
            const QString executableName = QString::fromWCharArray(processInfo.szExeFile);
            if (processName.compare(executableName, Qt::CaseInsensitive) == 0
                && (parentProcessId == 0 || parentProcessId == static_cast<int>(processInfo.th32ParentProcessID)))
            {
                const DWORD processId = processInfo.th32ProcessID;
                CloseHandle(processesSnapshot);
                return processId;
            }
        }
        while (Process32NextW(processesSnapshot, &processInfo));
    }

    CloseHandle(processesSnapshot);
    return 0;
}

void killProcessByName(QString processName)
{
    if (findProcessId(processName))
    {
        system(QString("taskkill /im %1 /f").arg(processName).toStdString().c_str());
        sleepByEventLoop(1);
    }
}

QStringList getShells()
{
    QString systemRoot = QProcessEnvironment::systemEnvironment().value("windir");
    if (systemRoot.trimmed().isEmpty())
        systemRoot = QProcessEnvironment::systemEnvironment().value("WINDIR");
    if (systemRoot.trimmed().isEmpty())
        systemRoot = "c:\\Windows";

    QStringList possibleShells;
    possibleShells << (systemRoot + "\\system32\\cmd.exe"); //reversed slashes
    //possibleShells << ("C:/Windows/system32/cmd.exe"); //normal slashes
    //possibleShells << (systemRoot + "\\system32\\WindowsPowerShell\\v1.0\\powershell.exe");
    //possibleShells << "C:\\Program\ Files\\Git\\bin\\bash.exe";
    //possibleShells << "C:\\Python27\\python.exe";
    //possibleShells << "C:/Python27/pythonw.exe";

    QStringList shells;
    foreach (QString possibleShell, possibleShells)
    {
        if (QFile::exists(possibleShell))
            shells << possibleShell;
    }

    return shells;
}
#endif

class PtyQtTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qInstallMessageHandler(testMessageHandler);
        qInfo().noquote() << "[ptyqt] Test suite starting on"
                          << QSysInfo::productType() << QSysInfo::productVersion()
                          << "(Qt" << QT_VERSION_STR << ")";
    }

    void cleanupTestCase()
    {
        qInfo() << "[ptyqt] Test suite finished";
    }

    //unix unit tests
#ifdef Q_OS_UNIX
    void unixpty()
    {
        const QString shellPath = QStringLiteral("/bin/bash");
        qInfo().noquote() << "[UnixPTY] Starting shell:" << shellPath;

        QScopedPointer<IPtyProcess> unixPty(PtyQt::createPtyProcess(IPtyProcess::UnixPty));
        QCOMPARE(unixPty->type(), IPtyProcess::UnixPty);
        QVERIFY2(unixPty->isAvailable(), "The Unix PTY backend is unavailable");

        qInfo() << "[UnixPTY] Starting process at 200x80";
        const bool startResult = unixPty->startProcess(
            shellPath, QProcessEnvironment::systemEnvironment().toStringList(), 200, 80);
#ifdef PTYQT_DEBUG
        if (!startResult)
            qDebug() << unixPty->lastError() << unixPty->dumpDebugInfo();
#endif
        QVERIFY2(startResult, qPrintable(unixPty->lastError()));
        QVERIFY(unixPty->pid() != 0);
        qInfo() << "[UnixPTY] Started child process with PID" << unixPty->pid();

        //check shell welcome
        QEventLoop el;
        auto connection = QObject::connect(unixPty->notifier(), &QIODevice::readyRead, [&unixPty, &el]() {
            sleepByEventLoop(1);
            const QByteArray startupOutput = unixPty->readAll();
            qInfo().noquote() << "[UnixPTY] Startup output:" << QString::fromUtf8(startupOutput).trimmed();
            el.quit();
        });
        el.exec();
        unixPty->notifier()->disconnect(connection);

        //check shell read after write
        bool testRes = false;
        connection = QObject::connect(unixPty->notifier(), &QIODevice::readyRead, [&unixPty, &el, &testRes]() {
            sleepByEventLoop(1);
            QString res = QString::fromUtf8(unixPty->readAll());
            //qDebug() << res;

            //for e.g. bash return empty strings after needed data
            if (res.isEmpty() && testRes)
                return;

            testRes = res.contains("ptyqt_tests");
            testRes = testRes && res.contains("Makefile");
            testRes = testRes && res.contains("cmake_install.cmake");
            el.quit();
        });
        qInfo() << "[UnixPTY] Sending directory-listing command";
        qDebug() << "ptyin:" << unixPty->write("ls\n");
        el.exec();
        QVERIFY(testRes);
        qDebug() << "ptyin:" << unixPty->write("ls -alh\n");
        el.exec();
        QVERIFY(testRes);
        unixPty->notifier()->disconnect(connection);

        qInfo() << "[UnixPTY] Resizing terminal to 240x90";
        QVERIFY(unixPty->resize(240, 90));
        qInfo() << "[UnixPTY] Smoke test completed";
    }
#endif

    // Windows unit tests
#ifdef Q_OS_WIN
    void conpty()
    {
        const int buildNumber = QSysInfo::kernelVersion().section(QLatin1Char('.'), -1).toInt();
        qInfo() << "[ConPTY] Windows build:" << buildNumber;
        if (buildNumber < CONPTY_MINIMAL_WINDOWS_VERSION) {
            QSKIP("ConPTY requires Windows 10 build 18309 or newer");
        }

        const QStringList shells = getShells();
        if (shells.isEmpty()) {
            QSKIP("No supported Windows command interpreter was found");
        }

        for (const QString &shellPath : shells) {
            qInfo().noquote() << "[ConPTY] Starting interactive shell:" << shellPath;

            QScopedPointer<IPtyProcess> conPty(PtyQt::createPtyProcess(IPtyProcess::ConPty));
            QVERIFY(conPty);
            QCOMPARE(conPty->type(), IPtyProcess::ConPty);
            QVERIFY2(conPty->isAvailable(), "The Windows ConPTY API is unavailable");

            QEventLoop startupLoop;
            QTimer startupTimeout;
            startupTimeout.setSingleShot(true);
            connect(&startupTimeout, &QTimer::timeout, &startupLoop, &QEventLoop::quit);

            QByteArray startupOutput;
            const auto startupConnection = connect(
                conPty->notifier(), &QIODevice::readyRead, &startupLoop, [&]() {
                    startupOutput += conPty->readAll();
                    if (!startupOutput.isEmpty())
                        startupLoop.quit();
                });

            qInfo() << "[ConPTY] Launching shell with initial size 100x30";
            QVERIFY2(conPty->startProcess(
                         shellPath, QProcessEnvironment::systemEnvironment().toStringList(), 100, 30),
                     qPrintable(conPty->lastError()));
            QVERIFY(conPty->pid() != 0);
            qInfo() << "[ConPTY] Shell started with PID" << conPty->pid();

            startupOutput += conPty->readAll();
            if (startupOutput.isEmpty()) {
                startupTimeout.start(5000);
                startupLoop.exec();
                startupTimeout.stop();
            }
            disconnect(startupConnection);

            if (startupOutput.isEmpty()) {
                qWarning() << "[ConPTY] No initial shell output received within 5 seconds";
            } else {
                qInfo().noquote() << "[ConPTY] Initial shell output:"
                                  << QString::fromLocal8Bit(startupOutput).trimmed();
            }
            QVERIFY2(!startupOutput.isEmpty(),
                     "ConPTY shell did not produce startup output within 5 seconds");
            QVERIFY2(conPty->isRunning(),
                     "Interactive CMD exited before the test could send input");

            QEventLoop commandLoop;
            QTimer commandTimeout;
            commandTimeout.setSingleShot(true);
            connect(&commandTimeout, &QTimer::timeout, &commandLoop, &QEventLoop::quit);

            QByteArray commandOutput;
            const QByteArray expectedExecutable("ptyqt_tests.exe");
            const QByteArray expectedLibrary("ptyqt.dll");
            const auto commandConnection = connect(
                conPty->notifier(), &QIODevice::readyRead, &commandLoop, [&]() {
                    commandOutput += conPty->readAll();
                    if (commandOutput.contains(expectedExecutable)
                        && commandOutput.contains(expectedLibrary)) {
                        commandLoop.quit();
                    }
                });

            qInfo() << "[ConPTY] Sending directory-listing command";
            const QByteArray command("dir\r\n");
            QCOMPARE(conPty->write(command), static_cast<qint64>(command.size()));

            commandOutput += conPty->readAll();
            if (!commandOutput.contains(expectedExecutable)
                || !commandOutput.contains(expectedLibrary)) {
                commandTimeout.start(10000);
                commandLoop.exec();
                commandTimeout.stop();
            }
            disconnect(commandConnection);

            qInfo().noquote() << "[ConPTY] Directory-listing output:"
                              << QString::fromLocal8Bit(commandOutput).trimmed();
            QVERIFY2(commandOutput.contains(expectedExecutable),
                     "ConPTY output did not contain ptyqt_tests.exe");
            QVERIFY2(commandOutput.contains(expectedLibrary),
                     "ConPTY output did not contain ptyqt.dll");

            qInfo() << "[ConPTY] Resizing terminal to 120x40";
            QVERIFY2(conPty->resize(120, 40), "ConPTY resize to 120x40 failed");
            QCOMPARE(conPty->size().first, qint16(120));
            QCOMPARE(conPty->size().second, qint16(40));
            qInfo() << "[ConPTY] Resize verified";

            qInfo() << "[ConPTY] Closing interactive shell";
            QVERIFY2(conPty->kill(), "Could not close the interactive ConPTY shell");

            qInfo() << "[ConPTY] Smoke test completed for" << shellPath;
        }
    }

    void winpty()
    {
        //QVERIFY(false); //force quit
#ifdef PTYQT_DEBUG
        //run debug server
        killProcessByName(WINPTY_DBG_SERVER_NAME);

        qint64 debugServerPid;
        QProcess::startDetached(QString(WINPTY_DEBUG_SRV_PATH), QStringList() << "--everyone",
                                QCoreApplication::applicationDirPath(), &debugServerPid);
        QVERIFY(debugServerPid != 0);
#endif

        QStringList shells = getShells();

        foreach (QString shellPath, shells)
        {
            qDebug() << "Test" << shellPath;

            //create object
            QScopedPointer<IPtyProcess> winPty(PtyQt::createPtyProcess(IPtyProcess::WinPty));
            QCOMPARE(winPty->type(), IPtyProcess::WinPty);
            QVERIFY(winPty->isAvailable());

            //prepare to check shell welcome
            QEventLoop el;
            auto connection = QObject::connect(winPty->notifier(), &QIODevice::readyRead, [&winPty, &el]() {
                sleepByEventLoop(1);
                //qDebug() << "winPty.read" << winPty->readAll();
                winPty->readAll();
                el.quit();
            });

            //start WinPty agent and cmd.exe
            bool startResult = winPty->startProcess(shellPath, QProcessEnvironment::systemEnvironment().toStringList(), 200, 80);
#ifdef PTYQT_DEBUG
            if (!startResult)
                qDebug() << winPty->lastError() << winPty->dumpDebugInfo();
#endif
            QVERIFY(startResult);

            //check pid (winPty->pid() - PID of child process of winpty-agent.exe)
            QVERIFY(winPty->pid() != 0);
            //DWORD winPtyAgentPid = findProcessId(QString(WINPTY_AGENT_NAME).toStdString());
            //DWORD winPtyShellPid = findProcessId(QFileInfo(shellPath).fileName().toStdString(), winPtyAgentPid);
            //QCOMPARE(winPty->pid(), winPtyShellPid);

            //check shell welcome
            el.exec();
            winPty->notifier()->disconnect(connection);

            //check shell read after write
            bool testRes = false;
            connection = QObject::connect(winPty->notifier(), &QIODevice::readyRead, [&winPty, &el, &testRes]() {
                sleepByEventLoop(1);
                QString res = QString::fromUtf8(winPty->readAll());
                //qDebug() << res;

                //for e.g. bash return empty strings after needed data
                if (res.isEmpty() && testRes)
                    return;

                testRes = res.contains("winpty-agent.exe");
                testRes = testRes && res.contains("winpty.dll");
                testRes = testRes && res.contains("ptyqt_tests.exe");
                el.quit();
            });
            winPty->write("dir\r\n");
            el.exec();
            QVERIFY(testRes);
            winPty->notifier()->disconnect(connection);

            //resize window
            sleepByEventLoop(1);
            QVERIFY(winPty->resize(240, 90));

            //kill shell and winpty-agent processes
#ifdef PTYQT_DEBUG
            qDebug() << winPty->dumpDebugInfo();
            sleepByEventLoop(DEBUG_SLEEP_SEC);
#endif
            QVERIFY(winPty->kill());
            sleepByEventLoop(1);
            //QCOMPARE(findProcessId(QFileInfo(shellPath).fileName().toStdString(), winPtyAgentPid), 0);
            //QCOMPARE(findProcessId(QString(WINPTY_AGENT_NAME).toStdString()), 0);
        }

#ifdef PTYQT_DEBUG
        killProcessByName(WINPTY_DBG_SERVER_NAME);
#endif
    }
#endif
};

QTEST_MAIN(PtyQtTests)
#include "ptyqt_tests.moc"
