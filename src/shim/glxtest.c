#include <GL/glx.h>
#include <GL/gl.h>
#include <stdio.h>
#include <unistd.h>
typedef GLXContext (*cca_t)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
int main(void) {
    alarm(15);
    Display *d = XOpenDisplay(NULL); if (!d) { puts("no display"); return 1; }
    int a[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DOUBLEBUFFER, True, None }, n;
    GLXFBConfig *c = glXChooseFBConfig(d, DefaultScreen(d), a, &n); printf("configs=%d\n", n); fflush(stdout);
    cca_t cca = (cca_t)glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");
    int ca[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, None };
    GLXContext ctx = cca(d, c[0], NULL, True, ca); printf("ctx=%p\n", (void*)ctx); fflush(stdout);
    XVisualInfo *vi = glXGetVisualFromFBConfig(d, c[0]);
    XSetWindowAttributes swa = { .colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone) };
    Window w = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 64, 64, 0, vi->depth, InputOutput, vi->visual, CWColormap, &swa);
    glXMakeCurrent(d, w, ctx);
    printf("renderer=%s\nversion=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glClear(GL_COLOR_BUFFER_BIT); glFinish(); puts("finish ok");
    return 0;
}
