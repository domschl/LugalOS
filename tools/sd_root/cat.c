#include <lugal.h>

main() {
    char buf[256];
    int bytes = read_file("/proc/ps", buf, 255);
    if (bytes > 0) {
        buf[bytes] = 0;
        print(buf);
    }
}
