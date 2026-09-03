/* Read through vfs_read() by a quoted #include -- deliberately more than a
 * handful of bytes, so a length bounded by sizeof(char *) cannot pass. */
#define HDR_ANSWER 21

int hdr_double(int n) { return n + n; }
