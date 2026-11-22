#include "unixptyprocess.h"
#include <QStandardPaths>

#include <termios.h>
#include <errno.h>
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_FREEBSD)
#include <utmpx.h>
#endif
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>
#include <array>

UnixPtyProcess::UnixPtyProcess()
    : IPtyProcess()
    , m_readMasterNotify(0)
{
    m_shellProcess.setWorkingDirectory(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));

    QObject::connect(&m_shellProcess, &QProcess::started, &m_shellProcess, [this]() {
        qInfo().nospace() << "[UnixPTY] [process] started; PID=" << m_shellProcess.processId();
    });
    QObject::connect(&m_shellProcess, &QProcess::finished, &m_shellProcess,
                     [this](int exitCode, QProcess::ExitStatus status) {
        qInfo().nospace() << "[UnixPTY] [process] PID " << m_pid
                          << ": finished; exitCode=" << exitCode
                          << " exitStatus=" << static_cast<int>(status);
    });
    QObject::connect(&m_shellProcess, &QProcess::errorOccurred, &m_shellProcess,
                     [this](QProcess::ProcessError error) {
        qWarning().noquote() << "[UnixPTY] [process] PID" << m_pid
                             << "error=" << static_cast<int>(error)
                             << m_shellProcess.errorString();
    });
}

UnixPtyProcess::~UnixPtyProcess()
{
    kill();
}

bool UnixPtyProcess::startProcess(const QString &shellPath, QStringList environment, qint16 cols, qint16 rows)
{
    qInfo().noquote() << "[UnixPTY] [lifecycle] start requested:"
                      << shellPath << "size=" << cols << "x" << rows;
    m_lastError.clear();

    if (!isAvailable())
    {
        m_lastError = QString("UnixPty Error: unavailable");
        qWarning().noquote() << "[UnixPTY] [lifecycle]" << m_lastError;
        return false;
    }

    if (m_shellProcess.state() == QProcess::Running) {
        m_lastError = QStringLiteral("A Unix PTY process is already running");
        qWarning().noquote() << "[UnixPTY] [lifecycle]" << m_lastError;
        return false;
    }

    QFileInfo fi(shellPath);
    if (fi.isRelative() || !QFile::exists(shellPath))
    {
        //todo add auto-find executable in PATH env var
        m_lastError = QString("UnixPty Error: shell file path must be absolute");
        qWarning().noquote() << "[UnixPTY] [lifecycle]" << m_lastError;
        return false;
    }

    m_shellPath = shellPath;
    m_size = QPair<qint16, qint16>(cols, rows);

    int rc = 0;

    m_shellProcess.m_handleMaster = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (m_shellProcess.m_handleMaster < 0)
    {
        m_lastError = QString("UnixPty Error: unable to open master -> %1").arg(strerror(errno));
        qWarning().noquote() << "[UnixPTY] [pty]" << m_lastError;
        kill();
        return false;
    }

    m_shellProcess.m_handleSlaveName = ptsname(m_shellProcess.m_handleMaster);
    if ( m_shellProcess.m_handleSlaveName.isEmpty())
    {
        m_lastError = QString("UnixPty Error: unable to get slave name -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    rc = grantpt(m_shellProcess.m_handleMaster);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: unable to change perms for slave -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    rc = unlockpt(m_shellProcess.m_handleMaster);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: unable to unlock slave -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    m_shellProcess.m_handleSlave = ::open(m_shellProcess.m_handleSlaveName.toLatin1().data(), O_RDWR | O_NOCTTY);
    if (m_shellProcess.m_handleSlave < 0)
    {
        m_lastError = QString("UnixPty Error: unable to open slave -> %1").arg(strerror(errno));
        qWarning().noquote() << "[UnixPTY] [pty]" << m_lastError;
        kill();
        return false;
    }

    qInfo().nospace() << "[UnixPTY] [pty] master fd=" << m_shellProcess.m_handleMaster
                      << " slave fd=" << m_shellProcess.m_handleSlave
                      << " slave path=" << m_shellProcess.m_handleSlaveName;

    rc = fcntl(m_shellProcess.m_handleMaster, F_SETFD, FD_CLOEXEC);
    if (rc == -1)
    {
        m_lastError = QString("UnixPty Error: unable to set flags for master -> %1").arg(strerror(errno));
        qWarning().noquote() << "[UnixPTY] [pty]" << m_lastError;
        kill();
        return false;
    }

    rc = fcntl(m_shellProcess.m_handleSlave, F_SETFD, FD_CLOEXEC);
    if (rc == -1)
    {
        m_lastError = QString("UnixPty Error: unable to set flags for slave -> %1").arg(strerror(errno));
        qWarning().noquote() << "[UnixPTY] [pty]" << m_lastError;
        kill();
        return false;
    }

    struct ::termios ttmode;
    rc = tcgetattr(m_shellProcess.m_handleMaster, &ttmode);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: termios fail -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    ttmode.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#if defined(IUTF8)
    ttmode.c_iflag |= IUTF8;
#endif

    ttmode.c_oflag = OPOST | ONLCR;
    ttmode.c_cflag = CREAD | CS8 | HUPCL;
    ttmode.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;

    ttmode.c_cc[VEOF] = 4;
    ttmode.c_cc[VEOL] = -1;
    ttmode.c_cc[VEOL2] = -1;
    ttmode.c_cc[VERASE] = 0x7f;
    ttmode.c_cc[VWERASE] = 23;
    ttmode.c_cc[VKILL] = 21;
    ttmode.c_cc[VREPRINT] = 18;
    ttmode.c_cc[VINTR] = 3;
    ttmode.c_cc[VQUIT] = 0x1c;
    ttmode.c_cc[VSUSP] = 26;
    ttmode.c_cc[VSTART] = 17;
    ttmode.c_cc[VSTOP] = 19;
    ttmode.c_cc[VLNEXT] = 22;
    ttmode.c_cc[VDISCARD] = 15;
    ttmode.c_cc[VMIN] = 1;
    ttmode.c_cc[VTIME] = 0;

#if (__APPLE__)
    ttmode.c_cc[VDSUSP] = 25;
    ttmode.c_cc[VSTATUS] = 20;
#endif

    cfsetispeed(&ttmode, B38400);
    cfsetospeed(&ttmode, B38400);

    rc = tcsetattr(m_shellProcess.m_handleMaster, TCSANOW, &ttmode);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: unabble to set associated params -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    m_readMasterNotify = new QSocketNotifier(m_shellProcess.m_handleMaster, QSocketNotifier::Read, &m_shellProcess);
    m_readMasterNotify->setEnabled(true);
    m_readMasterNotify->moveToThread(m_shellProcess.thread());
    QObject::connect(m_readMasterNotify, &QSocketNotifier::activated, [this](int socket)
    {
        Q_UNUSED(socket)

        // One bounded read per activation: a blocking PTY read may return a
        // full buffer even when no further data is immediately available.
        std::array<char, 4096> nativeBuffer{};
        ssize_t len = -1;
        do {
            len = ::read(m_shellProcess.m_handleMaster, nativeBuffer.data(), nativeBuffer.size());
        } while (len < 0 && errno == EINTR);

        if (len > 0) {
            const QByteArray buffer(nativeBuffer.data(), static_cast<qsizetype>(len));
            m_shellReadBuffer.append(buffer);
#ifdef PTYQT_DEBUG
            qDebug().nospace() << "[UnixPTY] [reader] received " << len << " bytes";
#endif
            m_shellProcess.emitReadyRead();
            return;
        }

        if (len == 0) {
            qInfo() << "[UnixPTY] [reader] master reached EOF";
            m_readMasterNotify->setEnabled(false);
            return;
        }

        const int readError = errno;
        if (readError == EAGAIN || readError == EWOULDBLOCK)
            return;

        // Linux commonly reports EIO when the PTY slave has been closed.
        if (readError == EIO) {
            qInfo() << "[UnixPTY] [reader] slave disconnected (EIO)";
        } else {
            qWarning().noquote() << "[UnixPTY] [reader] read failed:"
                                 << QString::fromLocal8Bit(strerror(readError));
        }
        m_readMasterNotify->setEnabled(false);
    });

    QStringList defaultVars;

    defaultVars.append("TERM=xterm-256color");
    defaultVars.append("ITERM_PROFILE=Default");
    defaultVars.append("XPC_FLAGS=0x0");
    defaultVars.append("XPC_SERVICE_NAME=0");
    defaultVars.append("LANG=en_US.UTF-8");
    defaultVars.append("LC_ALL=en_US.UTF-8");
    defaultVars.append("LC_CTYPE=UTF-8");
    defaultVars.append("INIT_CWD=" + QCoreApplication::applicationDirPath());
    defaultVars.append("COMMAND_MODE=unix2003");
    defaultVars.append("COLORTERM=truecolor");

    QStringList varNames;
    foreach (QString line, environment)
    {
        varNames.append(line.split("=").first());
    }

    //append default env vars only if they don't exists in current env
    foreach (QString defVar, defaultVars)
    {
        if (!varNames.contains(defVar.split("=").first()))
            environment.append(defVar);
    }

    QProcessEnvironment envFormat;
    foreach (QString line, environment)
    {
        envFormat.insert(line.split("=").first(), line.split("=").last());
    }
    m_shellProcess.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_shellProcess.setProcessEnvironment(envFormat);
    m_shellProcess.setReadChannel(QProcess::StandardOutput);
    qInfo().noquote() << "[UnixPTY] [lifecycle] launching child:" << m_shellPath;
    m_shellProcess.start(m_shellPath, QStringList());
    if (!m_shellProcess.waitForStarted()) {
        m_lastError = QStringLiteral("Could not start PTY child: %1").arg(m_shellProcess.errorString());
        qWarning().noquote() << "[UnixPTY] [process]" << m_lastError;
        kill();
        return false;
    }

    m_pid = m_shellProcess.processId();
    qInfo().nospace() << "[UnixPTY] [lifecycle] child started; PID=" << m_pid;

    if (!resize(cols, rows)) {
        m_lastError = QStringLiteral("Could not set initial PTY size to %1x%2").arg(cols).arg(rows);
        qWarning().noquote() << "[UnixPTY] [lifecycle]" << m_lastError;
        kill();
        return false;
    }

    return true;
}

bool UnixPtyProcess::resize(qint16 cols, qint16 rows)
{
    if (m_shellProcess.m_handleMaster < 0 || m_shellProcess.m_handleSlave < 0) {
        qWarning() << "[UnixPTY] [resize] PTY descriptors are not open";
        return false;
    }

    struct winsize winp{};
    winp.ws_col = cols;
    winp.ws_row = rows;
    winp.ws_xpixel = 0;
    winp.ws_ypixel = 0;

    if (ioctl(m_shellProcess.m_handleMaster, TIOCSWINSZ, &winp) == -1) {
        qWarning().noquote() << "[UnixPTY] [resize] master ioctl failed:"
                             << QString::fromLocal8Bit(strerror(errno));
        return false;
    }
    if (ioctl(m_shellProcess.m_handleSlave, TIOCSWINSZ, &winp) == -1) {
        qWarning().noquote() << "[UnixPTY] [resize] slave ioctl failed:"
                             << QString::fromLocal8Bit(strerror(errno));
        return false;
    }

    m_size = QPair<qint16, qint16>(cols, rows);
    qInfo().nospace() << "[UnixPTY] [resize] terminal size changed to "
                      << cols << "x" << rows;
    return true;
}

bool UnixPtyProcess::kill()
{
    const bool hasResources = m_shellProcess.m_handleMaster >= 0
        || m_shellProcess.m_handleSlave >= 0
        || m_readMasterNotify
        || m_shellProcess.state() == QProcess::Running;
    if (!hasResources)
        return false;

    qInfo().nospace() << "[UnixPTY] [cleanup] PID " << m_pid << ": begin";

    // Stop notifications first, but keep PTY descriptors open until the child
    // exits. This avoids racing a notifier callback against close().
    if (m_readMasterNotify) {
        m_readMasterNotify->setEnabled(false);
        m_readMasterNotify->disconnect();
        m_readMasterNotify->deleteLater();
        m_readMasterNotify = nullptr;
    }

    if (m_shellProcess.state() == QProcess::Running) {
        qInfo().nospace() << "[UnixPTY] [cleanup] PID " << m_pid << ": sending terminate";
        m_shellProcess.terminate();
        if (!m_shellProcess.waitForFinished(1000)) {
            qWarning().nospace() << "[UnixPTY] [cleanup] PID " << m_pid
                                 << ": graceful termination timed out; sending kill";
            m_shellProcess.kill();
            m_shellProcess.waitForFinished(1000);
        }
    }

    m_shellProcess.m_handleSlaveName.clear();
    if (m_shellProcess.m_handleSlave >= 0) {
        ::close(m_shellProcess.m_handleSlave);
        m_shellProcess.m_handleSlave = -1;
    }
    if (m_shellProcess.m_handleMaster >= 0) {
        ::close(m_shellProcess.m_handleMaster);
        m_shellProcess.m_handleMaster = -1;
    }

    const bool stopped = m_shellProcess.state() == QProcess::NotRunning;
    qInfo().nospace() << "[UnixPTY] [cleanup] PID " << m_pid
                      << ": complete (stopped=" << (stopped ? 1 : 0) << ")";
    m_pid = 0;
    return stopped;
}

IPtyProcess::PtyType UnixPtyProcess::type()
{
    return IPtyProcess::UnixPty;
}

QString UnixPtyProcess::dumpDebugInfo()
{
#ifdef PTYQT_DEBUG
    return QString("PID: %1, In: %2, Out: %3, Type: %4, Cols: %5, Rows: %6, IsRunning: %7, Shell: %8, SlaveName: %9")
            .arg(m_pid).arg(m_shellProcess.m_handleMaster).arg(m_shellProcess.m_handleSlave).arg(type())
            .arg(m_size.first).arg(m_size.second).arg(m_shellProcess.state() == QProcess::Running)
            .arg(m_shellPath).arg(m_shellProcess.m_handleSlaveName);
#else
    return QString("Nothing...");
#endif
}

QIODevice *UnixPtyProcess::notifier()
{
    return &m_shellProcess;
}

QByteArray UnixPtyProcess::readAll()
{
    QByteArray tmpBuffer =  m_shellReadBuffer;
    m_shellReadBuffer.clear();
    return tmpBuffer;
}

qint64 UnixPtyProcess::write(const QByteArray &byteArray)
{
    if (m_shellProcess.m_handleMaster < 0) {
        m_lastError = QStringLiteral("PTY input master is not open");
        qWarning().noquote() << "[UnixPTY] [io]" << m_lastError;
        return -1;
    }

    const ssize_t result = ::write(m_shellProcess.m_handleMaster,
                                   byteArray.constData(), static_cast<size_t>(byteArray.size()));
    if (result < 0) {
        m_lastError = QStringLiteral("PTY write failed: %1").arg(strerror(errno));
        qWarning().noquote() << "[UnixPTY] [io]" << m_lastError;
        return -1;
    }
#ifdef PTYQT_DEBUG
    qDebug().nospace() << "[UnixPTY] [io] wrote " << result << " bytes";
#endif
    return static_cast<qint64>(result);
}

bool UnixPtyProcess::isAvailable()
{
	//todo check something more if required
    return true;
}

bool UnixPtyProcess::isRunning() const
{
    return m_shellProcess.state() == QProcess::Running;
}

void UnixPtyProcess::moveToThread(QThread *targetThread)
{
    m_shellProcess.moveToThread(targetThread);
}

void ShellProcess::setupPtyChildProcess()
{
    dup2(m_handleSlave, STDIN_FILENO);
    dup2(m_handleSlave, STDOUT_FILENO);
    dup2(m_handleSlave, STDERR_FILENO);

    pid_t sid = setsid();
    ioctl(m_handleSlave, TIOCSCTTY, 0);
    tcsetpgrp(m_handleSlave, sid);

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_FREEBSD)
    // on Android imposible to put record to the 'utmp' file
    struct utmpx utmpxInfo;
    memset(&utmpxInfo, 0, sizeof(utmpxInfo));

    strncpy(utmpxInfo.ut_user, qgetenv("USER"), sizeof(utmpxInfo.ut_user));

    QString device(m_handleSlaveName);
    if (device.startsWith("/dev/"))
        device = device.mid(5);

    const char *d = device.toLatin1().constData();

    strncpy(utmpxInfo.ut_line, d, sizeof(utmpxInfo.ut_line));

    strncpy(utmpxInfo.ut_id, d + strlen(d) - sizeof(utmpxInfo.ut_id), sizeof(utmpxInfo.ut_id));

    struct timeval tv;
    gettimeofday(&tv, 0);
    utmpxInfo.ut_tv.tv_sec = tv.tv_sec;
    utmpxInfo.ut_tv.tv_usec = tv.tv_usec;

    utmpxInfo.ut_type = USER_PROCESS;
    utmpxInfo.ut_pid = getpid();

    utmpxname(_PATH_UTMPX);
    setutxent();
    pututxline(&utmpxInfo);
    endutxent();

#if !defined(Q_OS_UNIX)
    updwtmpx(_PATH_UTMPX, &loginInfo);
#endif

#endif
}
