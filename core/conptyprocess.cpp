#include "conptyprocess.h"

#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QSysInfo>
#include <QMutexLocker>

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
        block.push_back(L'\\0');
    }

    if (block.empty()) {
        block.push_back(L'\\0');
    }
    block.push_back(L'\\0');
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
    {
        QMutexLocker locker(&m_mutex);
        m_readBuffer.append(data);
    }
    Q_EMIT readyRead();
}

ConPtyProcess::ConPtyProcess() = default;

ConPtyProcess::~ConPtyProcess()
{
    kill();
}

HRESULT ConPtyProcess::createPseudoConsoleAndPipes(HPCON *pty, HANDLE *input, HANDLE *output, qint16 cols, qint16 rows)
{
    HANDLE ptyInput = INVALID_HANDLE_VALUE;
    HANDLE ptyOutput = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&ptyInput, output, nullptr, 0)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (!CreatePipe(input, &ptyOutput, nullptr, 0)) {
        const DWORD error = GetLastError();
        CloseHandle(ptyInput);
        CloseHandle(*output);
        *output = INVALID_HANDLE_VALUE;
        return HRESULT_FROM_WIN32(error);
    }

    const HRESULT result = m_winContext.createPseudoConsole(
        COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)},
        ptyInput, ptyOutput, 0, pty);

    CloseHandle(ptyInput);
    CloseHandle(ptyOutput);
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
    QMutexLocker lifecycleLock(&m_lifecycleMutex);
    if (!isAvailable()) {
        m_lastError = m_winContext.lastError();
        return false;
    }
    if (isRunning() || m_ptyHandler || m_readThread) {
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

    HRESULT result = createPseudoConsoleAndPipes(&m_ptyHandler, &m_hPipeIn, &m_hPipeOut, cols, rows);
    if (FAILED(result)) {
        m_lastError = QStringLiteral("CreatePseudoConsole failed (0x%1)").arg(
            static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
        closeHandles();
        return false;
    }

    result = initializeStartupInfoAttachedToPseudoConsole(&m_startupInfo, m_ptyHandler);
    if (FAILED(result)) {
        m_lastError = QStringLiteral("Could not initialize the ConPTY process attributes");
        closeHandles();
        return false;
    }

    QStringList processEnvironment = environment;
    if (processEnvironment.isEmpty()) {
        processEnvironment = QProcessEnvironment::systemEnvironment().toStringList();
    }
    const std::vector<wchar_t> environmentBlock = makeEnvironmentBlock(processEnvironment);

    std::wstring commandLine = quoteWindowsArgument(executablePath);
    for (const QString &argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.cbegin(), commandLine.cend());
    mutableCommandLine.push_back(L'\\0');

    const std::wstring nativeExecutable = executablePath.toStdWString();
    const std::wstring nativeDirectory = currentDirectory.toStdWString();
    const BOOL created = CreateProcessW(
        nativeExecutable.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        const_cast<wchar_t *>(environmentBlock.data()),
        currentDirectory.isEmpty() ? nullptr : nativeDirectory.c_str(),
        &m_startupInfo.StartupInfo,
        &m_processInfo);

    if (!created) {
        m_lastError = QStringLiteral("CreateProcessW failed with Windows error %1").arg(GetLastError());
        closeHandles();
        return false;
    }

    m_pid = m_processInfo.dwProcessId;
    m_running.store(true);

    m_readThread = QThread::create([this]() {
        std::array<char, 4096> buffer{};
        for (;;) {
            DWORD bytesRead = 0;
            const BOOL success = ReadFile(m_hPipeIn, buffer.data(), static_cast<DWORD>(buffer.size()),
                                          &bytesRead, nullptr);
            if (bytesRead > 0) {
                m_buffer.append(QByteArray(buffer.data(), static_cast<qsizetype>(bytesRead)));
            }
            if (!success || bytesRead == 0) {
                break;
            }
        }

        if (m_processInfo.hProcess) {
            WaitForSingleObject(m_processInfo.hProcess, INFINITE);
            DWORD code = static_cast<DWORD>(-1);
            if (GetExitCodeProcess(m_processInfo.hProcess, &code)) {
                m_exitCode.store(static_cast<int>(code));
            }
        }
        m_running.store(false);
    });
    m_readThread->start();
    return true;
#endif
}

bool ConPtyProcess::resize(qint16 cols, qint16 rows)
{
#ifndef TOO_OLD_WINSDK
    QMutexLocker lifecycleLock(&m_lifecycleMutex);
    if (!m_ptyHandler || !m_winContext.resizePseudoConsole) {
        return false;
    }
    const HRESULT result = m_winContext.resizePseudoConsole(
        m_ptyHandler, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
    if (SUCCEEDED(result)) {
        m_size = {cols, rows};
        return true;
    }
#endif
    return false;
}

bool ConPtyProcess::kill()
{
    QMutexLocker lifecycleLock(&m_lifecycleMutex);
    const bool hadProcess = m_ptyHandler || m_processInfo.hProcess || m_readThread;
    if (!hadProcess) {
        return false;
    }

#ifndef TOO_OLD_WINSDK
    if (m_ptyHandler && m_winContext.closePseudoConsole) {
        m_winContext.closePseudoConsole(m_ptyHandler);
        m_ptyHandler = nullptr;
    }
#endif

    if (m_processInfo.hProcess && WaitForSingleObject(m_processInfo.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(m_processInfo.hProcess, 1);
        WaitForSingleObject(m_processInfo.hProcess, INFINITE);
    }

    if (m_readThread) {
        m_readThread->wait();
        delete m_readThread;
        m_readThread = nullptr;
    }

    closeHandles();
    m_running.store(false);
    m_pid = 0;
    return true;
}

void ConPtyProcess::closeHandles()
{
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
        m_lastError = QStringLiteral("WriteFile failed with Windows error %1").arg(GetLastError());
        return -1;
    }
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
