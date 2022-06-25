/*
 * main.c
 * Main file that contains the entry point into the reloader program.
 */

#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reloader.h"

static void	cleanup(int _signal);
static void	print_version_string(void);
static bool	set_signal_handlers(void);
static void	usage(const char *_prog);

static struct reloader_t *reloader;

static int		 flag[3];
static char		*short_options = "hvd:";
static struct option	 long_options[] = {
	{"help",	no_argument,		&flag[0],	'h'},
	{"version",	no_argument,		&flag[1],	'v'},
	{"delay",	required_argument,	&flag[2],	'd'},
	{0, 0, 0, 0}
};

int
main(int argc, char *argv[])
{
	int	opt, opt_idx;
	int	result;
	int	delay;
	char	path[PATH_MAX];
	char	cmd[PATH_MAX];

	for(;;) {
		opt_idx = 0;
		opt = getopt_long(argc, argv, short_options, long_options,
		    &opt_idx);
		if (opt == -1)
			break;

		/* If a long option was used set opt to the short one */
		if (opt == 0) {
			opt = flag[opt_idx];
		}

		switch(opt) {
		case 'v':
			print_version_string();
			exit(EXIT_SUCCESS);
			break;
		case 'd':
			delay = atoi(optarg);
			if (delay < 0) {
				fprintf(stderr,
				    "Delay must be a positive integer\n\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			} else if (delay == 0) {
				printf("Delay is invalid or zero. Defaulting " \
				    "to %d seconds.\n", RLD_DEFAULT_DELAY);
			}
			break;
		case 'h':
			/* FALLTHROUGH */
		case '?':
			/* FALLTHROUGH */
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
			break;
		}
	}

	if ((argc - optind) < 2) {
		fprintf(stderr, "Too few arguments\n\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	strncpy(path, argv[optind++], PATH_MAX);
	strncpy(cmd, argv[optind++], PATH_MAX);

	if (!set_signal_handlers()) {
		fprintf(stderr, "Failed to set signal handlers\n");
		exit(EXIT_FAILURE);
	}

	reloader = reloader_new();
	if (reloader == NULL) {
		fprintf(stderr, "Failed to create service reloader\n");
		exit(EXIT_FAILURE);
	}

	reloader_add_watch(reloader, path, cmd, delay);
	result = reloader_start(reloader);
	if (result == -1) {
		fprintf(stderr, "Failed to start reloader\n");
		reloader_close(reloader);
		exit(EXIT_FAILURE);
	} else if (result == 1) {
		fprintf(stderr, "Reloader stopped because no events were "     \
		    "pending or active\n");
		reloader_close(reloader);
	}

	exit(EXIT_SUCCESS);
}

static void
cleanup(int signal)
{

	printf("Canceling...");

	if (reloader_stop(reloader) == -1) {
		fprintf(stderr, "Failed to break event base loop\n");
	}
	reloader_close(reloader);

	printf("Done\n");

	return;
}

static void
print_version_string(void)
{

	fprintf(stderr, "Version: %s\n", RLD_VERSION);

	return;
}

static bool
set_signal_handlers(void)
{
	struct sigaction action;

	action.sa_handler = cleanup;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;

	if (sigaction(SIGHUP, &action, NULL) != 0) {
		return (false);
	}

	if (sigaction(SIGINT, &action, NULL) != 0) {
		return (false);
	}

	if (sigaction(SIGTERM, &action, NULL) != 0) {
		return (false);
	}

	return (true);
}

static void
usage(const char *prog)
{

	fprintf(stderr, "Version: %s\n" \
	    "Usage:\n  %s [-h|--help] [-v|--version] [-d|--delay secs] "       \
	    "path command\n", RLD_VERSION, prog);

	return;
}
