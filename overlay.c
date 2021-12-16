// Spawn a subprocess in a new mount namespace with added bind mounts.
// Invoke as
//   overlay dir/ exe args...
// 'exe' will see dir/ overlaid onto /.
//
// Will be obsoleted by kernel 5.11 which permits unprivileged overlayfs mounts.

#define _GNU_SOURCE
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int pivot_root(const char *new_root, const char *put_old);

#define CHKSYS(expr) ({long _ret = (expr); if (_ret < 0) err(255, "[%s:%d] syscall failed: %s", __FILE__, __LINE__, #expr); _ret;})

const int debug = 0;

const int kOpendirFlags = O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC;

struct dirent *nextent(DIR *dirp) {
  for (;;) {
    errno = 0;
    struct dirent *d = readdir(dirp);
    if (!d) {
      if (errno) err(1, "readdir");
      return d;
    }
    const char *n = d->d_name;
    if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
    return d;
  }
}

// "buf" most have size greater than PATH_MAX.  Crashes on error.
static void do_readlinkat(int dirfd, const char *pathname, char *buf) {
  int r = CHKSYS(readlinkat(dirfd, pathname, buf, PATH_MAX + 1));
  if (r > PATH_MAX) err(1, "readlinkat result too large for '%s'", pathname);
  buf[r] = 0;
}

// Requires cwd to be /proc/self/fd.
static void bindmntat(int srcdir, const char *srcname, int dstdir, const char *dstname) {
  // Trick to bind-mount symlinks as source or target,
  // from https://www.lkml.org/lkml/2019/12/30/14
  int srcpathfd = CHKSYS(openat(srcdir, srcname, O_PATH|O_NOFOLLOW|O_CLOEXEC));
  int dstpathfd = CHKSYS(openat(dstdir, dstname, O_PATH|O_NOFOLLOW|O_CLOEXEC));
  char src[32], dst[32];
  sprintf(src, "%d", srcpathfd);
  sprintf(dst, "%d", dstpathfd);
  CHKSYS(mount(src, dst, NULL, MS_BIND|MS_REC, NULL));
  CHKSYS(close(dstpathfd));
  CHKSYS(close(srcpathfd));
}

enum State {
  kRoot = 0,
  kBase = 1,
  kSkeleton = 2,
};

// Weave two directories identified by fds "base" and "top" into a directory
// named "outname" beneath "outfd".  "base" and "top" will be closed.
static void merge(int base, int top, int outfd, const char *outname, enum State state) {
  static char outpath[PATH_MAX] = "/";
  char *tail = strchr(outpath, '\0');
  DIR *basedir = fdopendir(base);
  DIR *topdir = fdopendir(top);
  struct dirent *d;
  char buf[PATH_MAX+1];
  // Check if each entry in top can be bind-mounted atop the corresponding
  // entry in base.  If not, we'll have to explode base into a new directory
  // and bind-mount each item individually.
  int explode = 0;
  while ((d = nextent(topdir))) {
    struct stat st;
    if (fstatat(base, d->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
        (d->d_type == DT_DIR) != (IFTODT(st.st_mode) == DT_DIR)) {
      if (debug) warnx("exploding '%s' due to entry '%s'", outpath, d->d_name);
      explode = 1;
      break;
    }
  }
  rewinddir(topdir);
  if (explode) {
    const char *target = outname;
    if (outfd >= 0) {  // otherwise accept AT_FDCWD
      sprintf(buf, "%d/%s", outfd, outname);
      target = buf;
    }
    if (state != kSkeleton) {
      CHKSYS(mount(NULL, target, "tmpfs", 0, "mode=755"));
    }
    int out = CHKSYS(openat(outfd, outname, kOpendirFlags));
    while ((d = nextent(topdir))) {
      if (d->d_type == DT_DIR) {
        CHKSYS(mkdirat(out, d->d_name, 0755));
        int b = openat(base, d->d_name, kOpendirFlags);
        if (b < 0) {
          bindmntat(top, d->d_name, out, d->d_name);
          continue;
        }
        // This directory exists in both base and top.
        int t = CHKSYS(openat(top, d->d_name, kOpendirFlags));
        sprintf(tail, "%s/", d->d_name);
        merge(b, t, out, d->d_name, kSkeleton);
        *tail = 0;
      } else if (d->d_type == DT_LNK) {
        do_readlinkat(top, d->d_name, buf);
        CHKSYS(symlinkat(buf, out, d->d_name));
      } else {
        CHKSYS(mknodat(out, d->d_name, S_IFREG|0644, 0));
        bindmntat(top, d->d_name, out, d->d_name);
      }
    }
    while ((d = nextent(basedir))) {
      if (d->d_type == DT_DIR) {
        if (mkdirat(out, d->d_name, 0755)) {
          if (errno == EEXIST) continue;  // created for top
          err(1, "mkdir of base entry '%s'", d->d_name);
        }
        bindmntat(base, d->d_name, out, d->d_name);
      } else if (d->d_type == DT_LNK) {
        do_readlinkat(base, d->d_name, buf);
        if (symlinkat(buf, out, d->d_name)) {
          if (errno == EEXIST) continue;  // created for top
          err(1, "symlink of base entry '%s'", d->d_name);
        }
      } else {
        if (mknodat(out, d->d_name, S_IFREG|0644, 0)) {
          if (errno == EEXIST) continue;  // created for top
          err(1, "mknod of base entry '%s'", d->d_name);
        }
        bindmntat(base, d->d_name, out, d->d_name);
      }
    }
    int mode = faccessat(base, ".", W_OK, 0) ? 0555 : 01777;
    if (debug) warnx("mode %04o '%s'", mode, outpath);
    CHKSYS(fchmod(out, mode));
    CHKSYS(close(out));
  } else {
    if (debug) warnx("stacking  '%s'", outpath);
    if (state != kBase) {
      bindmntat(base, ".", outfd, outname);
    }
    int out = CHKSYS(openat(outfd, outname, kOpendirFlags));
    while ((d = nextent(topdir))) {
      if (d->d_type == DT_DIR) {
        // This directory exists in both base and top.
        int b = CHKSYS(openat(base, d->d_name, kOpendirFlags));
        int t = CHKSYS(openat(top, d->d_name, kOpendirFlags));
        sprintf(tail, "%s/", d->d_name);
        merge(b, t, out, d->d_name, kBase);
        *tail = 0;
      } else {
        bindmntat(top, d->d_name, out, d->d_name);
      }
    }
    CHKSYS(close(out));
  }
  CHKSYS(closedir(topdir));
  CHKSYS(closedir(basedir));
}

int main(int argc, char **argv) {
  int e = 2;
  while (e < argc && strchr(argv[e], '=')) putenv(argv[e++]);
  if (e >= argc) errx(1, "usage: overlay tree [env...] exe args...");
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof cwd)) err(1, "getcwd");

  // User namespace setup.
  int uid = geteuid();
  int gid = getegid();
  CHKSYS(unshare(CLONE_NEWUSER|CLONE_NEWNS));
  int fd = CHKSYS(open("/proc/self/setgroups", O_WRONLY));
  CHKSYS(write(fd, "deny", 4));
  CHKSYS(close(fd));
  char buf[64];
  int len = sprintf(buf, "%d %d 1", uid, uid);
  fd = CHKSYS(open("/proc/self/uid_map", O_WRONLY));
  CHKSYS(write(fd, buf, len));
  CHKSYS(close(fd));
  len = sprintf(buf, "%d %d 1", gid, gid);
  fd = CHKSYS(open("/proc/self/gid_map", O_WRONLY));
  CHKSYS(write(fd, buf, len));
  CHKSYS(close(fd));

  // Open input trees.
  int oldroot = CHKSYS(open("/", kOpendirFlags));
  int overlay = CHKSYS(open(argv[1], kOpendirFlags));
  CHKSYS(chdir("/proc/self/fd"));

  // Rearrange mounts so our working area does not obscure the input.
  CHKSYS(mount(NULL, "/tmp", "tmpfs", 0, "mode=755"));
  CHKSYS(mkdir("/tmp/newroot", 0755));
  CHKSYS(mkdir("/tmp/oldroot", 0755));
  CHKSYS(pivot_root("/tmp", "/tmp/oldroot"));

  // Merge the supplied tree with /, writing/binding into a new directory.
  merge(oldroot, overlay, AT_FDCWD, "/newroot", kRoot);

  // Enter the new tree.
  CHKSYS(pivot_root("/newroot", "/newroot"));
  CHKSYS(umount2("/", MNT_DETACH));

  // Return to initial working directory and chain to the next program.
  CHKSYS(chdir(cwd));
  execvp(argv[e], argv+e);
  err(255, "execvp '%s'", argv[e]);
}
