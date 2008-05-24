#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mount.h>

static bool do_debug = false;

static void
debug(const char *fmt, ...)
{
	va_list ap;

	if (!do_debug)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool
do_mount(const char *device, const char *dir)
{
	pid_t pid;
	pid_t wpid;
	int status;

	if (!device || !dir)
		return false;

	pid = fork();
	if (pid < 0) {
		/* Error */
		return false;
	} else if (pid > 0) {
		/* We're in the parent process */
		do {
			wpid = waitpid(pid, &status, 0);
		} while (!WIFEXITED(status) && !WIFSIGNALED(status));
		if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS)
			return true;
		return false;
	} else {
		/* We're in the child process */
		debug("Mounting %s at %s\n", device, dir);
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		open("/dev/null", O_RDONLY, 0);
		open("/dev/null", O_WRONLY, 0);
		open("/dev/null", O_WRONLY, 0);
		execl("/bin/mount", "/bin/mount", "-n", "-t",
		      "ext3",
		      /*"ext3,ext2,vfat,reiserfs,xfs,isofs,udf",*/
		      "-o", "noatime,nodiratime,nodev,noexec,nosuid,ro",
		      device, dir, (char *)NULL);

		/* If execl works, we won't end up here */
		exit(EXIT_FAILURE);
	}
}

int
main(int argc, char **argv, char **envp)
{
	char *debugval;
	char *devpath;
	char *filepath;
	struct stat st;
	char *tmppath;
	char tpath[] = "/tmp/passdev.XXXXXX";
	char *keypath;
	int fd;
	size_t toread;
	size_t bytesread;
	char *keybuffer;
	size_t towrite;
	size_t byteswritten;
	ssize_t bytes;

	/* We only take one argument */
	if (argc != 2) {
		fprintf(stderr, "Incorrect number of arguments");
		goto error;
	}

	/* If DEBUG=1 is in the environment, enable debug messages */
	debugval = getenv("DEBUG");
	if (debugval && atoi(debugval) > 0)
		do_debug = true;

	/* Split string into device and path */
	devpath = argv[1];
	filepath = strchr(devpath, ':');
	if (!filepath || !(*filepath) || !(*(filepath + 1))) {
		fprintf(stderr, "Invalid key path");
		goto error;
	}
	*filepath = '\0';
	filepath++;
	debug("Path is %p and filepath is %p\n", devpath, filepath);

	/* Wait until device is available */
	if (access(devpath, F_OK)) {
		debug("Waiting for %s\n", devpath);
		while(access(devpath, F_OK))
			sleep(1);
	}

	/* Make sure device is a blockdev */
	if (stat(devpath, &st)) {
		fprintf(stderr, "Unable to stat %s\n", devpath);
		goto error;
	} else if (!S_ISBLK(st.st_mode)) {
		fprintf(stderr, "%s is no block device\n", devpath);
		goto error;
	}

	/* Create a tmp dir where we mount the device */
	tmppath = mkdtemp(tpath);
	if (!tmppath) {
		fprintf(stderr, "Failed to create temporary directory\n");
		goto error;
	}

	/* Ok, mount it */
	if (!do_mount(devpath, tmppath)) {
		fprintf(stderr, "Failed to mount %s\n", devpath);
		goto error_rmdir;
	}

	/* Generate the full path to the keyfile */
	keypath = malloc(strlen(tmppath) + 1 + strlen(filepath) + 1);
	if (!keypath) {
		fprintf(stderr, "Failed to allocate memory\n");
		goto error_umount;
	}
	sprintf(keypath, "%s/%s", tmppath, filepath);

	/* Check that the keyfile exists */
	if (access(keypath, F_OK)) {
		fprintf(stderr, "Keyfile doesn't exist\n");
		goto error_free;
	}

	/* Get the size of the keyfile */
	if (stat(keypath, &st)) {
		fprintf(stderr, "Unable to stat keyfile\n");
		goto error_free;
	}

	/* Check the size of the keyfile */
	if (st.st_size < 0) {
		fprintf(stderr, "Invalid keyfile size\n");
		goto error_free;
	}
	toread = (size_t)st.st_size;

	/* Open the keyfile */
	if ((fd = open(keypath, O_RDONLY)) < 0) {
		fprintf(stderr, "Failed to open keyfile\n");
		goto error_free;
	}

	/* Allocate a buffer for the keyfile contents */
	keybuffer = malloc(toread);
	if (!keybuffer) {
		fprintf(stderr, "Failed to allocate memory\n");
		goto error_close;
		exit(EXIT_FAILURE);
	}

	/* Read the keyfile */
	bytesread = 0;
	while (bytesread < toread) {
		bytes = read(fd, keybuffer + bytesread, toread - bytesread);
		if (bytes <= 0) {
			fprintf(stderr, "Failed to read entire key\n");
			goto error_keybuffer;
		}
		bytesread += bytes;
	}

	/* Clean up */
	close(fd);
	free(keypath);
	umount(tmppath);
	rmdir(tmppath);

	/* Write result */
	byteswritten = 0;
	towrite = toread;
	while (byteswritten < towrite) {
		bytes = write(STDOUT_FILENO, keybuffer + byteswritten,
			      towrite - byteswritten);
		if (bytes <= 0) {
			fprintf(stderr, "Failed to write entire key\n");
			memset(keybuffer, 0, toread);
			free(keybuffer);
			goto error;
		}
		byteswritten += bytes;
	}

	/* Clean up */
	memset(keybuffer, 0, toread);
	free(keybuffer);

	/* Done */
	exit(EXIT_SUCCESS);

	/* Error handling */
error_keybuffer:
	memset(keybuffer, 0, toread);
	free(keybuffer);
error_close:
	close(fd);
error_free:
	free(keypath);
error_umount:
	umount(tmppath);
error_rmdir:
	rmdir(tmppath);
error:
	exit(EXIT_FAILURE);
}

