#include "conptyprocess.h"

#ifdef Q_OS_WIN

#include <QFileInfo>
#include <QDir>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QSysInfo>
#include <QMutexLocker>
#include <QDebug>

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::wstring quoteWindowsArgument(const QString &argument)
{
    const std::wstring value = argument.toStdWString();

    // Do not quote simple switches such as CMD's /D and /C. CMD has its own
    // command-line parser and does not treat quoted switches like a CRT argv
    // parser would. Quote values containing whitespace, quotes, or CMD
    // metacharacters; the remaining escaping follows the standard Windows
    // backslash-before-quote rules.
    if (!value.empty()
        && value.find_first_of(L" \t\n\v\"&|<>^()%!") == std::wstring::npos) {
        return value;
    }

    std::wstring result(1, L'"');
    size_t backslashes = 0;

    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
            backslashes = 0;
        }
    }

    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::vector<wchar_t> makeEnvironmentBlock(const QStringList &environment)
{
    QStringList entries = environment;
    entries.sort(Qt::CaseInsensitive);

    std::vector<wchar_t> block;
    for (const QString &entry : entries) {
        const std::wstring wideEntry = entry.toStdWString();
        block.insert(block.end(), wideEntry.cbegin(), wideEntry.cend());
        block.push_back(L'\0');
    }

    if (block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
}

WindowsContext::~WindowsContext()
{
    if (m_kernel32) {
        FreeLibrary(m_kernel32);
    }
}

bool WindowsContext::init()
{
    if (createPseudoConsole && resizePseudoConsole && closePseudoConsole) {
        return true;
    }

    m_kernel32 = LoadLibraryW(L"kernel32.dll");
    if (!m_kernel32) {
        m_lastError = QStringLiteral("Unable to load kernel32.dll");
        return false;
    }

    createPseudoConsole = reinterpret_cast<CreatePseudoConsolePtr>(GetProcAddress(m_kernel32, "CreatePseudoConsole"));
    resizePseudoConsole = reinterpret_cast<ResizePseudoConsolePtr>(GetProcAddress(m_kernel32, "ResizePseudoConsole"));
    closePseudoConsole = reinterpret_cast<ClosePseudoConsolePtr>(GetProcAddress(m_kernel32, "ClosePseudoConsole"));

    if (!createPseudoConsole || !resizePseudoConsole || !closePseudoConsole) {
        m_lastError = QStringLiteral("The Windows ConPTY API is not available");
        createPseudoConsole = nullptr;
        resizePseudoConsole = nullptr;
        closePseudoConsole = nullptr;
        FreeLibrary(m_kernel32);
        m_kernel32 = nullptr;
        return false;
    }

    return true;
}

QString WindowsContext::lastError() const
{
    return m_lastError;
}

PtyBuffer::PtyBuffer()
{
    open(QIODevice::ReadOnly);
}

PtyBuffer::~PtyBuffer() = default;

qint64 PtyBuffer::readData(char *data, qint64 maxlen)
{
    QMutexLocker locker(&m_mutex);
    const qint64 count = qMin(maxlen, static_cast<qint64>(m_readBuffer.size()));
    if (count <= 0) {
        return 0;
    }

    std::memcpy(data, m_readBuffer.constData(), static_cast<size_t>(count));
    m_readBuffer.remove(0, static_cast<qsizetype>(count));
#ifdef PTYQT_DEBUG
    qDebug().nospace() << "[ConPTY] [buffer] readData returned " << count
                       << " bytes; " << m_readBuffer.size() << " remain";
#endif
    return count;
}

qint64 PtyBuffer::writeData(const char *, qint64)
{
    return -1;
}

bool PtyBuffer::isSequential() const
{
    return true;
}

qint64 PtyBuffer::bytesAvailable() const
{
    QMutexLocker locker(&m_mutex);
    return static_cast<qint64>(m_readBuffer.size()) + QIODevice::bytesAvailable();
}

void PtyBuffer::append(const QByteArray &data)
{
    if (data.isEmpty()) {
        return;
    }
    qint64 bufferedBytes = 0;
    {
        QMutexLocker locker(&m_mutex);
        m_readBuffer.append(data);
        bufferedBytes = static_cast<qint64>(m_readBuffer.size());
    }
#ifdef PTYQT_DEBUG
    qDebug().nospace() << "[ConPTY] [buffer] appended " << data.size()
                       << " bytes; " << bufferedBytes << " buffered";
#endif
    Q_EMIT readyRead();
}

ConPtyProcess::ConPtyProcess() = default;

ConPtyProcess::~ConPtyProcess()
{
    kill();
}

HRESULT ConPtyProcess::createPseudoConsoleAndPipes(qint16 cols, qint16 rows)
{
    HANDLE ptyInput = INVALID_HANDLE_VALUE;
    HANDLE applicationOutput = INVALID_HANDLE_VALUE;
    HANDLE applicationInput = INVALID_HANDLE_VALUE;
    HANDLE ptyOutput = INVALID_HANDLE_VALUE;

    // Input channel: the application writes to applicationOutput; ConPTY reads
    // from ptyInput.
    if (!CreatePipe(&ptyInput, &applicationOutput, nullptr, 0)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Output channel: ConPTY writes to ptyOutput; the application reads from
    // applicationInput.
    if (!CreatePipe(&applicationInput, &ptyOutput, nullptr, 0)) {
        const DWORD error = GetLastError();
        CloseHandle(ptyInput);
        CloseHandle(applicationOutput);
        return HRESULT_FROM_WIN32(error);
    }

    m_hPipeIn = applicationInput;
    m_hPipeOut = applicationOutput;
    m_hPtyInput = ptyInput;
    m_hPtyOutput = ptyOutput;

    qDebug().nospace() << "[ConPTY] [pipes] app(write)=" << static_cast<void *>(m_hPipeOut)
                       << " app(read)=" << static_cast<void *>(m_hPipeIn)
                       << " ConPTY(read)=" << static_cast<void *>(m_hPtyInput)
                       << " ConPTY(write)=" << static_cast<void *>(m_hPtyOutput);

    const HRESULT result = m_winContext.createPseudoConsole(
        COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)},
        m_hPtyInput, m_hPtyOutput, 0, &m_ptyHandler);

    if (FAILED(result)) {
        qWarning().nospace() << "[ConPTY] [lifecycle] CreatePseudoConsole failed (HRESULT 0x"
                             << QString::number(static_cast<quint32>(result), 16) << ")";
        closePseudoConsolePipeHandles();
        return result;
    }

    qInfo().nospace() << "[ConPTY] [lifecycle] pseudoconsole created; size="
                      << cols << "x" << rows;
    return result;
}

HRESULT ConPtyProcess::initializeStartupInfoAttachedToPseudoConsole(STARTUPINFOEX *startupInfo, HPCON pty)
{
    SIZE_T attributeListSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);

    startupInfo->lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        std::malloc(attributeListSize));
    if (!startupInfo->lpAttributeList) {
        return E_OUTOFMEMORY;
    }

    startupInfo->StartupInfo.cb = sizeof(STARTUPINFOEX);

    // Explicitly prevent the child from reusing the host process' standard
    // handles. This is an empirical ConPTY workaround: with an attached host
    // console, some Windows configurations have been observed to route the
    // child's banner/prompt to the parent console instead of the ConPTY pipe.
    // INVALID_HANDLE_VALUE is used here as in the MiniPty backend experiment;
    // this is not the documented STARTF_USESTDHANDLES contract and must be
    // validated on the target Windows runners.
    startupInfo->StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    startupInfo->StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    startupInfo->StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    startupInfo->StartupInfo.hStdError = INVALID_HANDLE_VALUE;

    qDebug() << "[ConPTY] [process] explicitly suppressing inherited standard handles"
             << "(STARTF_USESTDHANDLES, all INVALID_HANDLE_VALUE)";

    if (!InitializeProcThreadAttributeList(startupInfo->lpAttributeList, 1, 0, &attributeListSize)) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        std::free(startupInfo->lpAttributeList);
        startupInfo->lpAttributeList = nullptr;
        return result;
    }

    if (!UpdateProcThreadAttribute(startupInfo->lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pty, sizeof(HPCON), nullptr, nullptr)) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        DeleteProcThreadAttributeList(startupInfo->lpAttributeList);
        std::free(startupInfo->lpAttributeList);
        startupInfo->lpAttributeList = nullptr;
        return result;
    }

    return S_OK;
}

QString ConPtyProcess::resolveExecutable(const QString &program)
{
    const QFileInfo info(program);
    if (info.isAbsolute() || program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\'))) {
        return info.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(program);
}

bool ConPtyProcess::startProcess(const QString &shellPath, QStringList environment, qint16 cols, qint16 rows)
{
    return startProcess(shellPath, {}, {}, std::move(environment), cols, rows);
}

bool ConPtyProcess::startProcess(const QString &program, const QStringList &arguments, const QString &workingDirectory,
                                 QStringList environment, qint16 cols, qint16 rows)
{
#ifdef TOO_OLD_WINSDK
    Q_UNUSED(program)
    Q_UNUSED(arguments)
    Q_UNUSED(workingDirectory)
    Q_UNUSED(environment)
    Q_UNUSED(cols)
    Q_UNUSED(rows)
    m_lastError = QStringLiteral("ConPTY requires Windows SDK 10.0.18346 or newer");
    return false;
#else
    qInfo().noquote() << "[ConPTY] [lifecycle] start requested:"
                      << program << "size=" << cols << "x" << rows;
    m_lastError.clear();
    if (!isAvailable()) {
        m_lastError = m_winContext.lastError();
        qWarning().noquote() << "[ConPTY] [lifecycle] unavailable:" << m_lastError;
        return false;
    }
    QMutexLocker lifecycleLock(&m_lifecycleMutex);
    if (isRunning() || m_ptyHandler || m_readThread || m_processThread) {
        m_lastError = QStringLiteral("A ConPTY process is already running");
        return false;
    }

    const QString executablePath = resolveExecutable(program);
    if (executablePath.isEmpty() || !QFileInfo(executablePath).isFile()) {
        m_lastError = QStringLiteral("Unable to resolve executable: %1").arg(program);
        return false;
    }

    QString currentDirectory;
    if (!workingDirectory.isEmpty()) {
        currentDirectory = QFileInfo(workingDirectory).absoluteFilePath();
        if (!QFileInfo(currentDirectory).isDir()) {
            m_lastError = QStringLiteral("Working directory does not exist: %1").arg(workingDirectory);
            return false;
        }
    }

    m_shellPath = executablePath;
    m_size = {cols, rows};
    m_exitCode.store(-1);

    HRESULT result = createPseudoConsoleAndPipes(cols, rows);
    if (FAILED(result)) {
        m_lastError = QStringLiteral("CreatePseudoConsole failed (0x%1)").arg(
            static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
        closeHandles();
        return false;
    }

    result = initializeStartupInfoAttachedToPseudoConsole(&m_startupInfo, m_ptyHandler);
    if (FAILED(result)) {
        m_lastError = QStringLiteral("Could not initialize the ConPTY process attributes (HRESULT 0x%1)")
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
        qWarning().noquote() << "[ConPTY] [lifecycle]" << m_lastError;
        closeHandles();
        return false;
    }
    qDebug() << "[ConPTY] [lifecycle] startup attributes initialized";

    QStringList processEnvironment = environment;
    if (processEnvironment.isEmpty()) {
        processEnvironment = QProcessEnvironment::systemEnvironment().toStringList();
    }
    const std::vector<wchar_t> environmentBlock = makeEnvironmentBlock(processEnvironment);

    // Always quote argv[0], even when the executable path has no spaces.
    // cmd.exe has its own command-line parser and must see an unambiguous
    // executable token before its /D and /C switches.
    const std::wstring nativeExecutable =
        QDir::toNativeSeparators(executablePath).toStdWString();
    std::wstring commandLine(1, L'"');
    commandLine += nativeExecutable;
    commandLine.push_back(L'"');

    for (const QString &argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.cbegin(), commandLine.cend());
    mutableCommandLine.push_back(L'\0');

    qInfo().noquote() << "[ConPTY] [process] CreateProcessW command line:"
                      << QString::fromStdWString(commandLine);

    const std::wstring nativeDirectory =
        QDir::toNativeSeparators(currentDirectory).toStdWString();
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        const_cast<wchar_t *>(environmentBlock.data()),
        currentDirectory.isEmpty() ? nullptr : nativeDirectory.c_str(),
        &m_startupInfo.StartupInfo,
        &m_processInfo);

    // Microsoft documents releasing the copies supplied to CreatePseudoConsole
    // after the CreateProcess call completes, not immediately after creation
    // of the pseudoconsole.
    const DWORD createProcessError = created ? ERROR_SUCCESS : GetLastError();
    closePseudoConsolePipeHandles();

    if (!created) {
        m_lastError = QStringLiteral("CreateProcessW failed with Windows error %1").arg(createProcessError);
        qWarning().noquote() << "[ConPTY] [process]" << m_lastError;
        closeHandles();
        return false;
    }

    m_pid = m_processInfo.dwProcessId;
    qInfo().nospace() << "[ConPTY] [process] child created; PID=" << m_pid;
    m_running.store(true);

    // Process lifetime and pipe lifetime are independent. A child can exit
    // while the ConPTY output pipe remains open until ClosePseudoConsole().
    // Monitor the process handle separately so isRunning() reflects the child.
    const HANDLE processHandle = m_processInfo.hProcess;
    const DWORD processId = m_processInfo.dwProcessId;
    m_processThread = QThread::create([this, processHandle, processId]() {
        qDebug().nospace() << "[ConPTY] [process] PID " << processId
                           << ": waiting for process handle";

        const DWORD waitResult = WaitForSingleObject(processHandle, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            qDebug().nospace() << "[ConPTY] [process] PID " << processId
                               << ": process handle signaled";
        } else if (waitResult == WAIT_FAILED) {
            const DWORD waitError = GetLastError();
            qWarning().nospace() << "[ConPTY] [process] PID " << processId
                                 << ": waiting for process failed (Win32 error " << waitError << ")";
        } else {
            qWarning().nospace() << "[ConPTY] [process] PID " << processId
                                 << ": unexpected process wait result "
                                 << QStringLiteral("0x%1").arg(waitResult, 8, 16, QLatin1Char('0'));
        }

        if (waitResult == WAIT_OBJECT_0) {
            DWORD code = static_cast<DWORD>(-1);
            if (GetExitCodeProcess(processHandle, &code)) {
                m_exitCode.store(static_cast<int>(code));
                qInfo().nospace() << "[ConPTY] [process] PID " << processId
                                  << ": exited with code " << code;
            } else {
                const DWORD error = GetLastError();
                qWarning().nospace() << "[ConPTY] [process] PID " << processId
                                     << ": GetExitCodeProcess failed (error " << error << ")";
            }
        }

        m_running.store(false);
        qInfo().nospace() << "[ConPTY] [process] PID " << processId
                          << ": state=stopped, exitCode=" << m_exitCode.load();
    });
    qInfo().nospace() << "[ConPTY] [process] PID " << processId << ": starting monitor thread";
    m_processThread->start();

    m_readThread = QThread::create([this, processId]() {
        std::array<char, 4096> buffer{};
#ifdef PTYQT_DEBUG
        qDebug().nospace() << "[ConPTY] [reader] PID " << processId
                           << ": started (output pipe=" << static_cast<void *>(m_hPipeIn) << ")";
#endif
        for (;;) {
            DWORD bytesRead = 0;
            const BOOL success = ReadFile(m_hPipeIn, buffer.data(), static_cast<DWORD>(buffer.size()),
                                          &bytesRead, nullptr);
            const DWORD readError = success ? ERROR_SUCCESS : GetLastError();
#ifdef PTYQT_DEBUG
            qDebug().nospace() << "[ConPTY] [reader] PID " << processId
                               << ": ReadFile success=" << (success ? 1 : 0)
                               << " bytes=" << bytesRead << " error=" << readError;
#endif
            if (bytesRead > 0) {
                m_buffer.append(QByteArray(buffer.data(), static_cast<qsizetype>(bytesRead)));
            }
            if (!success || bytesRead == 0) {
                if (!success && readError == ERROR_BROKEN_PIPE) {
                    qInfo().nospace() << "[ConPTY] [reader] PID " << processId
                                      << ": output pipe closed (ERROR_BROKEN_PIPE)";
                } else if (!success) {
                    qWarning().nospace() << "[ConPTY] [reader] PID " << processId
                                         << ": ReadFile failed (Win32 error " << readError << ")";
                } else {
                    qInfo().nospace() << "[ConPTY] [reader] PID " << processId
                                      << ": output stream reached EOF";
                }
#ifdef PTYQT_DEBUG
                qDebug().nospace() << "[ConPTY] [reader] PID " << processId << ": stopping";
#endif
                break;
            }
        }
    });
    m_readThread->start();
    return true;
#endif
}

bool ConPtyProcess::resize(qint16 cols, qint16 rows)
{
#ifdef KPTY_HAVE_CONPTY_SDK
    QMutexLocker lifecycleLock(&m_lifecycleMutex);
    if (!m_ptyHandler || !m_winContext.resizePseudoConsole) {
        return false;
    }
    const HRESULT result = m_winContext.resizePseudoConsole(
        m_ptyHandler, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
    if (SUCCEEDED(result)) {
        m_size = {cols, rows};
        qInfo().nospace() << "[ConPTY] [resize] terminal size changed to "
                          << cols << "x" << rows;
        return true;
    }
    qWarning().nospace() << "[ConPTY] [resize] ResizePseudoConsole failed (HRESULT 0x"
                         << QString::number(static_cast<quint32>(result), 16) << ")";
#endif
    return false;
}

bool ConPtyProcess::kill()
{
    QMutexLocker lifecycleLock(&m_lifecycleMutex);
    const bool hadProcess = m_ptyHandler || m_processInfo.hProcess || m_readThread || m_processThread;
    if (!hadProcess) {
        return false;
    }

    qInfo().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                      << ": begin (running=" << (isRunning() ? 1 : 0) << ")";

#ifdef KPTY_HAVE_CONPTY_SDK
    if (m_ptyHandler && m_winContext.closePseudoConsole) {
        qInfo().nospace() << "[ConPTY] [cleanup] PID " << m_pid << ": closing pseudoconsole";
        m_winContext.closePseudoConsole(m_ptyHandler);
        m_ptyHandler = nullptr;
        qInfo().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                          << ": ClosePseudoConsole returned";
    }
#endif

    // Closing the pseudo-console releases the output pipe. The process itself
    // is monitored independently, so wait for it here and only terminate it
    // when graceful shutdown does not complete within the grace period.
    if (m_processInfo.hProcess) {
        const DWORD waitResult = WaitForSingleObject(m_processInfo.hProcess, 1500);
        if (waitResult == WAIT_OBJECT_0) {
            qInfo().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                              << ": child exited after pseudoconsole close";
        } else if (waitResult == WAIT_TIMEOUT) {
            qWarning().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                                 << ": child did not exit within 1500 ms; terminating";
        } else if (waitResult == WAIT_FAILED) {
            qWarning().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                                 << ": process wait failed (Win32 error " << GetLastError() << ")";
        } else {
            qWarning().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                                 << ": unexpected process wait result "
                                 << QStringLiteral("0x%1").arg(waitResult, 8, 16, QLatin1Char('0'));
        }
        if (waitResult == WAIT_TIMEOUT) {
            const BOOL terminated = TerminateProcess(m_processInfo.hProcess, 1);
            const DWORD terminateError = terminated ? ERROR_SUCCESS : GetLastError();
            qInfo().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                              << ": TerminateProcess=" << (terminated ? 1 : 0)
                              << " (error " << terminateError << ")";
            WaitForSingleObject(m_processInfo.hProcess, INFINITE);
        }
    }

    // The process monitor must finish before its process handle is closed.
    if (m_processThread) {
        m_processThread->wait();
        qDebug().nospace() << "[ConPTY] [cleanup] PID " << m_pid << ": process monitor joined";
        delete m_processThread;
        m_processThread = nullptr;
    }

    // Drain output until the pseudo-console closes its pipe. If it remains
    // blocked, cancel the outstanding read before releasing the pipe handle.
    if (m_readThread && !m_readThread->wait(1500)) {
        qWarning().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                             << ": reader has not stopped; cancelling pending pipe I/O";
        if (m_hPipeIn != INVALID_HANDLE_VALUE) {
            const BOOL cancelled = CancelIoEx(m_hPipeIn, nullptr);
            const DWORD cancelError = cancelled ? ERROR_SUCCESS : GetLastError();
            qDebug().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                               << ": CancelIoEx=" << (cancelled ? 1 : 0)
                               << " (error " << cancelError << ")";
        }
        m_readThread->wait();
    }

    if (m_readThread) {
        qDebug().nospace() << "[ConPTY] [cleanup] PID " << m_pid << ": output reader joined";
        delete m_readThread;
        m_readThread = nullptr;
    }

    const int finalExitCode = m_exitCode.load();
    closeHandles();
    m_running.store(false);
    qInfo().nospace() << "[ConPTY] [cleanup] PID " << m_pid
                      << ": complete (exitCode=" << finalExitCode << ")";
    m_pid = 0;
    return true;
}

void ConPtyProcess::closePseudoConsolePipeHandles()
{
    if (m_hPtyInput != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPtyInput);
        m_hPtyInput = INVALID_HANDLE_VALUE;
    }
    if (m_hPtyOutput != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPtyOutput);
        m_hPtyOutput = INVALID_HANDLE_VALUE;
    }
}

void ConPtyProcess::closeHandles()
{
    // Startup rollback: the normal kill() path closes HPCON while the reader
    // is draining; before a child/reader exists, close it here.
    if (m_ptyHandler && m_winContext.closePseudoConsole) {
        qDebug() << "[ConPTY] [cleanup] closing pseudoconsole during startup rollback";
        m_winContext.closePseudoConsole(m_ptyHandler);
        m_ptyHandler = nullptr;
    }
    closePseudoConsolePipeHandles();

    if (m_startupInfo.lpAttributeList) {
        DeleteProcThreadAttributeList(m_startupInfo.lpAttributeList);
        std::free(m_startupInfo.lpAttributeList);
        m_startupInfo.lpAttributeList = nullptr;
    }
    if (m_processInfo.hThread) {
        CloseHandle(m_processInfo.hThread);
        m_processInfo.hThread = nullptr;
    }
    if (m_processInfo.hProcess) {
        CloseHandle(m_processInfo.hProcess);
        m_processInfo.hProcess = nullptr;
    }
    if (m_hPipeOut != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeOut);
        m_hPipeOut = INVALID_HANDLE_VALUE;
    }
    if (m_hPipeIn != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeIn);
        m_hPipeIn = INVALID_HANDLE_VALUE;
    }
    m_ptyHandler = nullptr;
}

IPtyProcess::PtyType ConPtyProcess::type()
{
    return ConPty;
}

QString ConPtyProcess::dumpDebugInfo()
{
#ifdef PTYQT_DEBUG
    return QStringLiteral("PID: %1, Type: %2, Cols: %3, Rows: %4")
        .arg(m_pid).arg(type()).arg(m_size.first).arg(m_size.second);
#else
    return QStringLiteral("Nothing");
#endif
}

QIODevice *ConPtyProcess::notifier()
{
    return &m_buffer;
}

QByteArray ConPtyProcess::readAll()
{
    return m_buffer.readAll();
}

qint64 ConPtyProcess::write(const QByteArray &byteArray)
{
    QMutexLocker lifecycleLock(&m_lifecycleMutex);
    if (m_hPipeOut == INVALID_HANDLE_VALUE || byteArray.isEmpty()) {
        return 0;
    }

    DWORD bytesWritten = 0;
    if (!WriteFile(m_hPipeOut, byteArray.constData(), static_cast<DWORD>(byteArray.size()),
                   &bytesWritten, nullptr)) {
        const DWORD error = GetLastError();
        m_lastError = QStringLiteral("WriteFile failed with Windows error %1").arg(error);
        qWarning().noquote() << "[ConPTY] [io]" << m_lastError;
        return -1;
    }
#ifdef PTYQT_DEBUG
    qDebug().nospace() << "[ConPTY] [io] wrote " << bytesWritten << " bytes";
#endif
    return static_cast<qint64>(bytesWritten);
}

bool ConPtyProcess::isAvailable()
{
#ifdef TOO_OLD_WINSDK
    return false;
#else
    const QString version = QSysInfo::kernelVersion();
    const int buildNumber = version.section(QLatin1Char('.'), -1).toInt();
    return buildNumber >= CONPTY_MINIMAL_WINDOWS_VERSION && m_winContext.init();
#endif
}

bool ConPtyProcess::isRunning() const
{
    return m_running.load();
}

int ConPtyProcess::exitCode() const
{
    return m_exitCode.load();
}

void ConPtyProcess::moveToThread(QThread *targetThread)
{
    Q_UNUSED(targetThread)
}
#endif // Q_OS_WIN
