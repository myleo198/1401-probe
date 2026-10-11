// build check for drmlog.so: one real ioctl() call through the libc symbol (as libdrm makes them)
#include <fcntl.h>
#include <sys/ioctl.h>
int main(void) { int n = 0, fd = open("/dev/null", O_RDONLY); ioctl(fd, FIONREAD, &n); return 0; }
