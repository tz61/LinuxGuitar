#include "audio/RtKit.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include "audio/Format.h"
#include "common/I18n.h"

namespace gs {
namespace {

const char* kRtkitName = "org.freedesktop.RealtimeKit1";
const char* kRtkitPath = "/org/freedesktop/RealtimeKit1";

std::atomic<pid_t> g_rtTid{0};

// When the RLIMIT_RTTIME soft limit fires (an RT thread hogged the CPU too long), demote it to a normal
// thread instead of letting the process be killed.
void onSigXcpu(int) {
    pid_t tid = g_rtTid.load();
    if (tid > 0) {
        struct sched_param sp{};
        sched_setscheduler(tid, SCHED_OTHER, &sp);
    }
}

bool getIntProperty(DBusConnection* c, const char* prop, long long& out) {
    DBusMessage* m = dbus_message_new_method_call(kRtkitName, kRtkitPath, "org.freedesktop.DBus.Properties", "Get");
    if (!m) return false;
    const char* iface = kRtkitName;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 2000, &err);
    dbus_message_unref(m);
    if (!r) {
        dbus_error_free(&err);
        return false;
    }
    bool ok = false;
    DBusMessageIter it, sub;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &sub);
        int t = dbus_message_iter_get_arg_type(&sub);
        if (t == DBUS_TYPE_INT32) {
            dbus_int32_t v;
            dbus_message_iter_get_basic(&sub, &v);
            out = v;
            ok = true;
        } else if (t == DBUS_TYPE_INT64) {
            dbus_int64_t v;
            dbus_message_iter_get_basic(&sub, &v);
            out = v;
            ok = true;
        }
    }
    dbus_message_unref(r);
    return ok;
}

bool callRtkit(DBusConnection* c, const char* method, int value, std::string& msg) {
    DBusMessage* m = dbus_message_new_method_call(kRtkitName, kRtkitPath, kRtkitName, method);
    if (!m) return false;
    dbus_uint64_t pid = (dbus_uint64_t)getpid();
    dbus_uint64_t tid = (dbus_uint64_t)syscall(SYS_gettid);
    bool ok;
    if (std::strcmp(method, "MakeThreadRealtimeWithPID") == 0) {
        dbus_uint32_t prio = (dbus_uint32_t)value;
        ok = dbus_message_append_args(m, DBUS_TYPE_UINT64, &pid, DBUS_TYPE_UINT64, &tid, DBUS_TYPE_UINT32, &prio,
                                      DBUS_TYPE_INVALID);
    } else {
        dbus_int32_t nice = (dbus_int32_t)value;
        ok = dbus_message_append_args(m, DBUS_TYPE_UINT64, &pid, DBUS_TYPE_UINT64, &tid, DBUS_TYPE_INT32, &nice,
                                      DBUS_TYPE_INVALID);
    }
    if (!ok) {
        dbus_message_unref(m);
        return false;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 3000, &err);
    dbus_message_unref(m);
    if (!r) {
        msg = strf(tr("rtkit refused: %s"), err.message ? err.message : tr("unknown error"));
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(r);
    return true;
}

}  // namespace

int makeThreadRealtime(int want, std::string& msg) {
    want = std::clamp(want, 1, 99);

    // 1) Direct call (works when the user has an rtprio limit configured)
    struct sched_param sp{};
    sp.sched_priority = want;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
        msg = strf(tr("SCHED_FIFO priority %d (direct)"), want);
        return want;
    }

    // 2) Via rtkit
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (!c) {
        msg = strf(tr("Cannot connect to the system D-Bus: %s"), err.message ? err.message : "");
        dbus_error_free(&err);
        return 0;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);

    long long maxPrio = 20, rttimeMax = 200000;
    getIntProperty(c, "MaxRealtimePriority", maxPrio);
    getIntProperty(c, "RTTimeUSecMax", rttimeMax);
    int prio = (int)std::min<long long>(want, maxPrio);

    // rtkit requires RLIMIT_RTTIME to be set on the process (hard limit <= RTTimeUSecMax)
    struct rlimit rl{};
    if (getrlimit(RLIMIT_RTTIME, &rl) == 0) {
        rlim_t lim = (rlim_t)rttimeMax;
        if (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > lim) {
            rl.rlim_max = lim;
            rl.rlim_cur = lim * 3 / 4;  // the soft limit fires SIGXCPU first -> demote, never reach the hard-limit SIGKILL
            setrlimit(RLIMIT_RTTIME, &rl);
        }
    }
    struct sigaction sa{};
    sa.sa_handler = onSigXcpu;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGXCPU, &sa, nullptr);

    int got = 0;
    std::string why;
    if (callRtkit(c, "MakeThreadRealtimeWithPID", prio, why)) {
        // Note: don't use pthread_getschedparam (glibc caches stale values); rtkit sets SCHED_RR|SCHED_RESET_ON_FORK
        int policy = sched_getscheduler(0);
        if (policy >= 0) policy &= ~SCHED_RESET_ON_FORK;
        struct sched_param cur{};
        if ((policy == SCHED_FIFO || policy == SCHED_RR) && sched_getparam(0, &cur) == 0) {
            got = cur.sched_priority;
            g_rtTid.store((pid_t)syscall(SYS_gettid));
            msg = strf(tr("%s priority %d (via rtkit)"), policy == SCHED_RR ? "SCHED_RR" : "SCHED_FIFO", got);
        }
    }
    if (got == 0) {
        // 3) Fallback: raise the nice level
        long long minNice = -15;
        getIntProperty(c, "MinNiceLevel", minNice);
        std::string why2;
        bool niceOk = callRtkit(c, "MakeThreadHighPriorityWithPID", (int)minNice, why2);
        msg = strf(tr("No real-time priority (%s)"), why.c_str()) + (niceOk ? strf(tr(", nice raised to %d"), (int)minNice) : "");
    }
    dbus_connection_close(c);
    dbus_connection_unref(c);
    return got;
}

}  // namespace gs
