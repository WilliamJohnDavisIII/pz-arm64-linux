#include <X11/Xlib.h>
__attribute__((visibility("default"))) char init;
__attribute__((constructor)) static void pz_xinit(void) { XInitThreads(); init = 1; }
