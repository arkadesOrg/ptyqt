#include <QTest>
#include "ptyqt.h"
#ifdef Q_OS_WIN
#include "conptyprocess.h"
#endif
#include <QProcessEnvironment>
#include <QFile>
#include <QSysInfo>
#include <QThread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <string>
#include <QTimer>
#include <QStandardPaths>
#include <QDateTime>

#ifdef Q_OS_WIN
#ifndef _WINDEF_
typedef unsigned long DWORD;
#endif
#endif

#define WINPTY_DBG_SERVER_NAME "winpty-debugserver.exe"
#define WINPTY_AGENT_NAME "winpty-agent.exe"

//increase it for visual control each shell
#define DEBUG_SLEEP_SEC 1

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

            QScopedPointer<IPtyProcess> process(PtyQt::createPtyProcess(IPtyProcess::ConPty));
            QVERIFY(process);
            QCOMPARE(process->type(), IPtyProcess::ConPty);
            QVERIFY2(process->isAvailable(), "The Windows ConPTY API is unavailable");

            QEventLoop readyLoop;
            QTimer readyTimeout;
            readyTimeout.setSingleShot(true);
            connect(&readyTimeout, &QTimer::timeout, &readyLoop, &QEventLoop::quit);
            connect(process->notifier(), &QIODevice::readyRead, &readyLoop, &QEventLoop::quit);

            qInfo() << "[ConPTY] Launching shell with initial size 100x30";
            QVERIFY2(process->startProcess(shellPath, QProcessEnvironment::systemEnvironment().toStringList(), 100, 30),
                     qPrintable(process->lastError()));
            QVERIFY(process->pid() != 0);
            qInfo() << "[ConPTY] Shell started with PID" << process->pid();

            if (process->notifier()->bytesAvailable() == 0) {
                readyTimeout.start(5000);
                readyLoop.exec();
                readyTimeout.stop();
            }

            const QByteArray startupOutput = process->readAll();
            if (startupOutput.isEmpty()) {
                qWarning() << "[ConPTY] No initial shell output received within 5 seconds";
            } else {
                qInfo().noquote() << "[ConPTY] Initial shell output:"
                                  << QString::fromLocal8Bit(startupOutput).trimmed();
            }
            QCOMPARE(process->notifier()->bytesAvailable(), qint64(0));

            qInfo() << "[ConPTY] Resizing terminal to 120x40";
            QVERIFY2(process->resize(120, 40), "ConPTY resize to 120x40 failed");
            QCOMPARE(process->size().first, qint16(120));
            QCOMPARE(process->size().second, qint16(40));
            qInfo() << "[ConPTY] Resize verified";

            const QString pwsh = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
            const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell"));
            const QString executable = !pwsh.isEmpty() ? pwsh : powershell;
            QVERIFY2(!executable.isEmpty(), "Neither pwsh.exe nor powershell.exe is available on PATH");
            qInfo().noquote() << "[ConPTY] Using command interpreter:" << executable;

            qInfo() << "[ConPTY] Closing interactive shell";
            QVERIFY2(process->kill(), "Could not close the interactive ConPTY shell");

            // Exercise the ConPTY-specific overload that accepts arguments and a working directory.
            QScopedPointer<ConPtyProcess> commandProcess(new ConPtyProcess);
            const QStringList arguments{
                QStringLiteral("-NoProfile"),
                QStringLiteral("-NonInteractive"),
                QStringLiteral("-Command"),
                QStringLiteral("Write-Output 'ptyqt-argument-check'; exit 7")
            };
            qInfo() << "[ConPTY] Launching argument/working-directory test process";
            QVERIFY2(commandProcess->startProcess(
                executable,
                arguments,
                QCoreApplication::applicationDirPath(),
                QProcessEnvironment::systemEnvironment().toStringList(),
                100, 30), qPrintable(commandProcess->lastError()));
            qInfo() << "[ConPTY] Test process started with PID" << commandProcess->pid();

            QEventLoop commandLoop;
            QTimer commandTimeout;
            commandTimeout.setSingleShot(true);
            connect(commandProcess->notifier(), &QIODevice::readyRead, &commandLoop, &QEventLoop::quit);
            connect(&commandTimeout, &QTimer::timeout, &commandLoop, &QEventLoop::quit);

            const QByteArray expectedOutput("ptyqt-argument-check");
            QByteArray output;
            commandTimeout.start(10000);
            while (commandTimeout.isActive() && !output.contains(expectedOutput)) {
                commandLoop.exec();
                output += commandProcess->readAll();
            }
            output += commandProcess->readAll();

            qInfo().noquote() << "[ConPTY] Captured command output:"
                              << QString::fromLocal8Bit(output).trimmed();
            QVERIFY2(output.contains(expectedOutput),
                     qPrintable(QStringLiteral("Expected marker was not received; captured output: %1")
                                    .arg(QString::fromLocal8Bit(output))));

            qInfo() << "[ConPTY] Waiting for process exit";
            QTRY_VERIFY_WITH_TIMEOUT(!commandProcess->isRunning(), 10000);
            const int exitCode = commandProcess->exitCode();
            qInfo() << "[ConPTY] Process exited with code" << exitCode;
            QCOMPARE(exitCode, 7);

            qInfo() << "[ConPTY] Releasing process and pseudo-console resources";
            QVERIFY2(commandProcess->kill(), "ConPTY resource cleanup failed");
            qInfo() << "[ConPTY] All smoke checks passed for" << shellPath;
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
