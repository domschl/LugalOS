#ifndef LUGALOS_DRIVERS_WINCLOSE_H
#define LUGALOS_DRIVERS_WINCLOSE_H

#include <stdint.h>

/* The close protocol for canvas windows (drivers/winclose.c).
 *
 * A canvas window whose program reads its events is not closed by its close
 * box or Cmd-W: the program is sent WIN_EV_CLOSE_REQUEST and closes it
 * itself. If the window is still there after 200 ms and the program has
 * not read the request -- or 5 s after it did, closing slowly -- an alert
 * over the launcher says the program is not responding:
 *   Wait          asked again in 5 s;
 *   Close Window  the window goes, its canvas is kept for the program, which
 *                 may still draw into it, and freed when the program lets go
 *                 of it or its task ends.
 * The program keeps its session claim either way: a kernel task cannot be
 * stopped from outside safely, so a second start says it is in use rather
 * than share a stuck program's state.
 *
 * winclose_watch(): canvas cid's program (task pid) was just asked. From
 * any task; the watch runs on the views task. */
void winclose_watch(uint8_t cid, int pid);
/* `launcher status`: the windows watched, and the alerts shown. */
void winclose_report(void);

#endif /* LUGALOS_DRIVERS_WINCLOSE_H */
