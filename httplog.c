/*
 *  httplog.c - An Apache logfile rollover program
 *
 *  Copyright (c) 2001,2002 Eli Sand
 *
 *  This source file is covered by the Free Software License (FSL), as published
 *  by Eli Sand.  A copy of the Free Software License (FSL) should have been
 *  included with this file, if not, please contact the author of this software.
 *
 */

#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "defines.h"

#ifdef USE_ZLIB
# include <wait.h>
# include <zlib.h>
#endif

/* maximum characters allowed in path */
#ifndef PATH_MAX
# define PATH_MAX	512
#endif

/* maximum characters allowed from stdin */
#ifndef BUFSIZ
# define BUFSIZ		8192
#endif

#define min(x, y)	((x) < (y)) ? (x) : (y)

#ifdef USE_ZLIB
 int gzip(char *);
#endif
int mkdirs(char *);
int eprintf(const char *, const char *, ...);
int parsetags(char *, size_t, const char *);
void sighandler(int);
char *getlocalfqdn(void);


int main(int argc, char *argv[]) {
	int curopt = 0;
	time_t epoch = time(NULL);
	unsigned long bufsize = BUFSIZ;

	FILE *fd = NULL;
	char *buffer = NULL;

	char input[BUFSIZ + 1] = {'\0'};
	char tmplog[PATH_MAX + 1] = {'\0'};
	char oldlog[PATH_MAX + 1] = {'\0'};
	char curlog[PATH_MAX + 1] = {'\0'};
	char symlnk[PATH_MAX + 1] = {'\0'};

	struct tm *timeinfo;
	struct group  *gid = NULL;
	struct passwd *uid = NULL;
	struct sigaction sigact_hup;
	struct sigaction sigact_term;

#ifdef USE_ZLIB
	int zlib = 0;
	struct sigaction sigact_chld;

	/* trap SIGCHLD so we can get rid of any zombie processes */
	sigact_chld.sa_handler = (void *)sighandler;
	sigemptyset(&sigact_chld.sa_mask);
	sigaction(SIGCHLD, &sigact_chld, NULL);
#endif

	/* trap SIGHUP so we can flush our logfile */
	sigact_hup.sa_handler = (void *)sighandler;
	sigemptyset(&sigact_hup.sa_mask);
	sigaction(SIGHUP, &sigact_hup, NULL);

	/* trap SIGTERM so we can flush our logfile and exit gracefully */
	sigact_term.sa_handler = (void *)sighandler;
	sigemptyset(&sigact_term.sa_mask);
	sigaction(SIGTERM, &sigact_term, NULL);

	while ((curopt = getopt(argc, argv, "Vvzu:g:s:b:h?")) != EOF) {
		switch (curopt) {
			case 'V':
			case 'v': {
				eprintf("notice", "\b\b version %s", VERSION);
				break;
			}
			case 'z': {
				/* enable zlib compression for logfiles */
#ifdef USE_ZLIB
				zlib = 1;
#else
				eprintf("error", "zlib compression was not compiled into httplog");
				exit(1);
#endif
				break;
			}
			case 'u': {
				/* find the user id number for the specified user */
				if ((uid = getpwnam(optarg)) == NULL) {
					eprintf("error", "unable to find user id number for `%s'", optarg);
					exit(1);
				}
				break;
			}
			case 'g': {
				/* find the group id number for the specified group */
				if ((gid = getgrnam(optarg)) == NULL) {
					eprintf("error", "unable to find group id number for `%s'", optarg);
					exit(1);
				}
				break;
			}
			case 's': {
				/* enable symlink creation */
				strncpy(symlnk, optarg, PATH_MAX);
				break;
			}
			case 'b': {
				/* find out the maximum amount of ram to malloc */
				if (atoi(optarg) < BUFSIZ) {
					eprintf("error", "buffer size can be no less than `%d'", BUFSIZ);
					exit(1);
				}
				bufsize = atoi(optarg);
				break;
			}
			case 'h':
			case '?':
			default : {
				fprintf(stderr, "usage: %s logfile [-z] [-u user] [-g group] [-s symlink] [-b buffer_size]\n", argv[0]);
				fprintf(stderr, "\tFor example, in your Apache httpd.conf file, insert this line:\n");
				fprintf(stderr, "\tCustomLog \"|/path/to/%s /path/to/logfiles/ex%%Y%%m%%d.log\" combined\n", argv[0]);
				exit(1);
			}
		}
	}
	if ((argc - optind) < 1) {
		fprintf(stderr, "usage: %s logfile [-z] [-u user] [-g group] [-s symlink] [-b buffer_size]\n", argv[0]);
		fprintf(stderr, "\tFor example, in your Apache httpd.conf file, insert this line:\n");
		fprintf(stderr, "\tCustomLog \"|/path/to/%s /path/to/logfiles/ex%%Y%%m%%d.log\" combined\n", argv[0]);
		exit(1);
	}

	/* change the current group id being used by the program */
	if (gid != NULL) {
		if (setgid(gid->gr_gid)) {
#ifdef USE_DEBUG
			eprintf("debug", strerror(errno));
#endif
			eprintf("error", "unable to set group id to `%s'", optarg);
			exit(1);
		}
	}

	/* change the current user id being used by the program */
	if (uid != NULL) {
		if (setuid(uid->pw_uid)) {
#ifdef USE_DEBUG
			eprintf("debug", strerror(errno));
#endif
			eprintf("error", "unable to set user id to `%s'", optarg);
			exit(1);
		}
	}

	/* if we get here, print out a notice to stderr saying we're up and running */
	eprintf("notice", "\b\b/%s configured -- resuming normal operations", VERSION);

	/* parse any special & tags in the filename template */
	parsetags(tmplog, PATH_MAX, argv[optind]);

	/* read standard input */
	/* if we didn't catch EOF from stdin, push text to file, and loop */
	/* test stdin for EOF so that when we catch a SIG, we don't up and die from fgets() returning NULL */
	while (!feof(stdin)) {
		if (fgets(input, BUFSIZ, stdin)) {

			/* parse filename given on command line for strftime formatting */
			if (!(epoch = time(NULL)) || !(timeinfo = localtime(&epoch))) {
#ifdef USE_DEBUG
				eprintf("debug", strerror(errno));
#endif
				eprintf("error", "unable to get local time");
				exit(1);
			}
			strftime(curlog, PATH_MAX, tmplog, timeinfo);

			/* compare the last filename to the current one to see if we need to open a new logfile */
			if (strcmp(oldlog, curlog)) {

				if (fd) {

					/* close the file and free up memory */
					fclose(fd);
					free(buffer);

					/* reset values to ensure data integrety */
					fd = NULL;
					buffer = NULL;

#ifdef USE_ZLIB
					/* if compression is enabled, fork off a child process for compression */
					if (zlib) {
						switch (fork()) {
							case  0: {
								exit(gzip(oldlog));
								break;
							}
							case -1: {
#ifdef USE_DEBUG
								eprintf("debug", strerror(errno));
#endif
								eprintf("error", "unable to fork compression stage for logfile `%s'", curlog);
								break;
							}
						}
					}
#endif
				}

				/* create path to the logfile if it doesn't exist already */
				mkdirs(curlog);

				/* try to append to the evaluated filename, if it doesn't exist, create it */
				if ((fd = fopen(curlog, "a")) == NULL) {
#ifdef USE_DEBUG
					eprintf("debug", strerror(errno));
#endif
					eprintf("error", "unable to open logfile `%s'", curlog);
					exit(1);
				}

				/* allocate some memory for our own logfile buffer */
				if ((buffer = malloc(bufsize + 1)) == NULL) {
#ifdef USE_DEBUG
					eprintf("debug", strerror(errno));
#endif
					eprintf("error", "unable to allocate memory for logfile `%s'", curlog);
					fclose(fd);
					exit(1);
				}
				memset(buffer, '\0', bufsize + 1);

				/* set up our own logfile buffer type based on flush delay setting */
				if (setvbuf(fd, buffer, (bufsize > BUFSIZ) ? _IOFBF : _IOLBF, bufsize) != 0) {
#ifdef USE_DEBUG
					eprintf("debug", strerror(errno));
#endif
					eprintf("error", "unable to initialize buffer for logfile `%s'", curlog);
					fclose(fd);
					free(buffer);
					exit(1);
				}

				/* make a symlink to the current filename */
				if (strlen(symlnk) > 0) {
					unlink(symlnk);
					mkdirs(symlnk);
					if (symlink(curlog, symlnk) != 0) {
#ifdef USE_DEBUG
						eprintf("debug", strerror(errno));
#endif
						eprintf("error", "unable to create symlink `%s'", symlnk);
					}
				}

				/* update the filename for the next loop comparison */
				strncpy(oldlog, curlog, PATH_MAX);
			}

			/* push the input to the logfile */
			if (fputs(input, fd) == EOF) {
#ifdef USE_DEBUG
				eprintf("debug", strerror(errno));
#endif
				eprintf("error", "unable to append to logfile `%s'", curlog);
				fclose(fd);
				free(buffer);
				exit(1);
			}
		}
	}

	/* close any open file descriptors and free up the ram used by the buffer */
	if (fd) {
		fclose(fd);
		free(buffer);
	}

	return(0);
}

#ifdef USE_ZLIB
int gzip(char *filename) {
	FILE    *fd = NULL;
	gzFile gzfd = NULL;
	char buffer[BUFSIZ + 1];
	char gzfilename[PATH_MAX + 1];

	/* zero out the buffer */
	memset(buffer, '\0', BUFSIZ + 1);

	/* add ".gz" to the end of the file */
	if ((strlen(filename) + 3) < PATH_MAX)
		sprintf(gzfilename, "%s.gz", filename);
	else {
		eprintf("error", "pathname for compressed file `%s.gz' is too long", filename);
		return(1);
	}

	/* try to open the file for reading */
	if ((fd = fopen(filename, "r")) == NULL) {
#ifdef USE_DEBUG
		eprintf("debug", strerror(errno));
#endif
		eprintf("error", "unable to open file `%s' for compression stage", filename);
		return(1);
	}

	/* try to create the compressed file at maximum compression */
	if ((gzfd = gzopen(gzfilename, "wb9")) == NULL) {
#ifdef USE_DEBUG
		eprintf("debug", strerror(errno));
#endif
		eprintf("error", "unable to open compressed file `%s' for compression stage", gzfilename);
		fclose(fd);
		return(1);
	}

	/* read from the uncompressed file until we hit EOF */
	while (fread(buffer, sizeof(char), BUFSIZ, fd) > 0) {

		/* output and compress the line we read to the compressed file */
		if (!gzwrite(gzfd, buffer, BUFSIZ)) {
			eprintf("error", "unable to output compressed data to compressed file `%s'", gzfilename);
			gzclose(gzfd);
			fclose(fd);
			return(1);
		}
	}

	/* test to see if there were any errors while reading in data */
	if (ferror(fd)) {
		eprintf("error", "unable to read data from file `%s'", filename);
		gzclose(gzfd);
		fclose(fd);
		return(1);
	}

	/* close both files */
	gzclose(gzfd);
	fclose(fd);

	/* delete the original uncompressed file */
	if (unlink(filename) == -1) {
#ifdef USE_DEBUG
		eprintf("debug", strerror(errno));
#endif
		eprintf("notice", "unable to delete file `%s' after compression stage", filename);
	}

	return(0);
}
#endif

int mkdirs(char *pathname) {
	int fd;
	char *tmpdir;
	char *curdir = pathname;

	/* if there's no directories to create, return to main */
	if (!(tmpdir = strchr(pathname, '/')))
		return(0);

	/* save a link to our current working directory */
	fd = open(".", O_RDONLY);

	/* for each directory in the path, create it if it doesn't exist */
	while ((tmpdir = strchr(curdir, '/'))) {

		/* isolate the next directory to create */
		*tmpdir = '\0';

		/* attempt to create the required directory, and cd into it */
		if (curdir == pathname)
			chdir("/");
		else {
			mkdir(curdir, 0755);
			chdir(curdir);
		}

		/* reset pathname to it's original state */
		*tmpdir = '/';
		curdir = tmpdir + 1;
	}

	/* change back to the original base directory */
	fchdir(fd);
	close(fd);

	return(0);
}

int eprintf(const char *type, const char *format, ...) {
	va_list ap;
	char errmsg[BUFSIZ + 1];

	/* get the current date and time */
	time_t  epoch = time(NULL);
	char *curtime = ctime(&epoch);

	/* cut off the '\n' at the end of ctime()'s output */
	curtime[strlen(curtime) - 1] = '\0';

	va_start(ap, format);
	vsprintf(errmsg, format, ap);
	va_end(ap);

	return(fprintf(stderr, "[%s] [%s] httplog: %s\n", curtime, type, errmsg));
}

int parsetags(char *output, size_t max, const char *input) {
	char *outptr = output;
	const char *curptr = NULL;
	const char *oldptr = input;

	/* search input for special tags */
	while ((curptr = strchr(oldptr, '%')) != NULL) {

		/* copy everything from last tag to current tag */
		strncpy(outptr, oldptr, min(max - strlen(output), curptr - oldptr));

		/* update output pointer */
		outptr += strlen(outptr);

		/* parse current tag */
		switch ((char)*(curptr + 1)) {
			case '1': {
				char *ptr;
				char *fqdn;

				/* output host name */
				if ((fqdn = getlocalfqdn()) != NULL) {
					if ((ptr = strchr(fqdn, '.')) != NULL)
						*ptr = '\0';
					strncpy(outptr, fqdn, min(max - strlen(output), strlen(fqdn)));
				}
				else
					strncpy(outptr, "none", min(max - strlen(output), 4 * sizeof(char)));
				break;
			}
			case '2': {
				char *ptr;
				char *fqdn;

				/* output domain name */
				if ((fqdn = getlocalfqdn()) != NULL) {
					if ((ptr = strchr(fqdn, '.')) != NULL)
						fqdn = ptr + 1;
					strncpy(outptr, fqdn, min(max - strlen(output), strlen(fqdn)));
				}
				else
					strncpy(outptr, "none", min(max - strlen(output), 4 * sizeof(char)));
				break;
			}
			case '3': {
				char *fqdn;

				/* output full domain name */
				if ((fqdn = getlocalfqdn()) != NULL)
					strncpy(outptr, fqdn, min(max - strlen(output), strlen(fqdn)));
				else
					strncpy(outptr, "none", min(max - strlen(output), 4 * sizeof(char)));
				break;
			}
			default: {
				/* copy the unknown tag to the output */
				strncpy(outptr, curptr, min(max - strlen(output), 2 * sizeof(char)));
			}
		}

		/* update tag pointers */
		oldptr = curptr + 2;

		/* update output pointer */
		outptr += strlen(outptr);
	}

	/* copy everything left over from input */
	strncpy(outptr, oldptr, min(max - strlen(output), strlen(oldptr)));

	return(strlen(output));
}

void sighandler(int signal) {

	/* handle the appropriate signal properly */
	switch (signal) {
		case SIGHUP: {
			/* flush all data for all open streams */
			eprintf("notice", "SIGHUP received.  Flushing buffers");
			fflush(NULL);
			break;
		}
		case SIGTERM: {
			/* flush all data for all open streams */
			eprintf("notice", "SIGTERM received.  Flushing buffers and exiting");
			fflush(NULL);
			exit(0);
			break;
		}
#ifdef USE_ZLIB
		case SIGCHLD: {
			/* free up resources tied up by any zombie child process */
			waitpid(-1, NULL, WNOHANG);
			break;
		}
#endif
	}

	return;
}

char *getlocalfqdn(void) {
	char hostname[256];
	struct hostent *hostinfo;

	gethostname(hostname, sizeof(hostname));
	if ((hostinfo = gethostbyname(hostname)) != NULL)
		return(hostinfo->h_name);
	else
		return(NULL);
}
