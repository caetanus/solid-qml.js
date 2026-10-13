#include "termworker.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void signalFd(int fd)
{
    const uint64_t one = 1;
    ssize_t r;
    do {
        r = ::write(fd, &one, sizeof one);
    } while (r < 0 && errno == EINTR);
}

void drainFd(int fd)
{
    uint64_t v;
    while (::read(fd, &v, sizeof v) > 0) {}
}

inline bool isContinuation(unsigned char c) { return (c & 0xc0) == 0x80; }

// Bytes at the end of p[0, len) that start a UTF-8 sequence the buffer does not complete (0-3).
size_t incompleteUtf8Tail(const char *p, size_t len)
{
    size_t back = 0;
    while (back < 3 && back < len && isContinuation(static_cast<unsigned char>(p[len - 1 - back])))
        ++back;
    if (back == len)
        return 0;
    const unsigned char lead = static_cast<unsigned char>(p[len - 1 - back]);
    const size_t need = lead >= 0xf0 && lead < 0xf8 ? 4 : lead >= 0xe0 && lead < 0xf0 ? 3 : lead >= 0xc0 ? 2 : 1;
    return need > back + 1 ? back + 1 : 0;
}

// A slice of n < len bytes of p: moved back (at most 3 bytes) so it does not end inside a sequence.
size_t sliceAtBoundary(const char *p, size_t n)
{
    size_t k = n;
    while (k > n - 3 && k > 1 && isContinuation(static_cast<unsigned char>(p[k])))
        --k;
    return isContinuation(static_cast<unsigned char>(p[k])) ? n : k; // not UTF-8 there: cut anywhere
}

} // namespace

TermWorker::~TermWorker()
{
    stop();
    if (m_wakeFd >= 0)
        ::close(m_wakeFd);
    if (m_guiFd >= 0)
        ::close(m_guiFd);
}

bool TermWorker::start(int masterFd, ScreenMutex *mutex, const TermWorkerSink *sink, void *user, bool startPaused)
{
    if (m_thread.joinable() || masterFd < 0)
        return false;
    m_master = masterFd;
    m_mutex = mutex;
    m_sink = sink;
    m_user = user;
    m_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    m_guiFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeFd < 0 || m_guiFd < 0)
        return false;
    const int fl = ::fcntl(m_master, F_GETFL);
    ::fcntl(m_master, F_SETFL, fl | O_NONBLOCK);
    m_paused.store(startPaused, std::memory_order_relaxed);
    m_buf.resize(kReadSize);
    m_out.reserve(4096);
    m_thread = std::thread([this] { run(); });
    return true;
}

void TermWorker::stop()
{
    if (!m_thread.joinable())
        return;
    m_stop.store(true, std::memory_order_release);
    wake();
    m_thread.join();
}

void TermWorker::wake()
{
    if (m_wakeFd >= 0)
        signalFd(m_wakeFd);
}

void TermWorker::guiAcknowledge()
{
    drainFd(m_guiFd);
    m_posted.store(false, std::memory_order_release);
}

void TermWorker::setPaused(bool paused)
{
    if (m_paused.exchange(paused, std::memory_order_acq_rel) == paused)
        return;
    if (!paused)
        wake(); // parse the carry, poll POLLIN again
}

void TermWorker::queueOutput(const char *data, size_t len)
{
    if (len == 0)
        return;
    if (m_outHead == m_out.size()) {
        m_out.clear();
        m_outHead = 0;
    }
    m_out.insert(m_out.end(), data, data + len);
    flushOutput();
    if (m_outHead != m_out.size() && !m_outPending.exchange(true, std::memory_order_acq_rel))
        wake(); // the worker adds POLLOUT
}

void TermWorker::flushOutput()
{
    while (m_outHead < m_out.size()) {
        const ssize_t n = ::write(m_master, m_out.data() + m_outHead, m_out.size() - m_outHead);
        if (n > 0) {
            m_outHead += size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return; // the slave's input queue is full: the worker retries on POLLOUT
        m_outHead = m_out.size(); // EIO (the shell is gone): drop it
    }
    m_out.clear();
    m_outHead = 0;
    m_outPending.store(false, std::memory_order_release);
}

// Parse what is left of the last read, one slice per lock hold. Stops (keeping the rest) while
// paused — checked under the lock, so once setPaused(true) returns no slice runs.
bool TermWorker::parseCarry()
{
    bool news = false;
    while (m_carryLen > 0) {
        m_mutex->lockWriter();
        if (m_paused.load(std::memory_order_relaxed)) {
            m_mutex->unlock();
            break;
        }
        const size_t n = m_carryLen <= kSlice ? m_carryLen : sliceAtBoundary(m_buf.data() + m_carryOff, kSlice);
        news |= m_sink->feed(m_user, m_buf.data() + m_carryOff, n);
        m_mutex->unlock();
        m_carryOff += n;
        m_carryLen -= n;
        m_mutex->yieldToReaders();
    }
    return news;
}

void TermWorker::postGui()
{
    m_newsPending = false;
    if (m_posted.exchange(true, std::memory_order_acq_rel))
        return; // a wake is in flight: the GUI applies everything recorded up to its apply
    m_lastPostNs = nowNs();
    signalFd(m_guiFd);
}

void TermWorker::run()
{
    ::pthread_setname_np(::pthread_self(), "st-parser"); // top/perf/gdb tell it from the GUI thread
    for (;;) {
        if (m_stop.load(std::memory_order_acquire))
            return;
        const bool paused = m_paused.load(std::memory_order_acquire);
        if (m_carryLen > 0 && !paused)
            m_newsPending |= parseCarry();

        int timeout = -1;
        if (m_newsPending) {
            if (m_posted.load(std::memory_order_acquire)) {
                m_newsPending = false; // the pending wake covers it
            } else {
                const int64_t wait = m_lastPostNs + kGuiIntervalNs - nowNs();
                if (wait <= 0)
                    postGui();
                else
                    timeout = int((wait + 999'999) / 1'000'000);
            }
        }

        pollfd fds[2];
        fds[0] = { m_wakeFd, POLLIN, 0 };
        short ev = 0;
        if (!m_eof && !m_paused.load(std::memory_order_acquire) && m_carryLen == 0)
            ev |= POLLIN;
        if (m_outPending.load(std::memory_order_acquire))
            ev |= POLLOUT;
        // No interest → no fd at all: POLLHUP is reported regardless of `events`, and a hung-up
        // master would spin this loop while paused or after EOF.
        fds[1] = { ev ? m_master : -1, ev, 0 };
        const int r = ::poll(fds, 2, timeout);
        if (r < 0 && errno != EINTR)
            return;
        if (r <= 0)
            continue; // timeout: the throttled post is due (top of the loop)
        if (fds[0].revents & POLLIN)
            drainFd(m_wakeFd);
        if (m_stop.load(std::memory_order_acquire))
            return;
        if (fds[1].revents & POLLOUT) {
            m_mutex->lockWriter();
            flushOutput();
            m_mutex->unlock();
        }
        if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) && !m_eof && m_carryLen == 0) {
            // An incomplete UTF-8 sequence left by the last read goes first, completed by this one.
            if (m_tailLen && m_tailOff)
                std::memmove(m_buf.data(), m_buf.data() + m_tailOff, m_tailLen);
            const size_t held = m_tailLen;
            m_tailOff = m_tailLen = 0;
            const ssize_t n = ::read(m_master, m_buf.data() + held, m_buf.size() - held);
            if (n > 0) {
                if (m_sink->readProbe)
                    m_sink->readProbe(m_user, size_t(n));
                const size_t have = held + size_t(n);
                m_tailLen = incompleteUtf8Tail(m_buf.data(), have);
                m_tailOff = have - m_tailLen;
                m_carryOff = 0;
                m_carryLen = have - m_tailLen;
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                m_eof = true; // EOF/EIO: the shell exited
                m_mutex->lockWriter();
                if (held) // nothing will complete it now
                    m_sink->feed(m_user, m_buf.data(), held);
                m_sink->exited(m_user);
                m_mutex->unlock();
                m_newsPending = true;
            } else {
                m_tailLen = held; // EAGAIN/EINTR: keep it where it now is (the front)
            }
        }
    }
}
