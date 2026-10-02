/*
 * demo.c — `rtspwall demo`.
 *
 * Starts rtspwall-demo.service (the packaged unit runs the bundled CC0
 * clips in /usr/share/rtspwall/demo with the same user and hardening as
 * the real service, and Conflicts=rtspwall.service), tails its journal —
 * including the 60 s statistics lines — and on Ctrl-C stops the demo and
 * restarts rtspwall.service if it was running before. The unit has the
 * right paths baked in, so nothing here builds paths.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"

static volatile sig_atomic_t demo_stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	demo_stop = 1;
}

static void demo_usage(FILE *out)
{
	fprintf(out,
		"Usage: sudo rtspwall demo\n"
		"\n"
		"Shows a 2x2 demo wall from bundled test clips (no cameras needed) by\n"
		"starting " CLI_DEMO_SERVICE ", and prints its log. Ctrl-C stops the demo and\n"
		"restarts " CLI_SERVICE " if it was running before.\n");
}

/* Starts `journalctl -f` for the demo unit with its stdout on a pipe. */
static pid_t start_tail(int *fd_out)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(p[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		signal(SIGINT, SIG_IGN);   /* the parent decides when to stop */
		execlp("journalctl", "journalctl", "-f", "-n", "0", "-o", "cat",
		       "-u", CLI_DEMO_SERVICE, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	*fd_out = p[0];
	return pid;
}

static void stop_tail(pid_t *tail, int *tail_fd)
{
	if (*tail > 0) {
		kill(*tail, SIGTERM);
		while (waitpid(*tail, NULL, 0) < 0 && errno == EINTR)
			;
		*tail = -1;
	}
	if (*tail_fd >= 0) {
		close(*tail_fd);
		*tail_fd = -1;
	}
}

static void restore(bool was_active)
{
	printf("\nstopping the demo ...\n");
	cli_systemctl("stop", CLI_DEMO_SERVICE);
	if (was_active) {
		if (cli_systemctl("start", CLI_SERVICE) == 0)
			printf("restarted %s (it was running before the demo)\n", CLI_SERVICE);
		else
			fprintf(stderr, "rtspwall: could not restart %s; try: sudo systemctl start "
					"rtspwall\n", CLI_SERVICE);
	} else {
		printf("%s was not running before the demo; left stopped\n", CLI_SERVICE);
	}
}

int cmd_demo(int argc, char **argv)
{
	if (argc > 1) {
		bool help = strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0;
		demo_usage(help ? stdout : stderr);
		return help ? 0 : 2;
	}
	if (!cli_have_systemd()) {
		fprintf(stderr,
			"rtspwall: systemd is not running here, and the demo runs as %s.\n"
			"Without systemd, run the demo config directly:\n"
			"  sudo rtspwall /usr/share/rtspwall/demo/demo.conf\n", CLI_DEMO_SERVICE);
		return 2;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "rtspwall: the demo starts and stops services: run it with sudo\n");
		return 2;
	}
	if (!cli_unit_exists(CLI_DEMO_SERVICE)) {
		fprintf(stderr, "rtspwall: %s is not installed (install the rtspwall package, "
				"or run install.sh from the source tree)\n", CLI_DEMO_SERVICE);
		return 2;
	}

	/* "running" includes activating (auto-restart between two attempts):
	 * a wall that is retrying must come back after the demo too. */
	bool was_active = cli_unit_running(CLI_SERVICE);

	struct sigaction sa = { .sa_handler = on_signal };   /* no SA_RESTART */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	int tail_fd = -1;
	pid_t tail = start_tail(&tail_fd);

	if (cli_systemctl("start", CLI_DEMO_SERVICE) != 0) {
		fprintf(stderr, "rtspwall: could not start %s; see: journalctl -u rtspwall-demo -b\n"
				"and run: sudo rtspwall doctor\n", CLI_DEMO_SERVICE);
		stop_tail(&tail, &tail_fd);
		restore(was_active);
		return 1;
	}
	printf("demo running: 4 moving tiles should appear within ~10 s; cell 4 rotates "
	       "every 8 s.\n%s"
	       "Statistics are logged every 60 s. Press Ctrl-C to stop.\n\n",
	       was_active ? "(" CLI_SERVICE " was stopped for the demo and comes back on Ctrl-C)\n"
			  : "");
	fflush(stdout);

	int rc = 0;
	time_t last_check = time(NULL);
	char buf[4096];
	while (!demo_stop) {
		struct pollfd p = { tail_fd, POLLIN, 0 };
		int r = tail_fd >= 0 ? poll(&p, 1, 1000) : (sleep(1), 0);
		if (r > 0) {
			ssize_t n = read(tail_fd, buf, sizeof buf);
			if (n > 0) {
				(void)!write(STDOUT_FILENO, buf, (size_t)n);
			} else if (n == 0) {
				close(tail_fd);
				tail_fd = -1;
			}
		}
		if (time(NULL) - last_check >= 2) {
			last_check = time(NULL);
			if (!cli_unit_active(CLI_DEMO_SERVICE)) {
				fprintf(stderr, "\nrtspwall: the demo stopped by itself; see: journalctl "
						"-u rtspwall-demo -b, and run: sudo rtspwall doctor\n");
				rc = 1;
				break;
			}
		}
	}

	stop_tail(&tail, &tail_fd);
	restore(was_active);
	return rc;
}
