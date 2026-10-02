/*
 * add.c — `rtspwall add [--config PATH] [--force] [--no-systemd] NAME [CELL]`.
 *
 * Adds one camera without the URL ever touching the command line:
 *   1. reads the URL from a hidden prompt (or stdin when it is not a
 *      terminal), checks it can live in a config line;
 *   2. probes it (and the cameras already configured, for the total
 *      decoder budget); a FAIL is refused unless --force;
 *   3. appends "NAME|URL|CELL" to the config — the first free cell unless
 *      CELL is given — by writing a temp file next to it, checking it with
 *      `rtspwall --check-config`, copying mode and owner (0640
 *      root:rtspwall) and renaming it over the original. Nothing else in
 *      the file changes; commented-out lines stay commented out;
 *   4. starts the wall: `systemctl enable --now` if the service is not
 *      enabled (and says how to undo that), otherwise `systemctl restart`.
 *
 * The line logic is pure and tested (cfg_* in clilogic.c).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli.h"

#define CONFIG_MAX_BYTES (1024 * 1024)

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
		"  --force         add the camera even if the probe says FAIL\n"
		"  --no-systemd    only edit the file, do not start/restart the service\n");
}

/* Writes `text` to `path` atomically, keeping the original file's mode
 * and owner. `checked` is called with the temp file's path before the
 * rename; a non-zero return aborts. */
static int write_atomic(const char *path, const char *text, const struct stat *orig,
			int (*checked)(const char *tmp))
{
	char tmp[LAYOUT_PATH_MAX + 16];
	if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
		fprintf(stderr, "rtspwall: path too long: %s\n", path);
		return -1;
	}
	int fd = mkstemp(tmp);
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
	if (!r && fchmod(fd, orig->st_mode & 07777) < 0) {
		fprintf(stderr, "rtspwall: chmod %s: %s\n", tmp, strerror(errno));
		r = -1;
	}
	if (!r && (orig->st_uid != geteuid() || orig->st_gid != getegid()) &&
	    fchown(fd, orig->st_uid, orig->st_gid) < 0) {
		fprintf(stderr, "rtspwall: cannot keep the owner of %s (%s); run with sudo\n",
			path, strerror(errno));
		r = -1;
	}
	if (!r && fsync(fd) < 0) {
		fprintf(stderr, "rtspwall: fsync %s: %s\n", tmp, strerror(errno));
		r = -1;
	}
	close(fd);
	if (!r && checked && checked(tmp) != 0)
		r = -1;
	if (!r && rename(tmp, path) < 0) {
		fprintf(stderr, "rtspwall: rename %s -> %s: %s\n", tmp, path, strerror(errno));
		r = -1;
	}
	if (r) {
		unlink(tmp);
		return -1;
	}

	/* Make the rename itself durable. */
	char dir[LAYOUT_PATH_MAX + 16];
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

static int start_service(void)
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
	if (!cli_unit_enabled(CLI_SERVICE)) {
		if (cli_systemctl("enable", CLI_SERVICE) != 0 ||
		    cli_systemctl("restart", CLI_SERVICE) != 0) {
			fprintf(stderr, "rtspwall: could not enable/start %s; see: "
					"journalctl -u rtspwall -b\n", CLI_SERVICE);
			return 1;
		}
		printf("enabled and started %s: the wall now also starts at boot\n"
		       "  (undo: sudo systemctl disable --now rtspwall)\n", CLI_SERVICE);
	} else {
		if (cli_systemctl("restart", CLI_SERVICE) != 0) {
			fprintf(stderr, "rtspwall: restart failed; see: journalctl -u rtspwall -b\n");
			return 1;
		}
		printf("restarted %s\n", CLI_SERVICE);
	}
	printf("status:   systemctl status rtspwall    log: journalctl -u rtspwall -f\n");
	return 0;
}

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

	/* ---- the config as it is */
	struct stat st;
	if (stat(config, &st) < 0) {
		fprintf(stderr, "rtspwall: %s: %s (is rtspwall installed? or use --config PATH)\n",
			config, strerror(errno));
		return 2;
	}
	char *text = cli_read_file(config, CONFIG_MAX_BYTES);
	if (!text) {
		fprintf(stderr, "rtspwall: cannot read %s: %s%s\n", config, strerror(errno),
			errno == EACCES ? " (run with sudo)" : "");
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

	/* ---- probe: the new camera, then the total with the existing ones */
	struct probe_result pr;
	enum budget_verdict v;
	probe_single(url, &pr, &v);

	long others = 0;
	int others_measured = 0, others_total = 0;
	static struct layout_config cfg;
	char err[768];
	if (cli_load_config(config, &cfg, err, sizeof err) == 0 && cfg.count > 0) {
		const char *urls[LAYOUT_MAX_CAMERAS];
		static struct probe_result res[LAYOUT_MAX_CAMERAS];
		for (int i = 0; i < cfg.count; i++)
			urls[i] = cfg.cam[i].url;
		others_total = cfg.count;
		probe_urls(urls, cfg.count, CLI_PROBE_TIMEOUT_MS, res);
		for (int i = 0; i < cfg.count; i++) {
			long m = res[i].err == PROBE_OK && res[i].s.codec == PROBE_CODEC_H264
				 ? budget_stream_mbps(res[i].s.width, res[i].s.height, res[i].s.fps)
				 : 0;
			if (m > 0) {
				others += m;
				others_measured++;
			}
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
		goto out;
	}
	if (v == BUDGET_FAIL)
		printf("--force: adding despite FAIL\n");

	/* ---- write */
	size_t cap = strlen(text) + strlen(name) + strlen(url) + 32;
	char *next = malloc(cap);
	if (!next || cfg_append_camera(text, name, url, cell, next, cap) < 0) {
		fprintf(stderr, "rtspwall: out of memory\n");
		free(next);
		goto out;
	}
	if (write_atomic(config, next, &st, check_with_daemon_parser) < 0) {
		free(next);
		goto out;
	}
	free(next);

	char masked[LAYOUT_URL_MAX];
	layout_mask_url(url, masked, sizeof masked);
	printf("\nadded:    %s|%s|%d to %s%s\n", name, masked, cell, config,
	       rotation ? " (shares the cell: rotation group)" : "");

	ret = no_systemd ? 0 : start_service();
out:
	free(text);
	return ret;
}
