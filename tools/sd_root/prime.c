#include <lugal.h>

main() {
    print("Prime numbers up to 30:\n");
    int i = 2;
    while (i <= 30) {
        int is_p = 1;
        int j = 2;
        while (j * j <= i) {
            if (i % j == 0) { is_p = 0; break; }
            j++;
        }
        if (is_p) {
            putnum(i);
            putchar(' ');
        }
        i++;
    }
    putchar('\n');
}
