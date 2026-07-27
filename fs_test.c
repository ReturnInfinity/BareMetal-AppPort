// fs_test.c -- exercises open/write/read/lseek/fstat/stat/unlink against
// the EXT2 image mounted by ext4_shim.c. Proves the musl -> posix_shim ->
// ext4_shim -> lwext4 file I/O path works end to end.
// build with build-app.sh
//
// This is also a valid *nix program of course (run it against a scratch
// directory, not anything you care about -- it creates/removes files).

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define TEST_PATH "/fs_test.txt"
#define TEST_PATH_RELATIVE "fs_test_relative.txt"

static const char msg[] = "Hello, EXT2 from lwext4!\n";
static const char more[] = "more data\n";

int main(void)
{
	// Start from a known state in case a previous run left the file
	// behind.
	unlink(TEST_PATH);
	unlink("/" TEST_PATH_RELATIVE);

	// Create + write
	int fd = open(TEST_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("open(O_CREAT) failed: %s\n", strerror(errno));
		return 1;
	}

	ssize_t n = write(fd, msg, strlen(msg));
	if (n != (ssize_t)strlen(msg)) {
		printf("write() wrote %zd bytes, expected %zu\n", n, strlen(msg));
		return 1;
	}

	if (close(fd) < 0) {
		printf("close() after write failed: %s\n", strerror(errno));
		return 1;
	}

	// Reopen + read back
	fd = open(TEST_PATH, O_RDONLY);
	if (fd < 0) {
		printf("open(O_RDONLY) failed: %s\n", strerror(errno));
		return 1;
	}

	char buf[256] = {0};
	n = read(fd, buf, sizeof(buf) - 1);
	if (n != (ssize_t)strlen(msg) || strcmp(buf, msg) != 0) {
		printf("read() back didn't match what was written\n");
		return 1;
	}

	// fstat
	struct stat st;
	if (fstat(fd, &st) < 0) {
		printf("fstat() failed: %s\n", strerror(errno));
		return 1;
	}
	if (!S_ISREG(st.st_mode) || st.st_size != (off_t)strlen(msg)) {
		printf("fstat() reported mode 0%o size %ld, expected a regular file of size %zu\n",
		       st.st_mode, (long)st.st_size, strlen(msg));
		return 1;
	}

	close(fd);

	// lseek + partial read
	fd = open(TEST_PATH, O_RDONLY);
	if (lseek(fd, 7, SEEK_SET) != 7) {
		printf("lseek(SEEK_SET, 7) failed: %s\n", strerror(errno));
		return 1;
	}
	memset(buf, 0, sizeof(buf));
	n = read(fd, buf, 5);
	if (n != 5 || strncmp(buf, "EXT2 ", 5) != 0) {
		printf("read() after lseek() returned unexpected data\n");
		return 1;
	}
	close(fd);

	// stat() by path
	if (stat(TEST_PATH, &st) < 0) {
		printf("stat() failed: %s\n", strerror(errno));
		return 1;
	}
	if (st.st_size != (off_t)strlen(msg)) {
		printf("stat() size %ld, expected %zu\n", (long)st.st_size, strlen(msg));
		return 1;
	}

	// O_APPEND
	fd = open(TEST_PATH, O_WRONLY | O_APPEND);
	if (fd < 0) {
		printf("open(O_APPEND) failed: %s\n", strerror(errno));
		return 1;
	}
	if (write(fd, more, strlen(more)) != (ssize_t)strlen(more)) {
		printf("append write() failed: %s\n", strerror(errno));
		return 1;
	}
	close(fd);

	if (stat(TEST_PATH, &st) < 0 || st.st_size != (off_t)(strlen(msg) + strlen(more))) {
		printf("size after append is wrong\n");
		return 1;
	}

	// Relative path -- no cwd concept on this port, so this should
	// land at the filesystem root, same as an absolute path.
	fd = open(TEST_PATH_RELATIVE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("open() of a relative path failed: %s\n", strerror(errno));
		return 1;
	}
	write(fd, "x", 1);
	close(fd);
	if (stat("/" TEST_PATH_RELATIVE, &st) < 0) {
		printf("relative path wasn't created at the filesystem root\n");
		return 1;
	}
	unlink("/" TEST_PATH_RELATIVE);

	// Opening a nonexistent file without O_CREAT must fail with ENOENT
	fd = open("/no_such_file.txt", O_RDONLY);
	if (fd >= 0) {
		printf("open() of a nonexistent file unexpectedly succeeded\n");
		close(fd);
		return 1;
	}
	if (errno != ENOENT) {
		printf("open() of a nonexistent file failed with %s, expected ENOENT\n", strerror(errno));
		return 1;
	}

	// unlink, then confirm it's really gone
	if (unlink(TEST_PATH) < 0) {
		printf("unlink() failed: %s\n", strerror(errno));
		return 1;
	}
	fd = open(TEST_PATH, O_RDONLY);
	if (fd >= 0) {
		printf("open() succeeded after unlink()\n");
		close(fd);
		return 1;
	}

	printf("all filesystem tests passed\n");
	return 0;
}
