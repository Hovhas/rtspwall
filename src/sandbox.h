/*
 * sandbox.h — run untrusted-input work (camera probes) in a forked child
 * without root.
 *
 * `sudo rtspwall probe/add/doctor` must read the root-only config, but the
 * stream itself (RTSP/SDP from the network, libavformat demuxers, and in
 * the fallback path libavcodec's software H.264 decoder) is attacker-
 * controllable input. So the parent reads config and URL as root and every
 * probe runs in a child that, before touching any of that input:
 *
 *   - resets SIGINT/SIGTERM/SIGHUP/SIGQUIT to the default and ignores
 *     SIGPIPE, points stdin/stdout at /dev/null;
 *   - closes every inherited file descriptor except its result pipe and,
 *     for a local file, the file it may read (opened by the parent);
 *   - runs in its own process group;
 *   - drops to the user `rtspwall` (or `nobody`/65534 when that does not
 *     exist, with a notice): setgroups to its primary group only (no
 *     supplementary groups — a probe needs neither video nor render),
 *     setresgid, setresuid, and checks that root cannot be regained
 *     (uid 0 and gid 0 are refused as targets);
 *   - sets PR_SET_NO_NEW_PRIVS and PR_SET_DUMPABLE=0;
 *   - writes one fixed-size result (no pointers) to the pipe and _exit()s.
 *
 * The parent reads exactly that many bytes (anything else is an error),
 * kills the process group of every child still running at the deadline
 * (also one that closed its pipe early), and reaps them all. The
 * caller validates the content.
 *
 * Without root there is nothing to drop; callers then run the work
 * in-process (see probe_urls).
 */
#ifndef SANDBOX_H
#define SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define SANDBOX_USER "rtspwall"

struct sandbox_user {
	uid_t uid;
	gid_t gid;
	char  name[32];
	bool  fallback;          /* SANDBOX_USER missing: nobody/65534 */
};

/* The user the children run as. Looked up once; prints a notice on
 * stderr the first time the nobody fallback is used. */
const struct sandbox_user *sandbox_user(void);

enum sandbox_status {
	SANDBOX_OK,              /* exited 0 with exactly outlen bytes */
	SANDBOX_TIMEOUT,         /* still running at the deadline: killed */
	SANDBOX_CRASHED,         /* died from a signal */
	SANDBOX_NOPRIV,          /* could not drop privileges: did nothing */
	SANDBOX_BADRESULT,       /* exited, but not exactly outlen bytes */
	SANDBOX_NOSTART,         /* pipe/fork failed */
};

struct sandbox_job {
	/* Runs in the child, after the privilege drop. `fd` is the job's fd
	 * (renumbered) or -1. Writes its result to `out` (outlen bytes, zeroed
	 * before the call). */
	void (*fn)(void *ctx, int fd, void *out);
	void  *ctx;
	int    fd;               /* passed to the child, or -1 */
	void  *out;              /* filled from the child on SANDBOX_OK */
	size_t outlen;           /* at most 64 KiB */

	enum sandbox_status status;
	int    signo;            /* SANDBOX_CRASHED: the signal */
	pid_t  pid;              /* the child's PID (for debug output) */
};

/* Runs every job in its own child, all in parallel, and waits for them;
 * children still running after timeout_ms are killed (SIGKILL, with their
 * process group), so this returns after about timeout_ms at most. The
 * calling process must be single-threaded. */
void sandbox_run(struct sandbox_job *jobs, int n, int timeout_ms);

/* The child side of the privilege drop (exported for the debug check):
 * returns 0 when running as u with no way back to root, -1 otherwise. */
int sandbox_drop(const struct sandbox_user *u);

const char *sandbox_status_text(enum sandbox_status s);

#endif
