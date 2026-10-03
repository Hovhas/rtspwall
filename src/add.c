/*
 * add.c — `rtspwall add [--config PATH] [--force] [--no-systemd] NAME [CELL]`.
 *
 * Adds one camera without the URL ever touching the command line:
 *   1. resolves the config path (realpath: a symlinked config is edited at
 *      its target), refuses a target that is not a regular file or whose
 *      directory other users can write to, and takes an exclusive lock on
 *      <confdir>/.rtspwall.lock for the whole read - probe - write, so two
 *      adds cannot lose each other's camera;
 *   2. reads the URL from a hidden prompt (or stdin when it is not a
 *      terminal), checks it can live in a config line;
 *   3. probes it (and the cameras already configured, for the total
 *      decoder budget; as root in unprivileged children, see sandbox.h);
 *      a FAIL is refused unless --force, and so is a config with 4+
 *      streams of >= 1080p and less gpu_mem active than they need
 *      (vcgencmd; the > 50 % budget rule and an unknown value only warn);
 *   4. appends "NAME|URL|CELL" — the first free cell unless CELL is given —
 *      by writing a 0600 temp file (mkostemp, O_NOFOLLOW) in the config's
 *      directory, checking it with `rtspwall --check-config`, copying mode
 *      and owner (0640 root:rtspwall), and renaming it over the original
 *      only if the original did not change meanwhile (device, inode, size,
 *      mtime). SIGINT/SIGTERM/SIGHUP/SIGQUIT remove the temp file (it
 *      holds the camera password). Nothing else in the file changes;
 *   5. gets the wall running with the new camera (see start_service: when
 *      rtspwall-config.path is active it does the restart, add only fills
 *      in what it does not do).
 *
 * The line logic is pure and tested (cfg_* in clilogic.c).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"

#define CONFIG_MAX_BYTES (1024 * 1024)
#define LOCK_NAME        ".rtspwall.lock"

static void add_usage(FILE *out)
{
	fprintf(out,
		"Usage: rtspwall add [--config PATH] [--force] [--no-systemd] NAME [CELL]\n"
		"\n"
		"Asks for the camera URL (hidden; or reads it from stdin when piped), probes\n"
		"it, adds NAME|URL|CELL to the config (first free cell unless CELL is given;\n"
		"a taken CELL makes a rotation group) and starts or restarts the service.\n"
		"\n"
		"  --config PATH   config file (default " CLI_DEFAULT_CONFIG ")\n"
		"  --force         add the camera even if the probe says FAIL or gpu_mem is\n"
		"                  too low for 4 or more 1080p streams\n"
		"  --no-systemd    only edit the file, do not start/restart the service\n");
}

/* ------------------------------------------------- temp-file cleanup */

/* The temp file holds the camera's password until it is renamed into
 * place: a signal must not leave it behind. */
static char cleanup_path[PATH_MAX];
static volatile sig_atomic_t cleanup_armed = 0;

static void cleanup_signal(int sig)
{
	if (cleanup_armed)
		unlink(cleanup_path);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void cleanup_install(void)
{
	static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
	struct sigaction sa = { .sa_handler = cleanup_signal };
	sigemptyset(&sa.sa_mask);
	for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		sigaddset(&sa.sa_mask, sigs[i]);
	for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		sigaction(sigs[i], &sa, NULL);
}

static void block_signals(bool block, sigset_t *old)
{
	if (block) {
		sigset_t all;
		sigfillset(&all);
		sigprocmask(SIG_BLOCK, &all, old);
	} else {
		sigprocmask(SIG_SETMASK, old, NULL);
	}
}

/* Creates the temp file (0600, O_EXCL + O_NOFOLLOW) and arms the cleanup
 * in one step with the signals blocked: a signal in between would leave
 * the file behind. `tmp` is the mkostemp template. */
static int create_temp(char *tmp)
{
	sigset_t old;
	block_signals(true, &old);
	int fd = mkostemp(tmp, O_CLOEXEC | O_NOFOLLOW);
	if (fd >= 0) {
		snprintf(cleanup_path, sizeof cleanup_path, "%s", tmp);
		cleanup_armed = 1;
	}
	int e = errno;
	block_signals(false, &old);
	errno = e;
	return fd;
}

/* Removes the temp file (if `unlink_it`) and disarms, signals blocked. */
static void cleanup_done(bool unlink_it)
{
	sigset_t old;
	block_signals(true, &old);
	if (unlink_it && cleanup_armed)
		unlink(cleanup_path);
	cleanup_armed = 0;
	block_signals(false, &old);
}

/* ------------------------------------------------- file-system checks */

static void describe(const struct stat *st, char *out, size_t outlen)
{
	struct passwd *p = getpwuid(st->st_uid);
	struct group *g = getgrgid(st->st_gid);
	snprintf(out, outlen, "%04o %s:%s", (unsigned)(st->st_mode & 07777),
		 p ? p->pw_name : "?", g ? g->gr_name : "?");
}

/* Writable by someone other than root (and the user running add)? */
static bool writable_by_others(const struct stat *st)
{
	uid_t me = geteuid();
	gid_t my_gid = getegid();
	if ((st->st_mode & S_IWUSR) && st->st_uid != 0 && st->st_uid != me)
		return true;
	if ((st->st_mode & S_IWGRP) && st->st_gid != 0 && (me == 0 || st->st_gid != my_gid))
		return true;
	return (st->st_mode & S_IWOTH) != 0;
}

static bool same_file_state(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

/* Takes <dir>/.rtspwall.lock (exclusive flock, waits for another add).
 * Returns the fd (kept open until exit), or -1 with a message. */
static int take_lock(const char *dir)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof path, "%s/" LOCK_NAME, dir) >= (int)sizeof path) {
		fprintf(stderr, "rtspwall: path too long: %s\n", dir);
		return -1;
	}
	int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		fprintf(stderr, "rtspwall: cannot create the lock file %s: %s%s\n", path,
			strerror(errno), errno == EACCES ? " (run with sudo)" : "");
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "rtspwall: %s is not a regular file\n", path);
		close(fd);
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		if (errno != EWOULDBLOCK) {
			fprintf(stderr, "rtspwall: cannot lock %s: %s\n", path, strerror(errno));
			close(fd);
			return -1;
		}
		fprintf(stderr, "rtspwall: another `rtspwall add` is editing this config; "
				"waiting for it to finish (lock: %s) ...\n", path);
		while (flock(fd, LOCK_EX) < 0) {
			if (errno != EINTR) {
				fprintf(stderr, "rtspwall: cannot lock %s: %s\n", path, strerror(errno));
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}

/* -------------------------------------------------------------- write */

/* Copies the extended attributes of `path` (the original config) to the
 * temp file `fd` - an ACL (system.posix_acl_access), a SELinux label or
 * user.* attributes set on the config survive the rename. A file system
 * without xattr support (ENOTSUP) is fine; an attribute that cannot be
 * copied is reported but does not stop the add (owner and mode are kept
 * either way). */
static void copy_xattrs(const char *path, int fd)
{
	int src = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
	if (src < 0)
		return;
	char names[4096];
	ssize_t len = flistxattr(src, names, sizeof names);
	if (len < 0) {
		if (errno != ENOTSUP && errno != EOPNOTSUPP)
			fprintf(stderr, "rtspwall: warning: cannot list the extended attributes of "
				"%s: %s\n", path, strerror(errno));
		close(src);
		return;
	}
	for (ssize_t off = 0; off < len; off += (ssize_t)strlen(names + off) + 1) {
		const char *name = names + off;
		char value[8192];
		ssize_t n = fgetxattr(src, name, value, sizeof value);
		if (n < 0 || fsetxattr(fd, name, value, (size_t)n, 0) < 0) {
			if (errno != ENOTSUP && errno != EOPNOTSUPP)
				fprintf(stderr, "rtspwall: warning: cannot keep the extended attribute "
					"%s of %s: %s\n", name, path, strerror(errno));
		}
	}
	close(src);
}

/* Writes `text` next to `path` and renames it over `path`, keeping the
 * original's mode and owner; aborts if `path` no longer matches `orig`.
 * `checked` is called with the temp file's path before the rename; a
 * non-zero return aborts. */
static int write_atomic(const char *path, const char *text, const struct stat *orig,
			int (*checked)(const char *tmp))
{
	char tmp[PATH_MAX];
	if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
		fprintf(stderr, "rtspwall: path too long: %s\n", path);
		return -1;
	}
	/* Mode 0600 from mkostemp: the password is never readable by others,
	 * not even before the fchmod below. */
	int fd = create_temp(tmp);
	if (fd < 0) {
		fprintf(stderr, "rtspwall: cannot create a temp file next to %s: %s\n", path,
			strerror(errno));
		return -1;
	}

	size_t len = strlen(text), off = 0;
	int r = 0;
	while (off < len) {
		ssize_t n = write(fd, text + off, len - off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			fprintf(stderr, "rtspwall: writing %s: %s\n", tmp, strerror(errno));
			r = -1;
			break;
		}
		off += (size_t)n;
	}
	if (!r && (orig->st_uid != geteuid() || orig->st_gid != getegid()) &&
	    fchown(fd, orig->st_uid, orig->st_gid) < 0) {
		fprintf(stderr, "rtspwall: cannot keep the owner of %s (%s); run with sudo\n",
			path, strerror(errno));
		r = -1;
	}
	if (!r && fchmod(fd, orig->st_mode & 07777) < 0) {
		fprintf(stderr, "rtspwall: chmod %s: %s\n", tmp, strerror(errno));
		r = -1;
	}
	/* After the chmod: an ACL also carries the group bits (as its mask),
	 * and copying it sets them back to exactly the original's. */
	if (!r)
		copy_xattrs(path, fd);
	if (!r && fsync(fd) < 0) {
		fprintf(stderr, "rtspwall: fsync %s: %s\n", tmp, strerror(errno));
		r = -1;
	}
	struct stat tst;
	if (!r && fstat(fd, &tst) < 0)
		r = -1;
	close(fd);
	if (!r && checked && checked(tmp) != 0)
		r = -1;

	/* What gets renamed must still be our temp file ... */
	struct stat lst;
	if (!r && (lstat(tmp, &lst) < 0 || !S_ISREG(lst.st_mode) || lst.st_ino != tst.st_ino ||
		   lst.st_dev != tst.st_dev)) {
		fprintf(stderr, "rtspwall: the temp file %s was replaced; nothing changed\n", tmp);
		r = -1;
	}
	/* ... and the config must still be the one that was read and probed. */
	struct stat now;
	if (!r && (lstat(path, &now) < 0 || !S_ISREG(now.st_mode) ||
		   !same_file_state(&now, orig))) {
		fprintf(stderr, "rtspwall: %s was changed by someone else while the camera was "
				"being probed; nothing was written - run add again\n", path);
		r = -1;
	}
	if (!r) {
		/* rename + disarm together: after the rename the name is gone. */
		sigset_t old;
		block_signals(true, &old);
		if (rename(tmp, path) < 0) {
			fprintf(stderr, "rtspwall: rename %s -> %s: %s\n", tmp, path, strerror(errno));
			r = -1;
		} else {
			cleanup_armed = 0;
		}
		block_signals(false, &old);
	}
	if (r) {
		cleanup_done(true);
		return -1;
	}

	/* Make the rename itself durable. */
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", path);
	int dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	return 0;
}

/* Runs `rtspwall --check-config` on the temp file before it replaces the
 * config, so add can never leave a config the service refuses. */
static int check_with_daemon_parser(const char *tmp)
{
	char out[8192], masked[8192];
	char *argv[] = { (char *)cli_self_exe(), "--check-config", (char *)tmp, NULL };
	int r = cli_run(argv, out, sizeof out, 20000);
	if (r == 0)
		return 0;
	struct report_buf rb;
	report_init(&rb);
	report_append_masked(&rb, out);
	snprintf(masked, sizeof masked, "%s", rb.data ? rb.data : "");
	report_free(&rb);
	fprintf(stderr, "rtspwall: the config would not pass --check-config (nothing was "
			"changed):\n%s", masked);
	return -1;
}

/* ------------------------------------------------------------ service */

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* rtspwall-config.path fires on the rename; its service validates the
 * file (after a 2 s debounce) and restarts the wall if it is enabled and
 * running or failed. Waits until that run is over, so add never restarts
 * the wall a second time. Returns true if the watcher ran. */
static bool wait_for_watcher(void)
{
	bool seen = false;
	for (int t = 0; t < 15000; t += 250) {
		char st[64];
		cli_unit_state(CLI_WATCH_SERVICE, st, sizeof st);
		bool busy = strcmp(st, "activating") == 0 || strcmp(st, "active") == 0 ||
			    strcmp(st, "deactivating") == 0;
		if (busy)
			seen = true;
		else if (seen || t >= 2000)
			break;     /* finished, or never started (older unit, no change) */
		sleep_ms(250);
	}
	return seen;
}

/* InvocationID of a unit (a new one per start); "" if it never ran, NULL
 * (out untouched) if systemctl could not tell. */
static const char *unit_invocation(const char *unit, char *out, size_t outlen)
{
	char *argv[] = { "systemctl", "show", "-p", "InvocationID", "--value", (char *)unit,
			 NULL };
	char buf[128];
	if (cli_run(argv, buf, sizeof buf, 10000) != 0)
		return NULL;
	buf[strcspn(buf, "\r\n")] = '\0';
	snprintf(out, outlen, "%s", buf);
	return out;
}

/* inv_before: rtspwall.service's InvocationID read before the rename
 * (NULL if unknown). The watcher only gets the credit for a restart when
 * the ID really changed: its validation may have rejected the file, or a
 * hardening/setpriv failure may have stopped it before the restart. */
static int start_service(const char *inv_before)
{
	if (!cli_have_systemd()) {
		printf("systemd is not running here; start the wall yourself, e.g.: "
		       "sudo rtspwall %s\n", CLI_DEFAULT_CONFIG);
		return 0;
	}
	if (geteuid() != 0) {
		printf("not root: start the service with: sudo systemctl restart rtspwall\n");
		return 0;
	}

	bool was_enabled = cli_unit_enabled(CLI_SERVICE);
	bool watcher = cli_unit_active(CLI_WATCH_PATH);
	if (!was_enabled) {
		if (cli_systemctl("enable", CLI_SERVICE) != 0) {
			fprintf(stderr, "rtspwall: could not enable %s; see: journalctl -u rtspwall "
					"-b\n", CLI_SERVICE);
			return 1;
		}
		printf("enabled %s: the wall now also starts at boot\n"
		       "  (undo: sudo systemctl disable --now rtspwall)\n", CLI_SERVICE);
	}

	/* The watcher only acts on an enabled wall (it was checked at the
	 * moment of the rename, so an enable just now does not count). */
	bool watcher_acted = false;
	if (watcher) {
		printf("waiting for %s to pick up the change ...\n", CLI_WATCH_PATH);
		bool ran = wait_for_watcher();
		char inv_after[128];
		watcher_acted = ran && was_enabled &&
				unit_restarted(inv_before, unit_invocation(CLI_SERVICE, inv_after,
									   sizeof inv_after));
		if (ran && was_enabled && !watcher_acted)
			printf("%s ran but did not restart %s (its check of the config failed or "
			       "it was blocked; see: journalctl -u %s -b); doing it now\n",
			       CLI_WATCH_PATH, CLI_SERVICE, CLI_WATCH_SERVICE);
	}

	char state[64];
	cli_unit_state(CLI_SERVICE, state, sizeof state);
	bool running = strcmp(state, "active") == 0 || strcmp(state, "activating") == 0 ||
		       strcmp(state, "reloading") == 0;
	int rc = 0;
	if (running && watcher_acted) {
		printf("%s checked the new config and restarted %s\n", CLI_WATCH_PATH, CLI_SERVICE);
	} else if (running) {
		if (cli_systemctl("restart", CLI_SERVICE) != 0)
			rc = 1;
		else
			printf("restarted %s\n", CLI_SERVICE);
	} else {
		if (strcmp(state, "failed") == 0) {
			cli_systemctl("reset-failed", CLI_SERVICE);
			printf("%s had failed: cleared the failed state\n", CLI_SERVICE);
		}
		if (cli_systemctl("start", CLI_SERVICE) != 0)
			rc = 1;
		else
			printf("started %s\n", CLI_SERVICE);
	}
	if (rc) {
		fprintf(stderr, "rtspwall: could not start %s; see: journalctl -u rtspwall -b\n",
			CLI_SERVICE);
		return 1;
	}
	printf("status:   systemctl status rtspwall    log: journalctl -u rtspwall -f\n");
	return 0;
}

/* ------------------------------------------------------------- command */

int cmd_add(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "config",     required_argument, NULL, 'c' },
		{ "force",      no_argument,       NULL, 'f' },
		{ "no-systemd", no_argument,       NULL, 'n' },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *config = CLI_DEFAULT_CONFIG;
	bool force = false, no_systemd = false;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "c:fnh", opts, NULL)) != -1) {
		switch (opt) {
		case 'c': config = optarg; break;
		case 'f': force = true; break;
		case 'n': no_systemd = true; break;
		case 'h': add_usage(stdout); return 0;
		default:  add_usage(stderr); return 2;
		}
	}
	if (argc - optind < 1 || argc - optind > 2) {
		add_usage(stderr);
		return 2;
	}
	const char *name = argv[optind];
	int cell = 0;
	if (argc - optind == 2) {
		char *end;
		long c = strtol(argv[optind + 1], &end, 10);
		if (!*argv[optind + 1] || *end || c < 1 || c > 64) {
			fprintf(stderr, "rtspwall: CELL must be a number from 1, got \"%s\"\n",
				argv[optind + 1]);
			return 2;
		}
		cell = (int)c;
	}

	const char *why = cfg_check_name(name);
	if (why) {
		fprintf(stderr, "rtspwall: invalid NAME \"%s\": %s\n", name, why);
		return 2;
	}
	cleanup_install();

	/* ---- where the config really is, and may we write there */
	char real[PATH_MAX];
	if (!realpath(config, real)) {
		fprintf(stderr, "rtspwall: %s: %s (is rtspwall installed? or use --config PATH)\n",
			config, strerror(errno));
		return 2;
	}
	if (strcmp(real, config) != 0) {
		struct stat l;
		if (lstat(config, &l) == 0 && S_ISLNK(l.st_mode))
			printf("note:     %s is a symlink; editing its target %s\n", config, real);
	}
	char dirbuf[PATH_MAX];
	snprintf(dirbuf, sizeof dirbuf, "%s", real);
	const char *dir = dirname(dirbuf);
	struct stat dst;
	if (stat(dir, &dst) < 0) {
		fprintf(stderr, "rtspwall: %s: %s\n", dir, strerror(errno));
		return 2;
	}
	if (writable_by_others(&dst)) {
		char m[96];
		describe(&dst, m, sizeof m);
		fprintf(stderr, "rtspwall: refusing to edit %s: its directory %s (%s) is writable "
				"by other users, who could replace the config or the temp file (with "
				"the camera password) before it is renamed into place.\nFix: sudo chown root:root %s && sudo chmod "
				"go-w %s\n", real, dir, m, dir, dir);
		return 2;
	}

	int lock_fd = take_lock(dir);
	if (lock_fd < 0)
		return 2;

	/* ---- the config as it is (under the lock) */
	int cfd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	struct stat st;
	if (cfd < 0 || fstat(cfd, &st) < 0) {
		fprintf(stderr, "rtspwall: cannot read %s: %s%s\n", real, strerror(errno),
			errno == EACCES ? " (run with sudo)" : "");
		if (cfd >= 0)
			close(cfd);
		close(lock_fd);
		return 2;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "rtspwall: %s is not a regular file\n", real);
		close(cfd);
		close(lock_fd);
		return 2;
	}
	char *text = cli_read_fd(cfd, CONFIG_MAX_BYTES);
	int read_errno = errno;
	close(cfd);
	if (!text) {
		fprintf(stderr, "rtspwall: cannot read %s: %s\n", real, strerror(read_errno));
		close(lock_fd);
		return 2;
	}

	struct cfg_scan scan;
	cfg_scan_text(text, name, &scan);
	int ret = 2;
	if (scan.manual_mode) {
		fprintf(stderr, "rtspwall: %s uses manual tiles (name|url|w|h|x|y); add only "
				"handles GRID layouts - edit the file by hand\n", config);
		goto out;
	}
	if (!scan.grid_cols) {
		fprintf(stderr, "rtspwall: %s has no GRID= line; add one first (e.g. GRID=2x2)\n",
			config);
		goto out;
	}
	if (scan.name_taken) {
		fprintf(stderr, "rtspwall: a camera named \"%s\" already exists in %s\n", name, config);
		goto out;
	}
	if (scan.n_cameras >= LAYOUT_MAX_CAMERAS) {
		fprintf(stderr, "rtspwall: %s already has %d cameras (the maximum)\n", config,
			scan.n_cameras);
		goto out;
	}
	int cells = scan.grid_cols * scan.grid_rows;
	if (cell > cells) {
		fprintf(stderr, "rtspwall: CELL %d is outside GRID=%dx%d (cells 1-%d)\n", cell,
			scan.grid_cols, scan.grid_rows, cells);
		goto out;
	}
	bool rotation = false;
	if (!cell) {
		cell = cfg_first_free_cell(&scan);
		if (!cell) {
			fprintf(stderr, "rtspwall: all %d cells of GRID=%dx%d are taken; give a CELL "
					"to share it (rotation group), or enlarge GRID\n", cells,
				scan.grid_cols, scan.grid_rows);
			goto out;
		}
	} else {
		rotation = scan.cell_used[cell];
	}

	/* ---- the URL */
	char url[LAYOUT_URL_MAX + 2];
	if (cli_read_url(!isatty(STDIN_FILENO), url, sizeof url) < 0)
		goto out;
	why = cfg_check_url(url);
	if (why) {
		fprintf(stderr, "rtspwall: %s\n", why);
		goto out;
	}

	/* ---- probe: the new camera (as the wall will play it with this
	 * config's UNIFI_REWRITE), then the total with the existing ones */
	size_t cap = strlen(text) + strlen(name) + strlen(url) + 32;
	char *next = malloc(cap);
	if (!next || cfg_append_camera(text, name, url, cell, next, cap) < 0) {
		fprintf(stderr, "rtspwall: out of memory\n");
		free(next);
		goto out;
	}
	/* UNIFI_REWRITE as the file will have it (it may be set anywhere; an
	 * empty config does not parse on its own). */
	static struct layout_config cfg;
	char err[768];
	enum layout_unifi_mode mode = LAYOUT_UNIFI_MODE_TLS;
	if (layout_parse(&cfg, next, NULL, NULL, err, sizeof err) == 0)
		mode = cfg.unifi_mode;
	bool cfg_ok = layout_parse(&cfg, text, NULL, NULL, err, sizeof err) == 0;
	struct probe_result pr;
	enum budget_verdict v;
	probe_single(url, mode, &pr, &v);

	long others = 0;
	int others_measured = 0, others_total = 0, n_large = 0;
	if (cfg_ok && cfg.count > 0) {
		const char *urls[LAYOUT_MAX_CAMERAS];
		static struct probe_result res[LAYOUT_MAX_CAMERAS];
		for (int i = 0; i < cfg.count; i++)
			urls[i] = cfg.cam[i].url;
		others_total = cfg.count;
		probe_urls(urls, cfg.count, CLI_PROBE_TIMEOUT_MS, res);
		for (int i = 0; i < cfg.count; i++) {
			bool h264 = res[i].err == PROBE_OK && res[i].s.codec == PROBE_CODEC_H264;
			long m = h264 ? budget_stream_mbps(res[i].s.width, res[i].s.height,
							   res[i].s.fps)
				      : 0;
			if (m > 0) {
				others += m;
				others_measured++;
			}
			if (h264 && gpu_mem_large_stream(res[i].s.width, res[i].s.height))
				n_large++;
		}
	}
	if (pr.err == PROBE_OK && others_total > 0) {
		long total = others + budget_stream_mbps(pr.s.width, pr.s.height, pr.s.fps);
		enum budget_verdict tv = budget_verdict(total);
		printf("total:    %.1f %% of the decoder with the %d existing camera%s (%d "
		       "measured): %s\n", budget_percent(total), others_total,
		       others_total == 1 ? "" : "s", others_measured, budget_verdict_name(tv));
		if (tv > v)
			v = tv;
	}

	if (v == BUDGET_FAIL && !force) {
		fprintf(stderr, "\nrtspwall: not added (probe: FAIL). Fix the stream, or add it "
				"anyway with --force\n");
		free(next);
		goto out;
	}
	if (v == BUDGET_FAIL)
		printf("--force: adding despite FAIL\n");

	/* ---- gpu_mem for the resulting config. 4+ streams of >= 1080p at
	 * too little gpu_mem made the decoder firmware run out of memory ("Not
	 * enough GPU mem") and the codec stayed wedged until a reboot: refuse
	 * that before the wall starts with it. The budget rule (> 50 %) is an
	 * unvalidated heuristic: only warn. */
	bool new_h264 = pr.err == PROBE_OK && pr.s.codec == PROBE_CODEC_H264;
	long gpu_total = others + (new_h264 ? budget_stream_mbps(pr.s.width, pr.s.height,
								  pr.s.fps) : 0);
	if (new_h264 && gpu_mem_large_stream(pr.s.width, pr.s.height))
		n_large++;
	unsigned why_need = GPU_NEED_NONE;
	int need = doctor_gpu_mem_needed(gpu_total, n_large, &why_need);
	int n_cams = others_total + 1;
	if (need) {
		struct cli_gpu_mem g;
		cli_gpu_mem_read(&g);
		enum gpu_check gc = gpu_mem_check(need, g.live, g.configtxt);
		char pending[160] = "";
		if (gc == GPU_CHECK_LOW_REBOOT)
			snprintf(pending, sizeof pending, "; config.txt sets %s=%d, active after a "
				 "reboot", g.info.key, g.configtxt);
		/* A gpu_mem line under a filter that cannot be evaluated ([HDMI:0],
		 * [EDID=...]): the value after a reboot is not certain either. */
		char uncertain[200] = "";
		if (g.info.uncertain)
			snprintf(uncertain, sizeof uncertain, " (config.txt also sets gpu_mem under "
				 "%s; check with: sudo rtspwall doctor)", g.info.filter);
		bool refuse = (gc == GPU_CHECK_LOW || gc == GPU_CHECK_LOW_REBOOT) &&
			      (why_need & GPU_NEED_LARGE);
		if (gc == GPU_CHECK_OK) {
			printf("gpu_mem:  %d MB active, enough (the new config, %d camera%s, needs "
			       "%d MB)\n", g.live, n_cams, n_cams == 1 ? "" : "s", need);
		} else if (gc == GPU_CHECK_UNKNOWN) {
			fprintf(stderr, "rtspwall: warning: cannot read the active gpu_mem (vcgencmd "
					"get_mem gpu failed); the new config (%d camera%s) needs %d MB. "
					"Check with: sudo rtspwall doctor\n", n_cams,
				n_cams == 1 ? "" : "s", need);
		} else if (refuse && !force) {
			fprintf(stderr, "\nrtspwall: not added (gpu_mem: %d MB active, the new config "
					"has %d streams of 1080p or larger and needs %d MB%s). With too "
					"little gpu_mem the decoder firmware can lock up until a "
					"reboot.\n", g.live, n_large, need, pending);
			if (gc == GPU_CHECK_LOW_REBOOT)
				fprintf(stderr, "Fix: sudo reboot, then run add again%s. Or use the "
						"camera's sub-stream, or add it anyway with --force\n",
					uncertain);
			else
				fprintf(stderr, "Fix: sudo rtspwall doctor --fix, then sudo reboot as "
						"a separate command%s. Or use the camera's sub-stream, or "
						"add it anyway with --force\n", uncertain);
			free(next);
			goto out;
		} else if (refuse) {
			printf("--force: adding despite too little gpu_mem (%d MB active, %d MB "
			       "needed%s)\n", g.live, need, pending);
		} else {
			fprintf(stderr, "rtspwall: warning: gpu_mem %d MB active; the new config (%d "
					"camera%s) uses %.0f %% of the decoder and may need %d MB%s. "
					"Recommended: %s%s\n", g.live, n_cams, n_cams == 1 ? "" : "s",
				budget_percent(gpu_total), need, pending,
				gc == GPU_CHECK_LOW_REBOOT
					? "sudo reboot"
					: "sudo rtspwall doctor --fix, then sudo reboot as a "
					  "separate command",
				uncertain);
		}
	}

	/* ---- write (the service's InvocationID first: start_service compares
	 * it to tell whether rtspwall-config.path really restarted the wall) */
	char inv_buf[128];
	const char *inv_before = NULL;
	if (!no_systemd && cli_have_systemd() && geteuid() == 0)
		inv_before = unit_invocation(CLI_SERVICE, inv_buf, sizeof inv_buf);
	if (write_atomic(real, next, &st, check_with_daemon_parser) < 0) {
		free(next);
		goto out;
	}
	free(next);

	char masked[LAYOUT_URL_MAX];
	layout_mask_url(url, masked, sizeof masked);
	printf("\nadded:    %s|%s|%d to %s%s\n", name, masked, cell, real,
	       rotation ? " (shares the cell: rotation group)" : "");

	if (no_systemd) {
		if (cli_have_systemd() && cli_unit_active(CLI_WATCH_PATH) &&
		    cli_unit_enabled(CLI_SERVICE))
			printf("note:     --no-systemd: %s is active and restarts the wall by itself "
			       "when it sees the change\n", CLI_WATCH_PATH);
		ret = 0;
	} else {
		ret = start_service(inv_before);
	}
out:
	free(text);
	close(lock_fd);
	return ret;
}
