/*
 * cli.c — subcommand dispatch, help text and the helpers the user commands
 * share. See cli.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"

static const struct {
	const char *name;
	int (*fn)(int, char **);
} commands[] = {
	{ "probe",  cmd_probe },
	{ "add",    cmd_add },
	{ "doctor", cmd_doctor },
	{ "demo",   cmd_demo },
};

bool cli_is_subcommand(const char *arg)
{
	if (!arg)
		return false;
	for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++)
		if (strcmp(arg, commands[i].name) == 0)
			return true;
	return false;
}

int cli_main(int argc, char **argv)
{
	/* Line-buffered even into a pipe, so stdout and stderr lines keep
	 * their order in logs and bug reports. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++)
		if (strcmp(argv[1], commands[i].name) == 0)
			return commands[i].fn(argc - 1, argv + 1);   /* argv[0] = subcommand */
	return 2;
}

void cli_usage(FILE *out)
{
	fprintf(out,
		"\n"
		"Commands (the URL is read from a hidden prompt, never the command line):\n"
		"  rtspwall probe [URL | - | CONFIG]\n"
		"                     check a camera stream (codec, size, fps, decoder budget)\n"
		"                     without playing it; no argument = hidden prompt,\n"
		"                     - = read the URL from stdin, CONFIG = probe every\n"
		"                     camera and print the budget table\n"
		"  rtspwall add [--config PATH] [--force] [--no-systemd] NAME [CELL]\n"
		"                     probe a camera and add it to the first free cell (or\n"
		"                     CELL), then start or restart the service\n"
		"  rtspwall doctor [--config PATH] [--fix] [--yes] [--report] [--summary]\n"
		"                  [--no-hardware] [--no-probe]\n"
		"                     self-test with copy-paste fixes; exit 0 ok, 1 warning,\n"
		"                     2 failure; --report prints a bundle for bug reports with\n"
		"                     every URL and token masked\n"
		"  rtspwall demo      show the built-in demo wall (no cameras needed);\n"
		"                     Ctrl-C stops it and restores the previous state\n"
		"\n"
		"Run `rtspwall COMMAND --help` for details.\n");
}

/* ------------------------------------------------------------------ files */

char *cli_read_file(const char *path, size_t max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		close(fd);
		errno = ENOMEM;
		return NULL;
	}
	for (;;) {
		if (len + 1 >= cap) {
			if (cap > max) {
				free(buf);
				close(fd);
				errno = EFBIG;
				return NULL;
			}
			char *nb = realloc(buf, cap * 2);
			if (!nb) {
				free(buf);
				close(fd);
				errno = ENOMEM;
				return NULL;
			}
			buf = nb;
			cap *= 2;
		}
		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			free(buf);
			close(fd);
			errno = e;
			return NULL;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	if (len > max) {
		free(buf);
		errno = EFBIG;
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

int cli_load_config(const char *path, struct layout_config *cfg, char *err, size_t errlen)
{
	char *text = cli_read_file(path, 1024 * 1024);
	if (!text) {
		snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	char msg[512];
	int r = layout_parse(cfg, text, NULL, NULL, msg, sizeof msg);
	free(text);
	if (r < 0) {
		char masked[512];
		layout_mask_urls_in_text(msg, masked, sizeof masked);
		snprintf(err, errlen, "%s: %s", path, masked);
	}
	return r;
}

/* ------------------------------------------------------------- the prompt */

static int tty_fd = -1;
static struct termios tty_saved;
static volatile sig_atomic_t tty_echo_off = 0;

static void tty_restore(void)
{
	if (tty_echo_off && tty_fd >= 0) {
		tcsetattr(tty_fd, TCSAFLUSH, &tty_saved);
		tty_echo_off = 0;
	}
}

/* Ctrl-C at the hidden prompt must not leave the terminal without echo. */
static void prompt_signal(int sig)
{
	if (tty_echo_off && tty_fd >= 0) {
		tcsetattr(tty_fd, TCSAFLUSH, &tty_saved);
		(void)!write(tty_fd, "\n", 1);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void chomp(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
	size_t lead = strspn(s, " \t");
	if (lead)
		memmove(s, s + lead, strlen(s + lead) + 1);
}

static int read_line_fd(int fd, char *out, size_t outlen)
{
	size_t n = 0;
	bool overlong = false;
	for (;;) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		if (c == '\n')
			break;
		if (n + 1 < outlen)
			out[n++] = c;
		else
			overlong = true;
	}
	out[n] = '\0';
	return overlong ? -1 : 0;
}

int cli_read_url(bool from_stdin, char *out, size_t outlen)
{
	out[0] = '\0';
	if (from_stdin) {
		if (read_line_fd(STDIN_FILENO, out, outlen) < 0) {
			fprintf(stderr, "rtspwall: the URL on stdin is too long\n");
			return -1;
		}
	} else {
		tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
		if (tty_fd < 0) {
			fprintf(stderr, "rtspwall: no terminal for the hidden prompt; pipe the URL "
					"in instead, e.g.:  rtspwall probe - < url.txt\n");
			return -1;
		}
		static const char prompt[] = "Camera URL (input hidden): ";
		(void)!write(tty_fd, prompt, sizeof prompt - 1);

		struct sigaction sa = { .sa_handler = prompt_signal }, old_int, old_term;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, &old_int);
		sigaction(SIGTERM, &sa, &old_term);
		if (tcgetattr(tty_fd, &tty_saved) == 0) {
			struct termios t = tty_saved;
			t.c_lflag &= ~(tcflag_t)ECHO;
			t.c_lflag |= ECHONL;
			if (tcsetattr(tty_fd, TCSAFLUSH, &t) == 0)
				tty_echo_off = 1;
		}
		int r = read_line_fd(tty_fd, out, outlen);
		tty_restore();
		sigaction(SIGINT, &old_int, NULL);
		sigaction(SIGTERM, &old_term, NULL);
		close(tty_fd);
		tty_fd = -1;
		if (r < 0) {
			fprintf(stderr, "rtspwall: the URL is too long\n");
			return -1;
		}
	}
	chomp(out);
	if (!out[0]) {
		fprintf(stderr, "rtspwall: no URL given\n");
		return -1;
	}
	return 0;
}

bool cli_interactive(void)
{
	if (!isatty(STDIN_FILENO))
		return false;
	int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return false;
	close(fd);
	return true;
}

bool cli_ask_yes_no(const char *question)
{
	int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return false;
	char line[16];
	(void)!write(fd, question, strlen(question));
	(void)!write(fd, " [y/N] ", 7);
	read_line_fd(fd, line, sizeof line);
	close(fd);
	chomp(line);
	return line[0] == 'y' || line[0] == 'Y';
}

/* ------------------------------------------------------------ subprocesses */

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int cli_run(char *const argv[], char *out, size_t outlen, int timeout_ms)
{
	int pfd[2];
	if (out && outlen)
		out[0] = '\0';
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -1;

	/* A status pipe tells exec failure (127 from the child would be
	 * ambiguous) apart from a real exit status. */
	int efd[2];
	if (pipe2(efd, O_CLOEXEC) < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]); close(pfd[1]); close(efd[0]); close(efd[1]);
		return -1;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0)
			dup2(devnull, STDIN_FILENO);
		dup2(pfd[1], STDOUT_FILENO);
		dup2(pfd[1], STDERR_FILENO);
		signal(SIGPIPE, SIG_DFL);
		execvp(argv[0], argv);
		int e = errno;
		(void)!write(efd[1], &e, sizeof e);
		_exit(127);
	}
	close(pfd[1]);
	close(efd[1]);

	size_t len = 0;
	int64_t deadline = now_ms() + timeout_ms;
	bool timed_out = false;
	for (;;) {
		int left = (int)(deadline - now_ms());
		if (left <= 0) {
			timed_out = true;
			break;
		}
		struct pollfd p = { pfd[0], POLLIN, 0 };
		int r = poll(&p, 1, left);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			timed_out = r == 0;
			break;
		}
		char buf[4096];
		ssize_t n = read(pfd[0], buf, sizeof buf);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (out && outlen > 1 && len < outlen - 1) {
			size_t take = (size_t)n;
			if (take > outlen - 1 - len)
				take = outlen - 1 - len;
			memcpy(out + len, buf, take);
			len += take;
			out[len] = '\0';
		}
	}
	close(pfd[0]);
	if (timed_out)
		kill(pid, SIGKILL);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	int exec_errno = 0;
	ssize_t en = read(efd[0], &exec_errno, sizeof exec_errno);
	close(efd[0]);
	if (en == (ssize_t)sizeof exec_errno)
		return -1;
	if (timed_out)
		return 128 + SIGKILL;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

const char *cli_self_exe(void)
{
	static char path[PATH_MAX];
	if (!path[0]) {
		ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
		if (n <= 0)
			snprintf(path, sizeof path, "rtspwall");
		else
			path[n] = '\0';
	}
	return path;
}

/* ---------------------------------------------------------------- systemd */

bool cli_have_systemd(void)
{
	struct stat st;
	return stat("/run/systemd/system", &st) == 0 && S_ISDIR(st.st_mode);
}

int cli_systemctl(const char *verb, const char *unit)
{
	char *argv[] = { "systemctl", (char *)verb, (char *)unit, NULL };
	char out[1024];
	int r = cli_run(argv, out, sizeof out, 60000);
	if (r != 0 && out[0] && strcmp(verb, "is-active") && strcmp(verb, "is-enabled"))
		fprintf(stderr, "%s", out);
	return r;
}

bool cli_unit_active(const char *unit)
{
	char *argv[] = { "systemctl", "is-active", "--quiet", (char *)unit, NULL };
	return cli_run(argv, NULL, 0, 10000) == 0;
}

bool cli_unit_enabled(const char *unit)
{
	char *argv[] = { "systemctl", "is-enabled", (char *)unit, NULL };
	char out[128];
	if (cli_run(argv, out, sizeof out, 10000) != 0)
		return false;
	return strncmp(out, "enabled", 7) == 0;
}

bool cli_unit_exists(const char *unit)
{
	char *argv[] = { "systemctl", "show", "-p", "LoadState", "--value", (char *)unit, NULL };
	char out[128];
	if (cli_run(argv, out, sizeof out, 10000) != 0)
		return false;
	return strncmp(out, "loaded", 6) == 0;
}
