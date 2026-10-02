/*
 * sandbox.c — forked, unprivileged children for untrusted-input work.
 * See sandbox.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sandbox.h"

#define NOBODY_ID     65534
#define EXIT_NOPRIV   125         /* child: privilege drop failed */
#define FD_RESULT     3           /* child: result pipe */
#define FD_JOB        4           /* child: the job's fd */
#define MAX_JOBS      64
#define MAX_OUT       (64 * 1024)
#define REAP_GRACE_MS 250         /* exit time for a child that sent EOF */

const struct sandbox_user *sandbox_user(void)
{
	static struct sandbox_user u;
	static bool done = false;

	if (done)
		return &u;
	done = true;
	struct passwd *pw = getpwnam(SANDBOX_USER);
	if (pw && pw->pw_uid != 0) {
		u.uid = pw->pw_uid;
		u.gid = pw->pw_gid;
		snprintf(u.name, sizeof u.name, "%s", SANDBOX_USER);
		return &u;
	}
	pw = getpwnam("nobody");
	u.uid = pw && pw->pw_uid != 0 ? pw->pw_uid : NOBODY_ID;
	u.gid = pw && pw->pw_uid != 0 ? pw->pw_gid : NOBODY_ID;
	snprintf(u.name, sizeof u.name, "nobody");
	u.fallback = true;
	fprintf(stderr, "rtspwall: note: user '%s' does not exist (is rtspwall installed?); "
			"probing as nobody (uid %u) instead of root\n", SANDBOX_USER,
		(unsigned)u.uid);
	return &u;
}

const char *sandbox_status_text(enum sandbox_status s)
{
	switch (s) {
	case SANDBOX_OK:        return "ok";
	case SANDBOX_TIMEOUT:   return "timed out";
	case SANDBOX_CRASHED:   return "crashed";
	case SANDBOX_NOPRIV:    return "could not drop root privileges";
	case SANDBOX_BADRESULT: return "returned an invalid result";
	case SANDBOX_NOSTART:   return "could not be started";
	}
	return "failed";
}

int sandbox_drop(const struct sandbox_user *u)
{
	/* Neither root's uid nor its group: gid 0 still opens root:root 0640
	 * files and devices. */
	if (u->uid == 0 || u->gid == 0)
		return -1;
	if (setgroups(1, &u->gid) < 0 || setresgid(u->gid, u->gid, u->gid) < 0 ||
	    setresuid(u->uid, u->uid, u->uid) < 0)
		return -1;

	/* Verify: every uid/gid is the target and root is gone for good. */
	uid_t r, e, s;
	gid_t gr, ge, gs;
	if (getresuid(&r, &e, &s) < 0 || r != u->uid || e != u->uid || s != u->uid)
		return -1;
	if (getresgid(&gr, &ge, &gs) < 0 || gr != u->gid || ge != u->gid || gs != u->gid)
		return -1;
	if (setuid(0) == 0 || seteuid(0) == 0)
		return -1;
	gid_t groups[4];
	int ng = getgroups(4, groups);
	if (ng < 0 || ng > 1 || (ng == 1 && groups[0] != u->gid))
		return -1;

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		return -1;
	if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) < 0)
		return -1;
	struct rlimit no_core = { 0, 0 };
	setrlimit(RLIMIT_CORE, &no_core);
	return 0;
}

/* Closes every fd >= from. */
static void close_from(int from)
{
#ifdef SYS_close_range
	if (syscall(SYS_close_range, (unsigned)from, ~0u, 0) == 0)
		return;
#endif
	long max = sysconf(_SC_OPEN_MAX);
	if (max < 0 || max > 65536)
		max = 65536;
	for (int fd = from; fd < max; fd++)
		close(fd);
}

static void write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return;
		p += n;
		len -= (size_t)n;
	}
}

static void child(struct sandbox_job *j, int result_fd, const struct sandbox_user *u)
{
	/* Own process group, so the parent can kill whatever the job started
	 * along with it (kill(-pid)). The parent sets it too (no race). */
	setpgid(0, 0);
	static const int dfl[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGCHLD };
	for (size_t i = 0; i < sizeof dfl / sizeof dfl[0]; i++)
		signal(dfl[i], SIG_DFL);
	signal(SIGPIPE, SIG_IGN);
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);

	/* Renumber: result pipe -> 3, job fd -> 4 (via high numbers first,
	 * so neither overwrites the other), stdin/stdout -> /dev/null. */
	int rfd = fcntl(result_fd, F_DUPFD, 100);
	int jfd = j->fd >= 0 ? fcntl(j->fd, F_DUPFD, 100) : -1;
	if (rfd < 0 || (j->fd >= 0 && jfd < 0))
		_exit(EXIT_NOPRIV);
	int devnull = open("/dev/null", O_RDWR);
	if (devnull < 0)
		_exit(EXIT_NOPRIV);
	dup2(devnull, STDIN_FILENO);
	dup2(devnull, STDOUT_FILENO);
	if (dup2(rfd, FD_RESULT) < 0 || (jfd >= 0 && dup2(jfd, FD_JOB) < 0))
		_exit(EXIT_NOPRIV);
	close_from(jfd >= 0 ? FD_JOB + 1 : FD_RESULT + 1);

	if (sandbox_drop(u) < 0)
		_exit(EXIT_NOPRIV);

	memset(j->out, 0, j->outlen);
	j->fn(j->ctx, jfd >= 0 ? FD_JOB : -1, j->out);
	write_all(FD_RESULT, j->out, j->outlen);
	_exit(0);
}

static int64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sandbox_run(struct sandbox_job *jobs, int n, int timeout_ms)
{
	const struct sandbox_user *u = sandbox_user();
	int rfd[MAX_JOBS];
	size_t got[MAX_JOBS];
	bool overlong[MAX_JOBS];
	static char scratch[MAX_OUT];

	if (n > MAX_JOBS)
		n = MAX_JOBS;
	/* No buffered output may be duplicated into a child. */
	fflush(stdout);
	fflush(stderr);

	for (int i = 0; i < n; i++) {
		struct sandbox_job *j = &jobs[i];
		rfd[i] = -1;
		got[i] = 0;
		overlong[i] = false;
		j->pid = -1;
		j->signo = 0;
		j->status = SANDBOX_NOSTART;
		if (j->outlen == 0 || j->outlen > MAX_OUT)
			continue;
		int p[2];
		if (pipe2(p, O_CLOEXEC) < 0)
			continue;
		pid_t pid = fork();
		if (pid < 0) {
			close(p[0]);
			close(p[1]);
			continue;
		}
		if (pid == 0) {
			close(p[0]);
			child(j, p[1], u);
		}
		setpgid(pid, pid);
		close(p[1]);
		j->pid = pid;
		rfd[i] = p[0];
	}

	/* Collect: each child writes outlen bytes and exits. */
	int64_t deadline = mono_ms() + timeout_ms;
	for (;;) {
		struct pollfd pfd[MAX_JOBS];
		int idx[MAX_JOBS], np = 0;
		for (int i = 0; i < n; i++)
			if (rfd[i] >= 0) {
				pfd[np] = (struct pollfd){ rfd[i], POLLIN, 0 };
				idx[np++] = i;
			}
		if (!np)
			break;
		int left = (int)(deadline - mono_ms());
		if (left <= 0)
			break;
		int r = poll(pfd, (nfds_t)np, left);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		for (int k = 0; k < np; k++) {
			if (!pfd[k].revents)
				continue;
			int i = idx[k];
			struct sandbox_job *j = &jobs[i];
			char *dst = got[i] < j->outlen ? (char *)j->out + got[i] : scratch;
			size_t room = got[i] < j->outlen ? j->outlen - got[i] : sizeof scratch;
			ssize_t m = read(rfd[i], dst, room);
			if (m < 0 && errno == EINTR)
				continue;
			if (m <= 0) {
				close(rfd[i]);
				rfd[i] = -1;
				continue;
			}
			if (got[i] >= j->outlen)
				overlong[i] = true;
			got[i] += (size_t)m;
		}
	}

	/* Reap. A child that closed its result pipe but keeps running (or a
	 * grandchild holding it open) must not block us: wait without blocking
	 * until the deadline (plus a short grace for a child that is just
	 * exiting), then kill its whole process group. The group is killed
	 * while the child is still a zombie (WNOWAIT), so its pid - and with it
	 * the group id - cannot have been reused yet. */
	int64_t reap_deadline = deadline + REAP_GRACE_MS;
	for (int i = 0; i < n; i++) {
		struct sandbox_job *j = &jobs[i];
		if (j->pid <= 0)
			continue;
		bool timed_out = rfd[i] >= 0;
		if (timed_out) {
			kill(-j->pid, SIGKILL);
			kill(j->pid, SIGKILL);
			close(rfd[i]);
			rfd[i] = -1;
		}
		bool killed = timed_out;
		for (;;) {
			siginfo_t si;
			si.si_pid = 0;
			int w = waitid(P_PID, (id_t)j->pid, &si, WEXITED | WNOHANG | WNOWAIT);
			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0 || si.si_pid == j->pid)
				break;                       /* exited (zombie), or gone */
			if (!killed && mono_ms() >= reap_deadline) {
				timed_out = killed = true;   /* then wait for the SIGKILL */
				kill(-j->pid, SIGKILL);
				kill(j->pid, SIGKILL);
			}
			struct timespec ts = { 0, 10 * 1000000L };
			nanosleep(&ts, NULL);
		}
		kill(-j->pid, SIGKILL);              /* leftover grandchildren */
		int st = 0;
		while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR)
			;
		if (timed_out)
			j->status = SANDBOX_TIMEOUT;
		else if (WIFSIGNALED(st)) {
			j->status = SANDBOX_CRASHED;
			j->signo = WTERMSIG(st);
		} else if (WIFEXITED(st) && WEXITSTATUS(st) == EXIT_NOPRIV)
			j->status = SANDBOX_NOPRIV;
		else if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && got[i] == j->outlen && !overlong[i])
			j->status = SANDBOX_OK;
		else
			j->status = SANDBOX_BADRESULT;
	}
}
