// fs_test.c -- exercises open/write/read/lseek/fstat/stat/unlink,
// chdir/getcwd, mkdir/opendir/readdir/rmdir, symlink/readlink, and
// timestamps against the EXT2 image mounted by ext4_shim.c. Proves the
// musl -> posix_shim -> ext4_shim -> lwext4 file I/O path works end to
// end.
// build with build-app.sh
//
// This is also a valid *nix program of course (run it against a scratch
// directory, not anything you care about -- it creates/removes files).

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#define TEST_PATH "/fs_test.txt"
#define TEST_PATH_RELATIVE "fs_test_relative.txt"

static const char msg[] = "Hello, EXT2 from lwext4!\n";
static const char more[] = "more data\n";

// Recursively print every entry under path, indented by depth. Uses
// lstat() (not stat()) so symlinks are reported as links rather than
// followed through to their target.
static void list_tree(const char *path, int depth)
{
	DIR *dir = opendir(path);
	if (!dir) {
		printf("opendir(%s) failed: %s\n", path, strerror(errno));
		return;
	}

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		char child[512];
		size_t plen = strlen(path);
		int need_slash = plen == 0 || path[plen - 1] != '/';
		snprintf(child, sizeof(child), "%s%s%s", path, need_slash ? "/" : "", de->d_name);

		struct stat st;
		if (lstat(child, &st) < 0) {
			printf("%*slstat(%s) failed: %s\n", depth * 2, "", child, strerror(errno));
			continue;
		}

		printf("%*s", depth * 2, "");
		if (S_ISDIR(st.st_mode)) {
			printf("%s/\n", child);
			list_tree(child, depth + 1);
		} else if (S_ISLNK(st.st_mode)) {
			char target[512] = {0};
			ssize_t n = readlink(child, target, sizeof(target) - 1);
			printf("%s -> %s\n", child, n >= 0 ? target : "?");
		} else {
			printf("%s (%ld bytes)\n", child, (long)st.st_size);
		}
	}

	closedir(dir);
}

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

	// Relative path -- cwd defaults to "/", so this should land at
	// the filesystem root just like an absolute path (chdir() itself
	// is exercised separately below).
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

	// Timestamps -- boot-relative but no longer always zero.
	fd = open(TEST_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	write(fd, msg, strlen(msg));
	close(fd);
	if (stat(TEST_PATH, &st) < 0) {
		printf("stat() after recreate failed: %s\n", strerror(errno));
		return 1;
	}
	if (st.st_mtime == 0 || st.st_ctime == 0 || st.st_atime == 0) {
		printf("timestamps are still all zero -- expected boot-relative non-zero values\n");
		return 1;
	}
	unlink(TEST_PATH);

	// cwd: chdir() into a fresh directory, create a file with a
	// relative path, and confirm it landed there (not at the root).
	const char *cwd_dir = "/cwd_test";
	const char *cwd_rel = "rel.txt";
	const char *cwd_abs = "/cwd_test/rel.txt";

	if (mkdir(cwd_dir, 0755) < 0) {
		printf("mkdir() failed: %s\n", strerror(errno));
		return 1;
	}
	if (chdir(cwd_dir) < 0) {
		printf("chdir() failed: %s\n", strerror(errno));
		return 1;
	}

	char cwdbuf[256];
	if (!getcwd(cwdbuf, sizeof(cwdbuf)) || strcmp(cwdbuf, cwd_dir) != 0) {
		printf("getcwd() returned \"%s\", expected \"%s\"\n", cwdbuf, cwd_dir);
		return 1;
	}

	fd = open(cwd_rel, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("open() of a relative path under chdir() failed: %s\n", strerror(errno));
		return 1;
	}
	write(fd, "x", 1);
	close(fd);

	if (stat(cwd_abs, &st) < 0) {
		printf("relative path wasn't created under the chdir()'d directory\n");
		return 1;
	}

	chdir("/");
	unlink(cwd_abs);
	if (rmdir(cwd_dir) < 0) {
		printf("rmdir() failed: %s\n", strerror(errno));
		return 1;
	}

	// mkdir/opendir/readdir/rmdir
	const char *dir_path = "/dir_test";
	if (mkdir(dir_path, 0755) < 0) {
		printf("mkdir() failed: %s\n", strerror(errno));
		return 1;
	}

	fd = open("/dir_test/a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("open() of a file under a fresh directory failed: %s\n", strerror(errno));
		return 1;
	}
	write(fd, "a", 1);
	close(fd);
	fd = open("/dir_test/b.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("open() of a second file under a fresh directory failed: %s\n", strerror(errno));
		return 1;
	}
	write(fd, "b", 1);
	close(fd);

	DIR *dir = opendir(dir_path);
	if (!dir) {
		printf("opendir() failed: %s\n", strerror(errno));
		return 1;
	}

	int saw_a = 0, saw_b = 0, saw_dot = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, "a.txt") == 0)
			saw_a = 1;
		else if (strcmp(de->d_name, "b.txt") == 0)
			saw_b = 1;
		else if (strcmp(de->d_name, ".") == 0)
			saw_dot = 1;
	}
	closedir(dir);

	if (!saw_a || !saw_b || !saw_dot) {
		printf("readdir() didn't see all expected entries (a=%d b=%d dot=%d)\n",
		       saw_a, saw_b, saw_dot);
		return 1;
	}

	unlink("/dir_test/a.txt");
	unlink("/dir_test/b.txt");
	if (rmdir(dir_path) < 0) {
		printf("rmdir() of a populated-then-emptied directory failed: %s\n", strerror(errno));
		return 1;
	}

	// symlink: lstat() must report the link itself, stat() must
	// follow it through to the real file's contents/size.
	const char *link_target = "/link_target.txt";
	const char *link_path = "/link_test";

	fd = open(link_target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	write(fd, msg, strlen(msg));
	close(fd);

	if (symlink(link_target, link_path) < 0) {
		printf("symlink() failed: %s\n", strerror(errno));
		return 1;
	}

	if (lstat(link_path, &st) < 0 || !S_ISLNK(st.st_mode)) {
		printf("lstat() on a symlink didn't report S_ISLNK\n");
		return 1;
	}

	if (stat(link_path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != (off_t)strlen(msg)) {
		printf("stat() on a symlink didn't follow through to the target file\n");
		return 1;
	}

	char linkbuf[256] = {0};
	ssize_t ln = readlink(link_path, linkbuf, sizeof(linkbuf) - 1);
	if (ln < 0 || strcmp(linkbuf, link_target) != 0) {
		printf("readlink() returned \"%s\", expected \"%s\"\n", linkbuf, link_target);
		return 1;
	}

	unlink(link_path);
	unlink(link_target);

	printf("all filesystem tests passed\n");

	printf("\nFilesystem contents:\n");
	list_tree("/", 0);

	return 0;
}
