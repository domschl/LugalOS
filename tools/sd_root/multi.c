/* Regression fixture for four bugs that every other fixture here missed,
 * because every other fixture is a single function called main() with an
 * empty parameter list and a <lugal.h> include that never touches the VFS.
 *
 *   - `(void)` parameter lists: the parser took the closing paren as the
 *     parameter's name, walked out of the parameter list and off through the
 *     rest of the file, and looped forever on a corrupted `locals` chain
 *     until UBSan halted the board.
 *   - two or more named parameters: Obj::next carried both the locals chain
 *     and the params chain, so the second parameter closed a cycle.
 *   - more than one function: the node and object pools were reset at the
 *     top of every function, so only the last function kept an intact AST.
 *     It compiled clean and returned the wrong answer.
 *   - a quoted #include actually read from the filesystem: the read was
 *     bounded by sizeof(a pointer), so it saw the first 3 bytes of the file.
 *
 * The values are chosen so that a wrong answer is a different number rather
 * than no output -- silence and 0 were both symptoms here, and neither is
 * distinguishable from "the program did not run" without a real value. */
#include "multi.h"

int seven(void) { return 7; }

int add(int a, int b) { return a + b; }

int tri(int a, int b, int c) { return a + b + c; }

int main(void) {
    /* 7 + 35 = 42, then 100 + 20 + 3 = 123, then HDR_ANSWER from multi.h. */
    putnum(add(seven(), 35));
    putchar(45);
    putnum(tri(100, 20, 3));
    putchar(45);
    putnum(hdr_double(HDR_ANSWER));
    return 0;
}
