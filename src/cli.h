/*
 * cli.h — the user commands: `rtspwall probe|add|doctor|demo ...`.
 *
 * main() calls cli_is_subcommand(argv[1]) first and hands the whole command
 * line to cli_main() for a subcommand; everything else (`rtspwall [CONFIG]`,
 * --check-config, --help, --version) is unchanged.
 *
 *   cli.c      dispatch, help text and shared helpers (subprocesses,
 *              systemctl, hidden URL prompt, file reading)
 *   probe.c    probe: DESCRIBE+SETUP without PLAY, codec/size/fps, budget
 *   add.c      add: probe + append one camera line atomically + start/restart
 *   doctor.c   doctor: system self-test, --fix, --report
 *   demo.c     demo: run rtspwall-demo.service and tail its journal
 *
 * The decisions themselves (verdicts, budget, config.txt and config-line
 * edits, report masking) are pure and unit-tested: clilogic.c, budget.c.
 */
#ifndef CLI_H
#define CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "budget.h"
#include "clilogic.h"
#include "layout.h"

#define CLI_DEFAULT_CONFIG  "/etc/rtspwall/cameras.conf"
#define CLI_SERVICE         "rtspwall.service"
#define CLI_DEMO_SERVICE    "rtspwall-demo.service"
#define CLI_PROBE_TIMEOUT_MS 5000

/* True if `arg` names a subcommand (probe, add, doctor, demo). */
bool cli_is_subcommand(const char *arg);

/* Runs the subcommand in argv[1]; argv[0] is the program. Returns the
 * process exit status. */
int cli_main(int argc, char **argv);

/* The subcommand part of --help. */
void cli_usage(FILE *out);

/* ------------------------------------------------------------ subcommands */

int cmd_probe(int argc, char **argv);
int cmd_add(int argc, char **argv);
int cmd_doctor(int argc, char **argv);
int cmd_demo(int argc, char **argv);

/* ------------------------------------------------------------------ probe */

struct probe_result {
	enum probe_error err;
	char   detail[320];       /* libav error text for err != PROBE_OK */
	struct probe_stream s;
	char   codec_name[32];    /* "h264", "hevc", ... */
	const char *fps_source;   /* "VUI", "SDP", "container", "packets", NULL */
	int    elapsed_ms;
};

/* Probes one URL (or local file) without starting playback: libavformat
 * with initial_pause=1 sends DESCRIBE and SETUP but no PLAY. Only when the
 * SDP/SPS lack the size or frame rate does it fall back to PLAY and at
 * most ~2 s of packets. Returns 0 if the stream description was read. */
int probe_url(const char *url, int timeout_ms, struct probe_result *r);

/* Probes n URLs in parallel threads (each with its own timeout). */
void probe_urls(const char *const *urls, int n, int timeout_ms, struct probe_result *res);

/* Verdict and hint for a probe result; a failed probe is FAIL. */
enum budget_verdict probe_result_verdict(const struct probe_result *r, const char *masked_url,
					 char *hint, size_t hintlen);

/* Prints the detailed block for one URL (masked). Returns the verdict. */
enum budget_verdict probe_print_one(const char *url, const struct probe_result *r);

/* Probes a URL as pasted (UniFi Protect rtsps URLs the way the daemon will
 * play them), prints the block, stores the verdict. Returns 0/1/2. */
int probe_single(const char *url, struct probe_result *r, enum budget_verdict *v);

/* Probes every camera of cfg in parallel and prints the budget table.
 * Returns the worst verdict; *total_out = summed macroblocks/s. */
enum budget_verdict probe_print_config(const struct layout_config *cfg, long *total_out);

/* ---------------------------------------------------------------- helpers */

/* Reads a whole file (at most max bytes) into a NUL-terminated heap
 * buffer. Returns NULL with errno set on failure (EFBIG if too large). */
char *cli_read_file(const char *path, size_t max);

/* Parses a config file with the daemon's own parser (layout_parse).
 * Returns 0, or -1 with a message in err. */
int cli_load_config(const char *path, struct layout_config *cfg, char *err, size_t errlen);

/* Gets a camera URL without it ever being on the command line:
 * from_stdin -> one line from standard input; otherwise a prompt on
 * /dev/tty with echo turned off. Returns 0, or -1 with a message printed. */
int cli_read_url(bool from_stdin, char *out, size_t outlen);

/* Asks a yes/no question on /dev/tty. False if there is no terminal. */
bool cli_ask_yes_no(const char *question);

/* True if a terminal is available for questions (stdin is a tty and
 * /dev/tty opens). */
bool cli_interactive(void);

/* Runs argv (PATH lookup), capturing stdout+stderr into out (truncated,
 * NUL-terminated; out may be NULL). stdin is /dev/null. Kills the child
 * after timeout_ms. Returns the exit status, 128+signal, or -1 if it could
 * not be run (e.g. not installed). */
int cli_run(char *const argv[], char *out, size_t outlen, int timeout_ms);

/* Path of the running rtspwall binary (for `--check-config` subprocesses). */
const char *cli_self_exe(void);

/* systemd */
bool cli_have_systemd(void);
int  cli_systemctl(const char *verb, const char *unit);   /* exit status */
bool cli_unit_active(const char *unit);
bool cli_unit_enabled(const char *unit);
bool cli_unit_exists(const char *unit);

#endif
