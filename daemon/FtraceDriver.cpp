/* Copyright (C) 2014-2021 by Arm Limited. All rights reserved. */

#include "FtraceDriver.h"

#include "Config.h"
#include "DynBuf.h"
#include "Logging.h"
#include "PrimarySourceProvider.h"
#include "SessionData.h"
#include "Tracepoints.h"
#include "lib/FileDescriptor.h"
#include "lib/Utils.h"
#include "linux/perf/IPerfAttrsConsumer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <regex.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

Barrier::Barrier() : mMutex(), mCond(), mCount(0)
{
    pthread_mutex_init(&mMutex, nullptr);
    pthread_cond_init(&mCond, nullptr);
}

Barrier::~Barrier()
{
    pthread_cond_destroy(&mCond);
    pthread_mutex_destroy(&mMutex);
}

void Barrier::init(unsigned int count)
{
    mCount = count;
}

void Barrier::wait()
{
    pthread_mutex_lock(&mMutex);

    mCount--;
    if (mCount == 0) {
        pthread_cond_broadcast(&mCond);
    }
    else {
        // Loop in case of spurious wakeups
        for (;;) {
            pthread_cond_wait(&mCond, &mMutex);
            if (mCount <= 0) {
                break;
            }
        }
    }

    pthread_mutex_unlock(&mMutex);
}

class FtraceCounter : public DriverCounter {
public:
    FtraceCounter(DriverCounter * next,
                  const TraceFsConstants & traceFsConstants,
                  const char * name,
                  const char * enable);
    ~FtraceCounter() override;

    // Intentionally unimplemented
    FtraceCounter(const FtraceCounter &) = delete;
    FtraceCounter & operator=(const FtraceCounter &) = delete;
    FtraceCounter(FtraceCounter &&) = delete;
    FtraceCounter & operator=(FtraceCounter &&) = delete;

    bool readTracepointFormat(IPerfAttrsConsumer & attrsConsumer);

    void prepare();
    void stop();

private:
    const TraceFsConstants & traceFsConstants;
    char * const mEnable;
    int mWasEnabled;
};

FtraceCounter::FtraceCounter(DriverCounter * next,
                             const TraceFsConstants & traceFsConstants,
                             const char * name,
                             const char * enable)
    : DriverCounter(next, name),
      traceFsConstants(traceFsConstants),
      mEnable(enable == nullptr ? nullptr : strdup(enable)),
      mWasEnabled(0)
{
}

FtraceCounter::~FtraceCounter()
{
    if (mEnable != nullptr) {
        free(mEnable);
    }
}

void FtraceCounter::prepare()
{
    if (mEnable == nullptr) {
        if (gSessionData.mFtraceRaw) {
            LOG_ERROR("The ftrace counter %s is not compatible with the more efficient ftrace collection as it is "
                      "missing the enable attribute. Please either add the enable attribute to the counter in "
                      "events XML or disable the counter in counter configuration.",
                      getName());
            handleException();
        }
        return;
    }

    char buf[1 << 10];
    snprintf(buf, sizeof(buf), "%s/%s/enable", traceFsConstants.path__events, mEnable);
    if ((lib::readIntFromFile(buf, mWasEnabled) != 0) || (lib::writeIntToFile(buf, 1) != 0)) {
        LOG_ERROR("Unable to read or write to %s", buf);
        handleException();
    }
}

void FtraceCounter::stop()
{
    if (mEnable == nullptr) {
        return;
    }

    char buf[1 << 10];
    snprintf(buf, sizeof(buf), "%s/%s/enable", traceFsConstants.path__events, mEnable);
    lib::writeIntToFile(buf, mWasEnabled);
}

bool FtraceCounter::readTracepointFormat(IPerfAttrsConsumer & attrsConsumer)
{
    return ::readTracepointFormat(attrsConsumer, traceFsConstants.path__events, mEnable);
}

static void handlerUsr1(int signum)
{
    (void) signum;

    // Although this signal handler does nothing, SIG_IGN doesn't interrupt splice in all cases
}

static ssize_t pageSize;

class FtraceReader {
public:
    FtraceReader(Barrier * const barrier, int cpu, int tfd, int pfd0, int pfd1)
        : mNext(mHead), mBarrier(barrier), mThread(), mCpu(cpu), mTfd(tfd), mPfd0(pfd0), mPfd1(pfd1)
    {
        mHead = this;
    }

    void start();
    bool interrupt();
    [[nodiscard]] bool join() const;

    static FtraceReader * getHead() { return mHead; }
    [[nodiscard]] FtraceReader * getNext() const { return mNext; }
    [[nodiscard]] int getPfd0() const { return mPfd0; }

private:
    static constexpr auto FTRACE_TIMEOUT = std::chrono::seconds {2};
    static FtraceReader * mHead;
    FtraceReader * const mNext;
    Barrier * const mBarrier;
    pthread_t mThread;
    const int mCpu;
    const int mTfd;
    const int mPfd0;
    const int mPfd1;
    std::atomic_bool mSessionIsActive {true};

    static void * runStatic(void * arg);
    void run();
};

FtraceReader * FtraceReader::mHead;

void FtraceReader::start()
{
    if (pthread_create(&mThread, nullptr, runStatic, this) != 0) {
        LOG_ERROR("Unable to start the ftraceReader thread");
        handleException();
    }
}

bool FtraceReader::interrupt()
{
    mSessionIsActive = false;
    return pthread_kill(mThread, SIGUSR1) == 0;
}

bool FtraceReader::join() const
{
    return pthread_join(mThread, nullptr) == 0;
}

void * FtraceReader::runStatic(void * arg)
{
    auto * const ftraceReader = static_cast<FtraceReader *>(arg);
    ftraceReader->run();
    return nullptr;
}

#ifndef SPLICE_F_MOVE

#include <sys/syscall.h>

// Pre Android-21 does not define splice
#define SPLICE_F_MOVE 1

static ssize_t sys_splice(int fd_in, loff_t * off_in, int fd_out, loff_t * off_out, size_t len, unsigned int flags)
{
    return syscall(__NR_splice, fd_in, off_in, fd_out, off_out, len, flags);
}

#define splice(fd_in, off_in, fd_out, off_out, len, flags) sys_splice(fd_in, off_in, fd_out, off_out, len, flags)

#endif

void FtraceReader::run()
{
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "gatord-reader%02i", mCpu);
        prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(&buf), 0, 0, 0);
    }

    // Gator runs at a high priority, reset the priority to the default
    if (setpriority(PRIO_PROCESS, syscall(__NR_gettid), 0) == -1) {
        LOG_ERROR("setpriority failed");
        handleException();
    }

    mBarrier->wait();

    while (mSessionIsActive) {
        const ssize_t bytes = splice(mTfd, nullptr, mPfd1, nullptr, pageSize, SPLICE_F_MOVE);
        if (bytes == 0) {
            LOG_ERROR("ftrace splice unexpectedly returned 0");
            handleException();
        }
        else if (bytes < 0) {
            if (errno != EINTR) {
                LOG_ERROR("splice failed");
                handleException();
            }
        }
        else {
            // Can there be a short splice read?
            if (bytes != pageSize) {
                LOG_ERROR("splice short read");
                handleException();
            }
            // Will be read by gatord-external
        }
    }

    if (!lib::setNonblock(mTfd)) {
        LOG_ERROR("lib::setNonblock failed");
        handleException();
    }

    // Starting timer to interrupt thread if it's hanging
    std::shared_ptr<std::atomic<bool>> isStuck = std::make_shared<std::atomic<bool>>(true);
    std::thread timeoutThread([&, isStuck]() {
        std::this_thread::sleep_for(FtraceReader::FTRACE_TIMEOUT);
        if (*isStuck) {
            LOG_DEBUG("ftrace reader is hanging. Interrupting reader thread");
            close(mTfd);
            close(mPfd1);
            pthread_kill(mThread, SIGKILL);
        }
    });
    timeoutThread.detach();

    for (;;) {
        ssize_t bytes;

        bytes = splice(mTfd, nullptr, mPfd1, nullptr, pageSize, SPLICE_F_MOVE);
        if (bytes <= 0) {
            break;
        }
        // Can there be a short splice read?
        if (bytes != pageSize) {
            LOG_ERROR("splice short read");
            handleException();
        }
        // Will be read by gatord-external
    }

    {
        // Read any slop
        ssize_t bytes;
        size_t size;
        char buf[1 << 16];

        if (sizeof(buf) < static_cast<size_t>(pageSize)) {
            LOG_ERROR("ftrace slop buffer is too small");
            handleException();
        }
        for (;;) {
            bytes = read(mTfd, buf, sizeof(buf));
            if (bytes == 0) {
                LOG_ERROR("ftrace read unexpectedly returned 0");
                handleException();
            }
            else if (bytes < 0) {
                if (errno != EAGAIN) {
                    LOG_ERROR("reading slop from ftrace failed");
                    handleException();
                }
                break;
            }
            else {
                size = bytes;
                bytes = write(mPfd1, buf, size);
                if (bytes != static_cast<ssize_t>(size)) {
                    LOG_ERROR("writing slop to ftrace pipe failed");
                    handleException();
                }
            }
        }
    }

    // Disabling the timeout thread
    *isStuck = false;

    close(mTfd);
    close(mPfd1);
    // Intentionally don't close mPfd0 as it is used after this thread is exited to read the slop
}

FtraceDriver::FtraceDriver(const TraceFsConstants & traceFsConstants, bool useForTracepoints, size_t numberOfCores)
    : SimpleDriver("Ftrace"),
      traceFsConstants(traceFsConstants),
      mBarrier(),
      mTracingOn(0),
      mSupported(false),
      mMonotonicRawSupport(false),
      mUseForTracepoints(useForTracepoints),
      mNumberOfCores(numberOfCores)
{
}

void FtraceDriver::readEvents(mxml_node_t * const xml)
{
    // Check the kernel version
    struct utsname utsname;
    if (uname(&utsname) != 0) {
        LOG_ERROR("uname failed");
        handleException();
    }

    // The perf clock was added in 3.10
    const int kernelVersion = lib::parseLinuxVersion(utsname);
    if (kernelVersion < KERNEL_VERSION(3, 10, 0)) {
        mSupported = false;
        LOG_SETUP(
            "Ftrace is disabled\nFor full ftrace functionality please upgrade to Linux 3.10 or later. With user space "
            "gator and Linux prior to 3.10, ftrace counters with the tracepoint and arg attributes will be available.");
        return;
    }
    mMonotonicRawSupport = kernelVersion >= KERNEL_VERSION(4, 2, 0);

    // Is debugfs or tracefs available?
    if (access(traceFsConstants.path, R_OK) != 0) {
        mSupported = false;
        LOG_SETUP("Ftrace is disabled\nUnable to locate the tracing directory");
        return;
    }

    if (geteuid() != 0) {
        mSupported = false;
        LOG_SETUP("Ftrace is disabled\nFtrace is not supported when running non-root");
        return;
    }

    mSupported = true;

    mxml_node_t * node = xml;
    while (true) {
        node = mxmlFindElement(node, xml, "event", nullptr, nullptr, MXML_DESCEND);
        if (node == nullptr) {
            break;
        }
        const char * counter = mxmlElementGetAttr(node, "counter");
        if (counter == nullptr) {
            continue;
        }

        if (strncmp(counter, "ftrace_", 7) != 0) {
            continue;
        }

        const char * regex = mxmlElementGetAttr(node, "regex");
        if (regex == nullptr) {
            LOG_ERROR("The regex counter %s is missing the required regex attribute", counter);
            handleException();
        }

        const char * tracepoint = mxmlElementGetAttr(node, "tracepoint");
        const char * enable = mxmlElementGetAttr(node, "enable");
        if (enable == nullptr) {
            enable = tracepoint;
        }
        if (!mUseForTracepoints && tracepoint != nullptr) {
            LOG_DEBUG("Not using ftrace for counter %s", counter);
            continue;
        }
        if (enable != nullptr) {
            char buf[1 << 10];
            snprintf(buf, sizeof(buf), "%s/%s/enable", traceFsConstants.path__events, enable);
            if (access(buf, W_OK) != 0) {
                LOG_SETUP("%s is disabled\n%s was not found", counter, buf);
                continue;
            }
        }

        LOG_DEBUG("Using ftrace for %s", counter);
        setCounters(new FtraceCounter(getCounters(), traceFsConstants, counter, enable));
    }
}

std::pair<std::vector<int>, bool> FtraceDriver::prepare()
{
    if (gSessionData.mFtraceRaw) {
        // Don't want the performace impact of sending all formats so gator only sends it for the enabled counters. This means other counters need to be disabled
        if (lib::writeCStringToFile(traceFsConstants.path__events__enable, "0") != 0) {
            LOG_ERROR("Unable to turn off all events");
            handleException();
        }
    }

    for (auto * counter = static_cast<FtraceCounter *>(getCounters()); counter != nullptr;
         counter = static_cast<FtraceCounter *>(counter->getNext())) {
        if (!counter->isEnabled()) {
            continue;
        }
        counter->prepare();
    }

    if (lib::readIntFromFile(traceFsConstants.path__tracing_on, mTracingOn) != 0) {
        LOG_ERROR("Unable to read if ftrace is enabled");
        handleException();
    }

    if (lib::writeCStringToFile(traceFsConstants.path__tracing_on, "0") != 0) {
        LOG_ERROR("Unable to turn ftrace off before truncating the buffer");
        handleException();
    }

    {
        int fd;
        // The below call can be slow on loaded high-core count systems.
        fd = open(traceFsConstants.path__trace, O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd < 0) {
            LOG_ERROR("Unable truncate ftrace buffer: %s", strerror(errno));
            handleException();
        }
        close(fd);
    }

    const char * const clock = mMonotonicRawSupport ? "mono_raw" : "perf";
    const char * const clock_selected = mMonotonicRawSupport ? "[mono_raw]" : "[perf]";
    const size_t max_trace_clock_file_length = 200;
    ssize_t trace_clock_file_length;
    char trace_clock_file_content[max_trace_clock_file_length + 1] = {0};
    bool must_switch_clock = true;
    // Only write to /trace_clock if the clock actually needs changing,
    // as changing trace_clock can be extremely expensive, especially on large
    // core count systems. The idea is that hopefully only on the first
    // capture, the trace clock needs to be changed. On subsequent captures,
    // the right clock is already being used.
    int fd = open(traceFsConstants.path__trace_clock, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("Couldn't open %s", traceFsConstants.path__trace_clock);
        handleException();
    }
    if ((trace_clock_file_length = ::read(fd, trace_clock_file_content, max_trace_clock_file_length - 1)) < 0) {
        LOG_ERROR("Couldn't read from %s", traceFsConstants.path__trace_clock);
        close(fd);
        handleException();
    }
    close(fd);
    trace_clock_file_content[trace_clock_file_length] = 0;
    if (::strstr(trace_clock_file_content, clock_selected) != nullptr) {
        // the right clock was already selected :)
        must_switch_clock = false;
    }

    // Writing to trace_clock can be very slow on loaded high core count
    // systems.
    if (must_switch_clock && lib::writeCStringToFile(traceFsConstants.path__trace_clock, clock) != 0) {
        LOG_ERROR("Unable to switch ftrace to the %s clock, please ensure you are running Linux %s or later",
                  clock,
                  mMonotonicRawSupport ? "4.2" : "3.10");
        handleException();
    }

    if (!gSessionData.mFtraceRaw) {
        const int fd = open(traceFsConstants.path__trace_pipe, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            LOG_ERROR("Unable to open trace_pipe");
            handleException();
        }
        return {{fd}, true};
    }

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handlerUsr1;
    if (sigaction(SIGUSR1, &act, nullptr) != 0) {
        LOG_ERROR("sigaction failed");
        handleException();
    }

    pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        LOG_ERROR("sysconf PAGESIZE failed");
        handleException();
    }

    mBarrier.init(mNumberOfCores + 1);

    std::pair<std::vector<int>, bool> result {{}, false};
    for (size_t cpu = 0; cpu < mNumberOfCores; ++cpu) {
        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC) != 0) {
            LOG_ERROR("pipe2 failed, %s (%i)", strerror(errno), errno);
            handleException();
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "%s/per_cpu/cpu%zu/trace_pipe_raw", traceFsConstants.path, cpu);
        const int tfd = open(buf, O_RDONLY | O_CLOEXEC);
        (new FtraceReader(&mBarrier, cpu, tfd, pfd[0], pfd[1]))->start();
        result.first.push_back(pfd[0]);
    }

    return result;
}

void FtraceDriver::start()
{
    if (lib::writeCStringToFile(traceFsConstants.path__tracing_on, "1") != 0) {
        LOG_ERROR("Unable to turn ftrace on");
        handleException();
    }

    if (gSessionData.mFtraceRaw) {
        mBarrier.wait();
    }
}

std::vector<int> FtraceDriver::stop()
{
    lib::writeIntToFile(traceFsConstants.path__tracing_on, mTracingOn);

    for (auto * counter = static_cast<FtraceCounter *>(getCounters()); counter != nullptr;
         counter = static_cast<FtraceCounter *>(counter->getNext())) {
        if (!counter->isEnabled()) {
            continue;
        }
        counter->stop();
    }

    std::vector<int> fds;
    if (gSessionData.mFtraceRaw) {
        for (FtraceReader * reader = FtraceReader::getHead(); reader != nullptr; reader = reader->getNext()) {
            reader->interrupt();
            fds.push_back(reader->getPfd0());
        }
        for (FtraceReader * reader = FtraceReader::getHead(); reader != nullptr; reader = reader->getNext()) {
            if (!reader->join()) {
                LOG_WARNING("Failed to wait for FtraceReader to finish. It's possible the thread has already ended.");
            }
        }
    }
    return fds;
}

bool FtraceDriver::readTracepointFormats(IPerfAttrsConsumer & attrsConsumer, DynBuf * const printb, DynBuf * const b)
{
    if (!gSessionData.mFtraceRaw) {
        return true;
    }

    if (!printb->printf("%s/header_page", traceFsConstants.path__events)) {
        LOG_DEBUG("DynBuf::printf failed");
        return false;
    }
    if (!b->read(printb->getBuf())) {
        LOG_DEBUG("DynBuf::read failed");
        return false;
    }
    attrsConsumer.marshalHeaderPage(b->getBuf());

    if (!printb->printf("%s/header_event", traceFsConstants.path__events)) {
        LOG_DEBUG("DynBuf::printf failed");
        return false;
    }
    if (!b->read(printb->getBuf())) {
        LOG_DEBUG("DynBuf::read failed");
        return false;
    }
    attrsConsumer.marshalHeaderEvent(b->getBuf());

    std::unique_ptr<DIR, int (*)(DIR *)> dir {opendir(traceFsConstants.path__events__ftrace), &closedir};
    if (dir == nullptr) {
        LOG_ERROR("Unable to open events ftrace folder");
        handleException();
    }
    struct dirent * dirent;
    while ((dirent = readdir(dir.get())) != nullptr) {
        if (dirent->d_name[0] == '.' || dirent->d_type != DT_DIR) {
            continue;
        }
        if (!printb->printf("%s/%s/format", traceFsConstants.path__events__ftrace, dirent->d_name)) {
            LOG_DEBUG("DynBuf::printf failed");
            return false;
        }
        if (!b->read(printb->getBuf())) {
            LOG_DEBUG("DynBuf::read failed");
            return false;
        }
        attrsConsumer.marshalFormat(b->getLength(), b->getBuf());
    }

    for (auto * counter = static_cast<FtraceCounter *>(getCounters()); counter != nullptr;
         counter = static_cast<FtraceCounter *>(counter->getNext())) {
        if (!counter->isEnabled()) {
            continue;
        }
        counter->readTracepointFormat(attrsConsumer);
    }

    return true;
}
