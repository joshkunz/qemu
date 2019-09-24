/*
 *  Emulation of Linux signals
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include <sys/ucontext.h>
#include <sys/resource.h>

#include "qemu.h"
#include "trace.h"
#include "signal-common.h"

static struct target_sigaction sigact_table[TARGET_NSIG];

static void host_signal_handler(int host_signum, siginfo_t *info,
                                void *puc);

static uint8_t host_to_target_signal_table[_NSIG] = {
    [SIGHUP] = TARGET_SIGHUP,
    [SIGINT] = TARGET_SIGINT,
    [SIGQUIT] = TARGET_SIGQUIT,
    [SIGILL] = TARGET_SIGILL,
    [SIGTRAP] = TARGET_SIGTRAP,
    [SIGABRT] = TARGET_SIGABRT,
/*    [SIGIOT] = TARGET_SIGIOT,*/
    [SIGBUS] = TARGET_SIGBUS,
    [SIGFPE] = TARGET_SIGFPE,
    [SIGKILL] = TARGET_SIGKILL,
    [SIGUSR1] = TARGET_SIGUSR1,
    [SIGSEGV] = TARGET_SIGSEGV,
    [SIGUSR2] = TARGET_SIGUSR2,
    [SIGPIPE] = TARGET_SIGPIPE,
    [SIGALRM] = TARGET_SIGALRM,
    [SIGTERM] = TARGET_SIGTERM,
#ifdef SIGSTKFLT
    [SIGSTKFLT] = TARGET_SIGSTKFLT,
#endif
    [SIGCHLD] = TARGET_SIGCHLD,
    [SIGCONT] = TARGET_SIGCONT,
    [SIGSTOP] = TARGET_SIGSTOP,
    [SIGTSTP] = TARGET_SIGTSTP,
    [SIGTTIN] = TARGET_SIGTTIN,
    [SIGTTOU] = TARGET_SIGTTOU,
    [SIGURG] = TARGET_SIGURG,
    [SIGXCPU] = TARGET_SIGXCPU,
    [SIGXFSZ] = TARGET_SIGXFSZ,
    [SIGVTALRM] = TARGET_SIGVTALRM,
    [SIGPROF] = TARGET_SIGPROF,
    [SIGWINCH] = TARGET_SIGWINCH,
    [SIGIO] = TARGET_SIGIO,
    [SIGPWR] = TARGET_SIGPWR,
    [SIGSYS] = TARGET_SIGSYS,
    /* next signals stay the same */
    /* Nasty hack: Reverse SIGRTMIN and SIGRTMAX to avoid overlap with
       host libpthread signals.  This assumes no one actually uses SIGRTMAX :-/
       To fix this properly we need to do manual signal delivery multiplexed
       over a single host signal.  */
    [__SIGRTMIN] = __SIGRTMAX,
    [__SIGRTMAX] = __SIGRTMIN,
};
static uint8_t target_to_host_signal_table[_NSIG];

int host_to_target_signal(int sig)
{
    if (sig < 0 || sig >= _NSIG)
        return sig;
    return host_to_target_signal_table[sig];
}

int target_to_host_signal(int sig)
{
    if (sig < 0 || sig >= _NSIG)
        return sig;
    return target_to_host_signal_table[sig];
}

static inline void target_sigaddset(target_sigset_t *set, int signum)
{
    signum--;
    abi_ulong mask = (abi_ulong)1 << (signum % TARGET_NSIG_BPW);
    set->sig[signum / TARGET_NSIG_BPW] |= mask;
}

static inline void target_sigdelset(target_sigset_t *set, int signum)
{
    abi_ulong mask = (abi_ulong)1 << (--signum % TARGET_NSIG_BPW);
    set->sig[signum / TARGET_NSIG_BPW] &= ~mask;
}

static inline int target_sigismember(const target_sigset_t *set, int signum)
{
    signum--;
    abi_ulong mask = (abi_ulong)1 << (signum % TARGET_NSIG_BPW);
    return ((set->sig[signum / TARGET_NSIG_BPW] & mask) != 0);
}

static inline void target_sigorset(target_sigset_t *set,
                                   const target_sigset_t *left,
                                   const target_sigset_t *right)
{
    int i;
    for (i = 0; i < TARGET_NSIG_WORDS; i++) {
        set->sig[i] = left->sig[i] | right->sig[i];
    }
}

void host_to_target_sigset_internal(target_sigset_t *d,
                                    const sigset_t *s)
{
    int i;
    target_sigemptyset(d);
    for (i = 1; i <= TARGET_NSIG; i++) {
        if (sigismember(s, i) == 1) {
            target_sigaddset(d, host_to_target_signal(i));
        }
    }
}

void host_to_target_sigset(target_sigset_t *d, const sigset_t *s)
{
    target_sigset_t d1;
    int i;

    host_to_target_sigset_internal(&d1, s);
    for(i = 0;i < TARGET_NSIG_WORDS; i++)
        d->sig[i] = tswapal(d1.sig[i]);
}

void target_to_host_sigset_internal(sigset_t *d,
                                    const target_sigset_t *s)
{
    int i;
    sigemptyset(d);
    for (i = 1; i <= TARGET_NSIG; i++) {
        if (target_sigismember(s, i)) {
            sigaddset(d, target_to_host_signal(i));
        }
    }
}

void target_to_host_sigset(sigset_t *d, const target_sigset_t *s)
{
    target_sigset_t s1;
    int i;

    for(i = 0;i < TARGET_NSIG_WORDS; i++)
        s1.sig[i] = tswapal(s->sig[i]);
    target_to_host_sigset_internal(d, &s1);
}

void host_to_target_old_sigset(abi_ulong *old_sigset,
                               const sigset_t *sigset)
{
    target_sigset_t d;
    host_to_target_sigset(&d, sigset);
    *old_sigset = d.sig[0];
}

void target_to_host_old_sigset(sigset_t *sigset,
                               const abi_ulong *old_sigset)
{
    target_sigset_t d;
    int i;

    d.sig[0] = *old_sigset;
    for(i = 1;i < TARGET_NSIG_WORDS; i++)
        d.sig[i] = 0;
    target_to_host_sigset(sigset, &d);
}

void target_to_abi_ulong_old_sigset(abi_ulong *old_sigset,
                                    const target_sigset_t *target_sigset)
{
    target_sigset_t d;
    tswapal_target_sigset(&d, target_sigset);

    memcpy(old_sigset, &d.sig, sizeof(target_sigset_t));
}

void abi_ulong_to_target_old_sigset(target_sigset_t *target_sigset,
                                    const abi_ulong *old_sigset)
{
    target_sigset_t d;

    memcpy(&d.sig, old_sigset, sizeof(target_sigset_t));
    tswapal_target_sigset(target_sigset, &d);
}

void tswapal_target_sigset(target_sigset_t *d, const target_sigset_t *s)
{
    int i;

    for (i = 0; i < TARGET_NSIG_WORDS; i++) {
        d->sig[i] = tswapal(s->sig[i]);
    }
}

int block_signals(void)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;
    sigset_t set;

    /* It's OK to block everything including SIGSEGV, because we won't
     * run any further guest code before unblocking signals in
     * process_pending_signals().
     */
    sigfillset(&set);
    sigprocmask(SIG_SETMASK, &set, 0);

    return atomic_xchg(&ts->signal_pending, 1);
}

/* Wrapper for sigprocmask function
 * Emulates a sigprocmask in a safe way for the guest. Note that set and oldset
 * are host signal set, not guest ones. Returns -TARGET_ERESTARTSYS if
 * a signal was already pending and the syscall must be restarted, or
 * 0 on success.
 * If set is NULL, this is guaranteed not to fail.
 */
int do_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    if (oldset) {
        *oldset = ts->signal_mask;
    }

    if (set) {
        int i;

        if (block_signals()) {
            return -TARGET_ERESTARTSYS;
        }

        switch (how) {
        case SIG_BLOCK:
            sigorset(&ts->signal_mask, &ts->signal_mask, set);
            break;
        case SIG_UNBLOCK:
            for (i = 1; i <= NSIG; ++i) {
                if (sigismember(set, i)) {
                    sigdelset(&ts->signal_mask, i);
                }
            }
            break;
        case SIG_SETMASK:
            ts->signal_mask = *set;
            break;
        default:
            g_assert_not_reached();
        }

        /* Silently ignore attempts to change blocking status of KILL or STOP */
        sigdelset(&ts->signal_mask, SIGKILL);
        sigdelset(&ts->signal_mask, SIGSTOP);
    }
    return 0;
}

#ifdef TRACK_TARGET_SIGMASK
int do_target_sigprocmask(int how, const target_sigset_t *target_set,
                          target_sigset_t *target_oldset,
                          const sigset_t *set, sigset_t *oldset)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    if (target_oldset) {
        *target_oldset = ts->target_signal_mask;
    }
    if (oldset) {
        *oldset = ts->signal_mask;
#ifdef MUX_SIG
        /*
         * The emulation of MUX_SIG being blocked is done using the
         * target_signal_mask, so the status of MUX_SIG is taken from there.
         */
        if (target_sigismember(&ts->target_signal_mask, MUX_SIG) == 1) {
            sigaddset(oldset, MUX_SIG);
        }
#endif
    }

    if (target_set && set) {
        int i;

        if (block_signals()) {
            return -TARGET_ERESTARTSYS;
        }

        switch (how) {
        case SIG_BLOCK:
            target_sigorset(&ts->target_signal_mask, &ts->target_signal_mask,
                            target_set);
            sigorset(&ts->signal_mask, &ts->signal_mask, set);
            break;
        case SIG_UNBLOCK:
            for (i = 1; i <= TARGET_NSIG; ++i) {
                if (target_sigismember(target_set, i) == 1) {
                    target_sigdelset(&ts->target_signal_mask, i);
                }
            }
            for (i = 1; i <= NSIG; ++i) {
                if (sigismember(set, i) == 1) {
                    sigdelset(&ts->signal_mask, i);
                }
            }
            break;
        case SIG_SETMASK:
            ts->target_signal_mask = *target_set;
            ts->signal_mask = *set;
            break;
        default:
            g_assert_not_reached();
        }

        /* Silently ignore attempts to change blocking status of KILL or STOP */
        target_sigdelset(&ts->target_signal_mask, SIGKILL);
        target_sigdelset(&ts->target_signal_mask, SIGSTOP);
        sigdelset(&ts->signal_mask, SIGKILL);
        sigdelset(&ts->signal_mask, SIGSTOP);
#ifdef MUX_SIG
        /*
         * Since MUX_SIG is used for all the target signals out of the host
         * range it must never be blocked on host. The emulation of MUX_SIG
         * being blocked is done using the target_signal_mask. The status
         * of MUX_SIG is taken form the target_signal_mask.
         */
        sigdelset(&ts->signal_mask, MUX_SIG);
#endif
    }
    return 0;
}
#endif

#if !defined(TARGET_NIOS2)
/* Just set the guest's signal mask to the specified value; the
 * caller is assumed to have called block_signals() already.
 */
#ifndef TRACK_TARGET_SIGMASK
void set_sigmask(const sigset_t *set)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    ts->signal_mask = *set;
}
#else
void target_set_sigmask(const sigset_t *set,
                        const target_sigset_t *target_set)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    ts->signal_mask = *set;
    ts->target_signal_mask = *target_set;
#ifdef MUX_SIG
    /* MUX_SIG can't be blocked on host */
    sigdelset(&ts->signal_mask, MUX_SIG);
#endif
}
#endif
#endif

/* sigaltstack management */

int on_sig_stack(unsigned long sp)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    return (sp - ts->sigaltstack_used.ss_sp
            < ts->sigaltstack_used.ss_size);
}

int sas_ss_flags(unsigned long sp)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    return (ts->sigaltstack_used.ss_size == 0 ? SS_DISABLE
            : on_sig_stack(sp) ? SS_ONSTACK : 0);
}

abi_ulong target_sigsp(abi_ulong sp, struct target_sigaction *ka)
{
    /*
     * This is the X/Open sanctioned signal stack switching.
     */
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    if ((ka->sa_flags & TARGET_SA_ONSTACK) && !sas_ss_flags(sp)) {
        return ts->sigaltstack_used.ss_sp + ts->sigaltstack_used.ss_size;
    }
    return sp;
}

void target_save_altstack(target_stack_t *uss, CPUArchState *env)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    __put_user(ts->sigaltstack_used.ss_sp, &uss->ss_sp);
    __put_user(sas_ss_flags(get_sp_from_cpustate(env)), &uss->ss_flags);
    __put_user(ts->sigaltstack_used.ss_size, &uss->ss_size);
}

/* siginfo conversion */

static inline void host_to_target_siginfo_noswap(target_siginfo_t *tinfo,
                                                 const siginfo_t *info)
{
    int sig = host_to_target_signal(info->si_signo);
    int si_code = info->si_code;
    int si_type;
    tinfo->si_signo = sig;
    tinfo->si_errno = 0;
    tinfo->si_code = info->si_code;

    /* This memset serves two purposes:
     * (1) ensure we don't leak random junk to the guest later
     * (2) placate false positives from gcc about fields
     *     being used uninitialized if it chooses to inline both this
     *     function and tswap_siginfo() into host_to_target_siginfo().
     */
    memset(tinfo->_sifields._pad, 0, sizeof(tinfo->_sifields._pad));

    /* This is awkward, because we have to use a combination of
     * the si_code and si_signo to figure out which of the union's
     * members are valid. (Within the host kernel it is always possible
     * to tell, but the kernel carefully avoids giving userspace the
     * high 16 bits of si_code, so we don't have the information to
     * do this the easy way...) We therefore make our best guess,
     * bearing in mind that a guest can spoof most of the si_codes
     * via rt_sigqueueinfo() if it likes.
     *
     * Once we have made our guess, we record it in the top 16 bits of
     * the si_code, so that tswap_siginfo() later can use it.
     * tswap_siginfo() will strip these top bits out before writing
     * si_code to the guest (sign-extending the lower bits).
     */

    switch (si_code) {
    case SI_USER:
    case SI_TKILL:
    case SI_KERNEL:
        /* Sent via kill(), tkill() or tgkill(), or direct from the kernel.
         * These are the only unspoofable si_code values.
         */
        tinfo->_sifields._kill._pid = info->si_pid;
        tinfo->_sifields._kill._uid = info->si_uid;
        si_type = QEMU_SI_KILL;
        break;
    default:
        /* Everything else is spoofable. Make best guess based on signal */
        switch (sig) {
        case TARGET_SIGCHLD:
            tinfo->_sifields._sigchld._pid = info->si_pid;
            tinfo->_sifields._sigchld._uid = info->si_uid;
            tinfo->_sifields._sigchld._status
                = host_to_target_waitstatus(info->si_status);
            tinfo->_sifields._sigchld._utime = info->si_utime;
            tinfo->_sifields._sigchld._stime = info->si_stime;
            si_type = QEMU_SI_CHLD;
            break;
        case TARGET_SIGIO:
            tinfo->_sifields._sigpoll._band = info->si_band;
            tinfo->_sifields._sigpoll._fd = info->si_fd;
            si_type = QEMU_SI_POLL;
            break;
        default:
            /* Assume a sigqueue()/mq_notify()/rt_sigqueueinfo() source. */
            tinfo->_sifields._rt._pid = info->si_pid;
            tinfo->_sifields._rt._uid = info->si_uid;
            /* XXX: potential problem if 64 bit */
            tinfo->_sifields._rt._sigval.sival_ptr
                = (abi_ulong)(unsigned long)info->si_value.sival_ptr;
            si_type = QEMU_SI_RT;
            break;
        }
        break;
    }

    tinfo->si_code = deposit32(si_code, 16, 16, si_type);
}

void tswap_siginfo(target_siginfo_t *tinfo,
                   const target_siginfo_t *info)
{
    int si_type = extract32(info->si_code, 16, 16);
    int si_code = sextract32(info->si_code, 0, 16);

    __put_user(info->si_signo, &tinfo->si_signo);
    __put_user(info->si_errno, &tinfo->si_errno);
    __put_user(si_code, &tinfo->si_code);

    /* We can use our internal marker of which fields in the structure
     * are valid, rather than duplicating the guesswork of
     * host_to_target_siginfo_noswap() here.
     */
    switch (si_type) {
    case QEMU_SI_KILL:
        __put_user(info->_sifields._kill._pid, &tinfo->_sifields._kill._pid);
        __put_user(info->_sifields._kill._uid, &tinfo->_sifields._kill._uid);
        break;
    case QEMU_SI_TIMER:
        __put_user(info->_sifields._timer._timer1,
                   &tinfo->_sifields._timer._timer1);
        __put_user(info->_sifields._timer._timer2,
                   &tinfo->_sifields._timer._timer2);
        break;
    case QEMU_SI_POLL:
        __put_user(info->_sifields._sigpoll._band,
                   &tinfo->_sifields._sigpoll._band);
        __put_user(info->_sifields._sigpoll._fd,
                   &tinfo->_sifields._sigpoll._fd);
        break;
    case QEMU_SI_FAULT:
        __put_user(info->_sifields._sigfault._addr,
                   &tinfo->_sifields._sigfault._addr);
        break;
    case QEMU_SI_CHLD:
        __put_user(info->_sifields._sigchld._pid,
                   &tinfo->_sifields._sigchld._pid);
        __put_user(info->_sifields._sigchld._uid,
                   &tinfo->_sifields._sigchld._uid);
        __put_user(info->_sifields._sigchld._status,
                   &tinfo->_sifields._sigchld._status);
        __put_user(info->_sifields._sigchld._utime,
                   &tinfo->_sifields._sigchld._utime);
        __put_user(info->_sifields._sigchld._stime,
                   &tinfo->_sifields._sigchld._stime);
        break;
    case QEMU_SI_RT:
        __put_user(info->_sifields._rt._pid, &tinfo->_sifields._rt._pid);
        __put_user(info->_sifields._rt._uid, &tinfo->_sifields._rt._uid);
        __put_user(info->_sifields._rt._sigval.sival_ptr,
                   &tinfo->_sifields._rt._sigval.sival_ptr);
        break;
    default:
        g_assert_not_reached();
    }
}

void host_to_target_siginfo(target_siginfo_t *tinfo, const siginfo_t *info)
{
    target_siginfo_t tgt_tmp;
    host_to_target_siginfo_noswap(&tgt_tmp, info);
    tswap_siginfo(tinfo, &tgt_tmp);
}

/* XXX: we support only POSIX RT signals are used. */
/* XXX: find a solution for 64 bit (additional malloced data is needed) */
void target_to_host_siginfo(siginfo_t *info, const target_siginfo_t *tinfo)
{
    /* This conversion is used only for the rt_sigqueueinfo syscall,
     * and so we know that the _rt fields are the valid ones.
     */
    abi_ulong sival_ptr;

    __get_user(info->si_signo, &tinfo->si_signo);
    __get_user(info->si_errno, &tinfo->si_errno);
    __get_user(info->si_code, &tinfo->si_code);
    __get_user(info->si_pid, &tinfo->_sifields._rt._pid);
    __get_user(info->si_uid, &tinfo->_sifields._rt._uid);
    __get_user(sival_ptr, &tinfo->_sifields._rt._sigval.sival_ptr);
    info->si_value.sival_ptr = (void *)(long)sival_ptr;
}

static int fatal_signal (int sig)
{
    switch (sig) {
    case TARGET_SIGCHLD:
    case TARGET_SIGURG:
    case TARGET_SIGWINCH:
        /* Ignored by default.  */
        return 0;
    case TARGET_SIGCONT:
    case TARGET_SIGSTOP:
    case TARGET_SIGTSTP:
    case TARGET_SIGTTIN:
    case TARGET_SIGTTOU:
        /* Job control signals.  */
        return 0;
    default:
        return 1;
    }
}

/* returns 1 if given signal should dump core if not handled */
static int core_dump_signal(int sig)
{
    switch (sig) {
    case TARGET_SIGABRT:
    case TARGET_SIGFPE:
    case TARGET_SIGILL:
    case TARGET_SIGQUIT:
    case TARGET_SIGSEGV:
    case TARGET_SIGTRAP:
    case TARGET_SIGBUS:
        return (1);
    default:
        return (0);
    }
}

void signal_init(void)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;
    struct sigaction act;
    struct sigaction oact;
    int i, j;
    int host_sig;

    // Mark that there are no pending sync signals.
    ts->sync_signal.signal = 0;
    ts->sigqueue_start_scan = 0;
    for (i = 0; i < MAX_SIGQUEUE_SIZE; i++) {
      ts->sigqueue_slots[i].used = false;
    }

    /* generate signal conversion tables */
    for (i = 1; i < _NSIG; i++) {
        if (host_to_target_signal_table[i] == 0)
            host_to_target_signal_table[i] = i;
    }
    for (i = 1; i < _NSIG; i++) {
        j = host_to_target_signal_table[i];
        target_to_host_signal_table[j] = i;
    }

    /* Set the signal mask from the host mask. */
    sigprocmask(0, 0, &ts->signal_mask);
#ifdef TRACK_TARGET_SIGMASK
    host_to_target_sigset_internal(&ts->target_signal_mask, &ts->signal_mask);
#endif

    /* set all host signal handlers. ALL signals are blocked during
       the handlers to serialize them. */
    memset(sigact_table, 0, sizeof(sigact_table));

    sigfillset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = host_signal_handler;
    for (i = 1; i <= TARGET_NSIG; i++) {
#ifdef TARGET_GPROF
        if (i == SIGPROF) {
            continue;
        }
#endif
        QSIMPLEQ_INIT(&ts->sigtab[i - 1].queue);
        ts->sigtab[i - 1].first.used = false;

        host_sig = target_to_host_signal(i);
        sigaction(host_sig, NULL, &oact);
        if (oact.sa_sigaction == (void *)SIG_IGN) {
            sigact_table[i - 1]._sa_handler = TARGET_SIG_IGN;
        } else if (oact.sa_sigaction == (void *)SIG_DFL) {
            sigact_table[i - 1]._sa_handler = TARGET_SIG_DFL;
        }
        /* If there's already a handler installed then something has
           gone horribly wrong, so don't even try to handle that case.  */
        /* Install some handlers for our own use.  We need at least
           SIGSEGV and SIGBUS, to detect exceptions.  We can not just
           trap all signals because it affects syscall interrupt
           behavior.  But do trap all default-fatal signals.  */
        if (fatal_signal (i))
            sigaction(host_sig, &act, NULL);
    }
}

/* Force a synchronously taken signal. The kernel force_sig() function
 * also forces the signal to "not blocked, not ignored", but for QEMU
 * that work is done in process_pending_signals().
 */
void force_sig(int sig)
{
    CPUState *cpu = thread_cpu;
    CPUArchState *env = cpu->env_ptr;
    target_siginfo_t info;

    info.si_signo = sig;
    info.si_errno = 0;
    info.si_code = TARGET_SI_KERNEL;
    info._sifields._kill._pid = 0;
    info._sifields._kill._uid = 0;
    queue_signal(env, info.si_signo, QEMU_SI_KILL, &info);
}

/* Force a SIGSEGV if we couldn't write to memory trying to set
 * up the signal frame. oldsig is the signal we were trying to handle
 * at the point of failure.
 */
#if !defined(TARGET_RISCV)
void force_sigsegv(int oldsig)
{
    if (oldsig == SIGSEGV) {
        /* Make sure we don't try to deliver the signal again; this will
         * end up with handle_pending_signal() calling dump_core_and_abort().
         */
        sigact_table[oldsig - 1]._sa_handler = TARGET_SIG_DFL;
    }
    force_sig(TARGET_SIGSEGV);
}

#endif

/* abort execution with signal */
static void QEMU_NORETURN dump_core_and_abort(int target_sig)
{
    CPUState *cpu = thread_cpu;
    CPUArchState *env = cpu->env_ptr;
    TaskState *ts = (TaskState *)cpu->opaque;
    int host_sig, core_dumped = 0;
    struct sigaction act;

    host_sig = target_to_host_signal(target_sig);
    trace_user_force_sig(env, target_sig, host_sig);
    gdb_signalled(env, target_sig);

    /* dump core if supported by target binary format */
    if (core_dump_signal(target_sig) && (ts->bprm->core_dump != NULL)) {
        stop_all_tasks();
        core_dumped =
            ((*ts->bprm->core_dump)(target_sig, env) == 0);
    }
    if (core_dumped) {
        /* we already dumped the core of target process, we don't want
         * a coredump of qemu itself */
        struct rlimit nodump;
        getrlimit(RLIMIT_CORE, &nodump);
        nodump.rlim_cur=0;
        setrlimit(RLIMIT_CORE, &nodump);
        (void) fprintf(stderr, "qemu: uncaught target signal %d (%s) - %s\n",
            target_sig, strsignal(host_sig), "core dumped" );
    }

    /* The proper exit code for dying from an uncaught signal is
     * -<signal>.  The kernel doesn't allow exit() or _exit() to pass
     * a negative value.  To get the proper exit code we need to
     * actually die from an uncaught signal.  Here the default signal
     * handler is installed, we send ourself a signal and we wait for
     * it to arrive. */
    sigfillset(&act.sa_mask);
    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigaction(host_sig, &act, NULL);

    /* For some reason raise(host_sig) doesn't send the signal when
     * statically linked on x86-64. */
    kill(getpid(), host_sig);

    /* Make sure the signal isn't masked (just reuse the mask inside
    of act) */
    sigdelset(&act.sa_mask, host_sig);
    sigsuspend(&act.sa_mask);

    /* unreachable */
    abort();
}

/* queue a signal so that it will be send to the virtual CPU as soon
   as possible */
int queue_signal(CPUArchState *env, int sig, int si_type,
                 target_siginfo_t *info)
{
    CPUState *cpu = env_cpu(env);
    TaskState *ts = cpu->opaque;

    trace_user_queue_signal(env, sig);

    info->si_code = deposit32(info->si_code, 16, 16, si_type);

    ts->sync_signal.signal = sig;
    ts->sync_signal.event.info = *info;
    /* signal that a new signal is pending */
    atomic_set(&ts->signal_pending, 1);
    return 1; /* indicates that the signal was queued */
}

#ifndef HAVE_SAFE_SYSCALL
static inline void rewind_if_in_safe_syscall(void *puc)
{
    /* Default version: never rewind */
}
#endif

static struct emulated_sigqueue * find_sigqueue_slot(TaskState *ts) {
  int start = ts->sigqueue_start_scan;
  for (int i = 0; i < MAX_SIGQUEUE_SIZE; i++) {
    int slot_idx = (start + i) % MAX_SIGQUEUE_SIZE;
    struct emulated_sigqueue *slot = &ts->sigqueue_slots[slot_idx];
    if (!slot->used) {
      slot->used = true;
      ts->sigqueue_start_scan = (slot_idx + 1) % MAX_SIGQUEUE_SIZE;
      return slot;
    }
  }
  return NULL;
}

static void host_signal_handler(int host_signum, siginfo_t *info,
                                void *puc)
{
    CPUArchState *env = thread_cpu->env_ptr;
    CPUState *cpu = env_cpu(env);
    TaskState *ts = cpu->opaque;

    int sig;
    target_siginfo_t tinfo;
    ucontext_t *uc = puc;
    struct emulated_sigtable *k;

    /* the CPU emulator uses some host signals to detect exceptions,
       we forward to it some signals */
    if ((host_signum == SIGSEGV || host_signum == SIGBUS)
        && info->si_code > 0) {
        if (cpu_signal_handler(host_signum, info, puc))
            return;
    }

    /* get target signal number */
    sig = host_to_target_signal(host_signum);
    if (sig < 1 || sig > TARGET_NSIG)
        return;

#ifdef MUX_SIG
    if (sig == MUX_SIG) {
        /* return the spoofed kill/tgkill signals into standard form */
        if (info->si_code == SIG_SPOOF(SI_USER)) {
            info->si_code = SI_USER;
        } else if (info->si_code == SIG_SPOOF(SI_TKILL)) {
            info->si_code = SI_TKILL;
        }

        /*
         * We assume that si_errno field will remain intact during signal
         * processing on the host. If it changes, the signal will be sent to
         * the wrong number (most likely to MUX_SIG).
         */
        /* get the actual target signal number */
        int target_sig = info->si_errno;
        if (target_sig >= _NSIG && target_sig < TARGET_NSIG) {
            sig = target_sig;
        }
    }
#endif
    trace_user_host_signal(env, host_signum, sig);

    rewind_if_in_safe_syscall(puc);

    host_to_target_siginfo_noswap(&tinfo, info);
    k = &ts->sigtab[sig - 1];

    // No need to worry about atomics here, because we've masked off all
    // other signals, and we're only touching thread-local datastructures.
    struct emulated_sigqueue *item = NULL;
    if (info->si_code == SI_QUEUE) {
      // Either this is the first item we're putting in the queue,
      // or we need to grab a sigqueue_slot from the global sigqueue
      // table.
      if (!k->first.used) {
        item = &k->first;
      } else {
        item = find_sigqueue_slot(ts);
        if (item == NULL) {
          qemu_log("ERROR: dropping signal %d, can't find free sigqueue slot.", sig);
          return;
        }
      }
    } else {
      // If we're not in an SI_QUEUE type event, then free up any
      // queued signals (there should never be more than 1), and
      // reset the queue.
      struct emulated_sigqueue *slot;
      QSIMPLEQ_FOREACH(slot, &k->queue, next) {
        slot->used = false;
      }
      QSIMPLEQ_INIT(&k->queue);
      // k->first can only be used in this signal's queue, and we just
      // free'd all the slots. So either this item wasn't used (queue was
      // empty), or we free'd it.
      assert(!k->first.used);
      item = &k->first;
    }

    item->used = true;
    item->info = tinfo;
    QSIMPLEQ_INSERT_TAIL(&k->queue, &k->first, next);

    ts->signal_pending = 1;

    /* Block host signals until target signal handler entered. We
     * can't block SIGSEGV or SIGBUS while we're executing guest
     * code in case the guest code provokes one in the window between
     * now and it getting out to the main loop. Signals will be
     * unblocked again in process_pending_signals().
     *
     * WARNING: we cannot use sigfillset() here because the uc_sigmask
     * field is a kernel sigset_t, which is much smaller than the
     * libc sigset_t which sigfillset() operates on. Using sigfillset()
     * would write 0xff bytes off the end of the structure and trash
     * data on the struct.
     * We can't use sizeof(uc->uc_sigmask) either, because the libc
     * headers define the struct field with the wrong (too large) type.
     */
    memset(&uc->uc_sigmask, 0xff, SIGSET_T_SIZE);
    sigdelset(&uc->uc_sigmask, SIGSEGV);
    sigdelset(&uc->uc_sigmask, SIGBUS);

    /* interrupt the virtual CPU as soon as possible */
    cpu_exit(thread_cpu);
}

/* do_sigaltstack() returns target values and errnos. */
/* compare linux/kernel/signal.c:do_sigaltstack() */
abi_long do_sigaltstack(abi_ulong uss_addr, abi_ulong uoss_addr, abi_ulong sp)
{
    int ret;
    struct target_sigaltstack oss;
    TaskState *ts = (TaskState *)thread_cpu->opaque;

    /* XXX: test errors */
    if(uoss_addr)
    {
        __put_user(ts->sigaltstack_used.ss_sp, &oss.ss_sp);
        __put_user(ts->sigaltstack_used.ss_size, &oss.ss_size);
        __put_user(sas_ss_flags(sp), &oss.ss_flags);
    }

    if(uss_addr)
    {
        struct target_sigaltstack *uss;
        struct target_sigaltstack ss;
        size_t minstacksize = TARGET_MINSIGSTKSZ;

#if defined(TARGET_PPC64)
        /* ELF V2 for PPC64 has a 4K minimum stack size for signal handlers */
        struct image_info *image = ((TaskState *)thread_cpu->opaque)->info;
        if (get_ppc64_abi(image) > 1) {
            minstacksize = 4096;
        }
#endif

        ret = -TARGET_EFAULT;
        if (!lock_user_struct(VERIFY_READ, uss, uss_addr, 1)) {
            goto out;
        }
        __get_user(ss.ss_sp, &uss->ss_sp);
        __get_user(ss.ss_size, &uss->ss_size);
        __get_user(ss.ss_flags, &uss->ss_flags);
        unlock_user_struct(uss, uss_addr, 0);

        ret = -TARGET_EPERM;
        if (on_sig_stack(sp))
            goto out;

        ret = -TARGET_EINVAL;
        if (ss.ss_flags != TARGET_SS_DISABLE
            && ss.ss_flags != TARGET_SS_ONSTACK
            && ss.ss_flags != 0)
            goto out;

        if (ss.ss_flags == TARGET_SS_DISABLE) {
            ss.ss_size = 0;
            ss.ss_sp = 0;
        } else {
            ret = -TARGET_ENOMEM;
            if (ss.ss_size < minstacksize) {
                goto out;
            }
        }

        ts->sigaltstack_used.ss_sp = ss.ss_sp;
        ts->sigaltstack_used.ss_size = ss.ss_size;
    }

    if (uoss_addr) {
        ret = -TARGET_EFAULT;
        if (copy_to_user(uoss_addr, &oss, sizeof(oss)))
            goto out;
    }

    ret = 0;
out:
    return ret;
}

/* do_sigaction() return target values and host errnos */
int do_sigaction(int sig, const struct target_sigaction *act,
                 struct target_sigaction *oact)
{
    struct target_sigaction *k;
    struct sigaction act1;
    int host_sig;
    int ret = 0;

    if (sig < 1 || sig > TARGET_NSIG || sig == TARGET_SIGKILL || sig == TARGET_SIGSTOP) {
        return -TARGET_EINVAL;
    }

    if (block_signals()) {
        return -TARGET_ERESTARTSYS;
    }

    k = &sigact_table[sig - 1];
    if (oact) {
        __put_user(k->_sa_handler, &oact->_sa_handler);
        __put_user(k->sa_flags, &oact->sa_flags);
#ifdef TARGET_ARCH_HAS_SA_RESTORER
        __put_user(k->sa_restorer, &oact->sa_restorer);
#endif
        /* Not swapped.  */
        oact->sa_mask = k->sa_mask;
    }
    if (act) {
        /* FIXME: This is not threadsafe.  */
        __get_user(k->_sa_handler, &act->_sa_handler);
        __get_user(k->sa_flags, &act->sa_flags);
#ifdef TARGET_ARCH_HAS_SA_RESTORER
        __get_user(k->sa_restorer, &act->sa_restorer);
#endif
        /* To be swapped in target_to_host_sigset.  */
        k->sa_mask = act->sa_mask;

        /* we update the host linux signal state */
        host_sig = target_to_host_signal(sig);
#ifdef MUX_SIG
        /* put the out of host range signal into the multiplex */
        if (sig >= _NSIG && sig < TARGET_NSIG) {
            host_sig = MUX_SIG;
        }
#endif
        if (host_sig != SIGSEGV && host_sig != SIGBUS) {
            sigfillset(&act1.sa_mask);
            act1.sa_flags = SA_SIGINFO;
            if (k->sa_flags & TARGET_SA_RESTART)
                act1.sa_flags |= SA_RESTART;
            /* NOTE: it is important to update the host kernel signal
               ignore state to avoid getting unexpected interrupted
               syscalls */
            if (k->_sa_handler == TARGET_SIG_IGN) {
                act1.sa_sigaction = (void *)SIG_IGN;
            } else if (k->_sa_handler == TARGET_SIG_DFL) {
                if (fatal_signal (sig))
                    act1.sa_sigaction = host_signal_handler;
                else
                    act1.sa_sigaction = (void *)SIG_DFL;
            } else {
                act1.sa_sigaction = host_signal_handler;
            }
            ret = sigaction(host_sig, &act1, NULL);
        }
    }
    return ret;
}

static void handle_pending_signal(CPUArchState *cpu_env, int sig,
                                  struct emulated_sigqueue * event)
{
    CPUState *cpu = env_cpu(cpu_env);
    abi_ulong handler;
    sigset_t set;
    target_sigset_t target_old_set;
#ifdef TRACK_TARGET_SIGMASK
    target_sigset_t target_set;
#endif
    struct target_sigaction *sa;
    TaskState *ts = cpu->opaque;

    trace_user_handle_signal(cpu_env, sig);

    sig = gdb_handlesig(cpu, sig);
    if (!sig) {
        sa = NULL;
        handler = TARGET_SIG_IGN;
    } else {
        sa = &sigact_table[sig - 1];
        handler = sa->_sa_handler;
    }

    if (do_strace) {
        print_taken_signal(sig, &event->info);
    }

    if (handler == TARGET_SIG_DFL) {
        /* default handler : ignore some signal. The other are job control or fatal */
        if (sig == TARGET_SIGTSTP || sig == TARGET_SIGTTIN || sig == TARGET_SIGTTOU) {
            kill(getpid(),SIGSTOP);
        } else if (sig != TARGET_SIGCHLD &&
                   sig != TARGET_SIGURG &&
                   sig != TARGET_SIGWINCH &&
                   sig != TARGET_SIGCONT) {
            dump_core_and_abort(sig);
        }
    } else if (handler == TARGET_SIG_IGN) {
        /* ignore sig */
    } else if (handler == TARGET_SIG_ERR) {
        dump_core_and_abort(sig);
    } else {
        /* compute the blocked signals during the handler execution */
        sigset_t *blocked_set;
#ifdef TRACK_TARGET_SIGMASK
        target_sigset_t *target_blocked_set;

        tswapal_target_sigset(&target_set, &sa->sa_mask);
#endif
        target_to_host_sigset(&set, &sa->sa_mask);
        /* SA_NODEFER indicates that the current signal should not be
           blocked during the handler */
        if (!(sa->sa_flags & TARGET_SA_NODEFER)) {
#ifdef TRACK_TARGET_SIGMASK
            target_sigaddset(&target_set, sig);
#endif
            sigaddset(&set, target_to_host_signal(sig));
        }

        /* save the previous blocked signal state to restore it at the
           end of the signal execution (see do_sigreturn) */
#ifdef TRACK_TARGET_SIGMASK
        target_old_set = ts->target_signal_mask;
#else
        host_to_target_sigset_internal(&target_old_set, &ts->signal_mask);
#endif

        /* block signals in the handler */
        blocked_set = ts->in_sigsuspend ?
            &ts->sigsuspend_mask : &ts->signal_mask;
        sigorset(&ts->signal_mask, blocked_set, &set);
#ifdef TRACK_TARGET_SIGMASK
        target_blocked_set = ts->in_sigsuspend ?
            &ts->target_sigsuspend_mask : &ts->target_signal_mask;
        target_sigorset(&ts->target_signal_mask, target_blocked_set,
                        &target_set);
#endif
        ts->in_sigsuspend = 0;

        /* if the CPU is in VM86 mode, we restore the 32 bit values */
#if defined(TARGET_I386) && !defined(TARGET_X86_64)
        {
            CPUX86State *env = cpu_env;
            if (env->eflags & VM_MASK)
                save_v86_state(env);
        }
#endif
        /* prepare the stack frame of the virtual CPU */
#if defined(TARGET_ARCH_HAS_SETUP_FRAME)
        if (sa->sa_flags & TARGET_SA_SIGINFO) {
            setup_rt_frame(sig, sa, &event->info, &target_old_set, cpu_env);
        } else {
            setup_frame(sig, sa, &target_old_set, cpu_env);
        }
#else
        /* These targets do not have traditional signals.  */
        setup_rt_frame(sig, sa, &event->info, &target_old_set, cpu_env);
#endif
        if (sa->sa_flags & TARGET_SA_RESETHAND) {
            sa->_sa_handler = TARGET_SIG_DFL;
        }
    }
}

void process_pending_signals(CPUArchState *cpu_env)
{
    CPUState *cpu = env_cpu(cpu_env);
    int sig;
    TaskState *ts = cpu->opaque;
    sigset_t set;
#ifdef TRACK_TARGET_SIGMASK
    target_sigset_t *target_blocked_set;
#else
    sigset_t *blocked_set;
#endif

    while (atomic_read(&ts->signal_pending)) {
        /* FIXME: This is not threadsafe.  */
        sigfillset(&set);
        sigprocmask(SIG_SETMASK, &set, 0);

    restart_scan:
        sig = ts->sync_signal.signal;
        if (sig) {
            /* Synchronous signals are forced,
             * see force_sig_info() and callers in Linux
             * Note that not all of our queue_signal() calls in QEMU correspond
             * to force_sig_info() calls in Linux (some are send_sig_info()).
             * However it seems like a kernel bug to me to allow the process
             * to block a synchronous signal since it could then just end up
             * looping round and round indefinitely.
             */
#ifdef TRACK_TARGET_SIGMASK
            if (sigismember(&ts->signal_mask, target_to_host_signal(sig)) == 1
                || target_sigismember(&ts->target_signal_mask, sig) == 1
                || sigact_table[sig - 1]._sa_handler == TARGET_SIG_IGN) {
                sigdelset(&ts->signal_mask, target_to_host_signal(sig));
                target_sigdelset(&ts->target_signal_mask, sig);
                sigact_table[sig - 1]._sa_handler = TARGET_SIG_DFL;
            }
#else
            if (sigismember(&ts->signal_mask, target_to_host_signal_table[sig])
                || sigact_table[sig - 1]._sa_handler == TARGET_SIG_IGN) {
                sigdelset(&ts->signal_mask, target_to_host_signal_table[sig]);
                sigact_table[sig - 1]._sa_handler = TARGET_SIG_DFL;
            }
#endif

            handle_pending_signal(cpu_env, sig, &ts->sync_signal.event);
            // mark the sync signal as completed.
            ts->sync_signal.signal = 0;
            // XXX: Why not restart scan here? Can sync signals generate other
            // sync signals.
        }

        for (sig = 1; sig <= TARGET_NSIG; sig++) {
#ifdef TRACK_TARGET_SIGMASK
            target_blocked_set = ts->in_sigsuspend ?
                &ts->target_sigsuspend_mask : &ts->target_signal_mask;
#else
            blocked_set = ts->in_sigsuspend ?
                &ts->sigsuspend_mask : &ts->signal_mask;
#endif

            struct emulated_sigtable *k = &ts->sigtab[sig - 1];

#ifdef TRACK_TARGET_SIGMASK
            if (QSIMPLEQ_EMPTY(&k->queue) ||
                target_sigismember(target_blocked_set, sig))
              continue;
            if (QSIMPLEQ_EMPTY(&k->queue) ||
                sigismember(blocked_set,
                            target_to_host_signal_table[sig]))
              continue;
#else
            if (QSIMPLEQ_EMPTY(&k->queue) ||
                sigismember(blocked_set,
                            target_to_host_signal_table[sig]))
              continue;
#endif


            // Dequeue and handle a pending signal.
            // Note: we don't need to loop over all signals, because
            // restart_scan will fall back into this loop.
            struct emulated_sigqueue *event = QSIMPLEQ_FIRST(&k->queue);
            QSIMPLEQ_REMOVE_HEAD(&k->queue, next);

            handle_pending_signal(cpu_env, sig, event);

            // Release the sigqueue slot to be used again later.
            event->used = false;

            /* Restart scan from the beginning, as handle_pending_signal
             * might have resulted in a new synchronous signal (eg SIGSEGV).
             */
            goto restart_scan;
        }

        /* if no signal is pending, unblock signals and recheck (the act
         * of unblocking might cause us to take another host signal which
         * will set signal_pending again).
         */
        atomic_set(&ts->signal_pending, 0);
        ts->in_sigsuspend = 0;
        set = ts->signal_mask;
        sigdelset(&set, SIGSEGV);
        sigdelset(&set, SIGBUS);
#ifdef MUX_SIG
        /* MUX_SIG can't be blocked on host */
        sigdelset(&ts->signal_mask, MUX_SIG);
#endif
        sigprocmask(SIG_SETMASK, &set, 0);
    }
    ts->in_sigsuspend = 0;
}
