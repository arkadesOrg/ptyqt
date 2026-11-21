#include <QTest>
#include "ptyqt.h"
#include <QProcessEnvironment>
#include <QThread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <string>
#include <QTimer>

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
DWORD findProcessId(const std::string& processName, int parentProcessId = 0)
{
    PROCESSENTRY32 processInfo;
    processInfo.dwSize = sizeof(processInfo);

    HANDLE processesSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (processesSnapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    Process32First(processesSnapshot, &processInfo);
    if (!processName.compare(processInfo.szExeFile))
    {
        if (parentProcessId == 0 || parentProcessId == processInfo.th32ParentProcessID)
        {
            CloseHandle(processesSnapshot);
            return processInfo.th32ProcessID;
        }
    }

    while (Process32Next(processesSnapshot, &processInfo))
    {
        if (!processName.compare(processInfo.szExeFile))
        {
            if (parentProcessId == 0 || parentProcessId == processInfo.th32ParentProcessID)
            {
                CloseHandle(processesSnapshot);
                return processInfo.th32ProcessID;
            }
        }
    }

    CloseHandle(processesSnapshot);
    return 0;
}

void killProcessByName(QString processName)
{
    if (findProcessId(processName.toStdString()))
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

    //unix unit tests
#ifdef Q_OS_UNIX
    void unixpty()
    {
        QString shellPath = "/bin/bash";

        QScopedPointer<IPtyProcess> unixPty(PtyQt::createPtyProcess(IPtyProcess::UnixPty));
        QCOMPARE(unixPty->type(), IPtyProcess::UnixPty);
        QVERIFY(unixPty->isAvailable());

        //start UnixPty agent and cmd.exe
        bool startResult = unixPty->startProcess(shellPath, QProcessEnvironment::systemEnvironment().toStringList(), 200, 80);
#ifdef PTYQT_DEBUG
        if (!startResult)
            qDebug() << unixPty->lastError() << unixPty->dumpDebugInfo();
#endif
        QVERIFY(startResult);

        //check pid
        QVERIFY(unixPty->pid() != 0);

        //check shell welcome
        QEventLoop el;
        auto connection = QObject::connect(unixPty->notifier(), &QIODevice::readyRead, [&unixPty, &el]() {
            sleepByEventLoop(1);
            qDebug() << "unixPty.read" << unixPty->readAll();
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
        qDebug() << "ptyin:" << unixPty->write("ls\n");
        el.exec();
        QVERIFY(testRes);
        qDebug() << "ptyin:" << unixPty->write("ls -alh\n");
        el.exec();
        QVERIFY(testRes);
        unixPty->notifier()->disconnect(connection);

        //resize window
        sleepByEventLoop(1);
        QVERIFY(unixPty->resize(240, 90));
    }
#endif

    // Windows unit tests
#ifdef Q_OS_WIN
    void conpty()
    {
        const int buildNumber = QSysInfo::kernelVersion().section(QLatin1Char('.'), -1).toInt();
        if (buildNumber < CONPTY_MINIMAL_WINDOWS_VERSION) {
            QSKIP("ConPTY requires Windows 10 build 18309 or newer");
        }

        const QStringList shells = getShells();
        if (shells.isEmpty()) {
            QSKIP("No supported Windows command interpreter was found");
        }

        for (const QString &shellPath : shells) {
            QScopedPointer<IPtyProcess> process(PtyQt::createPtyProcess(IPtyProcess::ConPty));
            QVERIFY(process);
            QCOMPARE(process->type(), IPtyProcess::ConPty);
            QVERIFY(process->isAvailable());

            QEventLoop readyLoop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &readyLoop, &QEventLoop::quit);
            connect(process->notifier(), &QIODevice::readyRead, &readyLoop, &QEventLoop::quit);

            QVERIFY2(process->startProcess(shellPath, QProcessEnvironment::systemEnvironment().toStringList(), 100, 30),
                     qPrintable(process->lastError()));
            QVERIFY(process->pid() != 0);

            timeout.start(5000);
            if (timeout.isActive()) {
                readyLoop.exec();
            }
            timeout.stop();

            // The read side is a real QIODevice: consuming bytes drains its buffer.
            const QByteArray startupOutput = process->readAll();
            QVERIFY(!startupOutput.isNull());
            QCOMPARE(process->notifier()->bytesAvailable(), qint64(0));

            QVERIFY(process->resize(120, 40));
            QCOMPARE(process->size().first, qint16(120));
            QCOMPARE(process->size().second, qint16(40));

            // Run a command whose argument contains whitespace and a quote.
            const QString marker = QStringLiteral("ptyqt argument roundtrip");
            const QString script = QStringLiteral("Write-Output '%1'; exit 7").arg(marker);
            const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell"));
            const QString pwsh = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
            QString executable = !pwsh.isEmpty() ? pwsh : powershell;

            if (!executable.isEmpty()) {
                QVERIFY(process->kill());
                QScopedPointer<IPtyProcess> commandProcess(PtyQt::createPtyProcess(IPtyProcess::ConPty));
                QVERIFY(commandProcess->startProcess(
                    executable,
                    {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                     QStringLiteral("-Command"), script},
                    QCoreApplication::applicationDirPath(),
                    QProcessEnvironment::systemEnvironment().toStringList(),
                    100, 30));

                QEventLoop commandLoop;
                QTimer commandTimeout;
                commandTimeout.setSingleShot(true);
                connect(commandProcess->notifier(), &QIODevice::readyRead, &commandLoop, &QEventLoop::quit);
                connect(&commandTimeout, &QTimer::timeout, &commandLoop, &QEventLoop::quit);

                QByteArray output;
                commandTimeout.start(10000);
                while (commandTimeout.isActive() && !output.contains(marker.toUtf8())) {
                    commandLoop.exec();
                    output += commandProcess->readAll();
                }
                commandTimeout.stop();

                QVERIFY2(output.contains(marker.toUtf8()), output.constData());
                QTRY_VERIFY_WITH_TIMEOUT(!static_cast<ConPtyProcess *>(commandProcess.data())->isRunning(), 10000);
                QCOMPARE(static_cast<ConPtyProcess *>(commandProcess.data())->exitCode(), 7);
            } else {
                // Keep basic ConPTY coverage on minimal Windows images.
                QVERIFY(process->kill());
            }
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
