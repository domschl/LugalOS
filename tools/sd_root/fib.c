#include <lugal.h>

fib(n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

main() {
    print("Fibonacci sequence:\n");
    int i = 0;
    while (i <= 10) {
        putnum(fib(i));
        putchar(' ');
        i++;
    }
    putchar('\n');
}
