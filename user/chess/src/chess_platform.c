#include "chess_platform.h"

int atoi(const char *s) {
    int sign = 1;
    int val = 0;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return sign * val;
}

static uint64_t g_chess_rand_state = 88172645463325252ULL;

void srand(unsigned int seed) {
    g_chess_rand_state = seed ? seed : 1;
}

long rand(void) {
    g_chess_rand_state ^= g_chess_rand_state << 13;
    g_chess_rand_state ^= g_chess_rand_state >> 7;
    g_chess_rand_state ^= g_chess_rand_state << 17;
    return (long)(g_chess_rand_state & 0x7fffffffUL);
}
