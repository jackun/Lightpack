#include "KMSGrabber.hpp"
#include "../drmsend/drmsend.h"
#include <cassert>
#include <cstdlib>
#include <cstdarg>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

#include <xcb/xcb.h>

#include <sys/wait.h>
#include <cstdio>

#include <thread>
#include <chrono>
#include <vector>
#include <iostream>
#include <fstream>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

//#include <xf86drm.h>
//#include <libdrm/drm_fourcc.h>
//#include <xf86drmMode.h>

#include <GLES2/gl2.h>
//#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
//#include <EGL/eglmesaext.h>


struct vec2 {
    float x;
    float y;
};

struct vec3 {
    float x;
    float y;
    float z;
};

/* structures */
struct Vertex {
    vec3 Position;
    vec2 TexCoord;
};

struct Texel {
    unsigned char r, g, b, a;
};

#define EGLCHECK(x) \
    x; \
    if (glGetError() != GL_NO_ERROR) \
        fprintf(stderr, "error on line %d: '%s;': %s\n", __LINE__, #x, getEGLErrorString(eglGetError()));

typedef struct {
    drmsend_response_t resp;
    int fb_fds[OBS_DRMSEND_MAX_FRAMEBUFFERS];
} dmabuf_source_fblist_t;

struct dmabuf_source_t {
    GLuint texture;
    EGLDisplay edisp;
    EGLImage eimage;

    dmabuf_source_fblist_t fbs;
    int active_fb;

    bool show_cursor;
};

static int dmabuf_source_receive_framebuffers(dmabuf_source_fblist_t *list)
{
    const char *dri_filename = "/dev/dri/card0"; // FIXME
    const char *drmsend_filename = "./drmsend"; // FIXME

    fprintf( stderr, "dmabuf_source_receive_framebuffers");

    int retval = 0;
    int sockfd = -1;
    int exited = 0;

    /* Get socket filename */
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    {
        strcpy(addr.sun_path, "/tmp/drmsend.sock");
        fprintf( stderr, "Will bind socket to %s", addr.sun_path);
    }

    /* 1. create and listen on unix socket */
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

    unlink(addr.sun_path);
    if (-1 == bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr))) {
        fprintf( stderr, "Cannot bind unix socket to %s: %d\n",
             addr.sun_path, errno);
        close(sockfd);
        unlink(addr.sun_path);
        return retval;
    }

    if (-1 == listen(sockfd, 1)) {
        fprintf( stderr, "Cannot listen on unix socket bound to %s: %d\n",
             addr.sun_path, errno);
        close(sockfd);
        unlink(addr.sun_path);
        return retval;
    }

    /* 2. run obs-drmsend utility */
    const pid_t drmsend_pid = fork();
    if (drmsend_pid == -1) {
        fprintf( stderr, "Cannot fork(): %d\n", errno);
        close(sockfd);
        unlink(addr.sun_path);
        return retval;
    } else if (drmsend_pid == 0) {
        execlp(drmsend_filename, drmsend_filename, dri_filename,
               addr.sun_path, NULL);
        fprintf(stderr, "Cannot execlp(%s): %d\n", drmsend_filename,
            errno);
        exit(-1);
    }

    fprintf( stderr, "Forked obs-drmsend to pid %d\n", drmsend_pid);

    /* 3. select() on unix socket w/ timeout */
    // FIXME updating timeout w/ time left is linux-specific, other unices might not do that
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sockfd, &set);
        const int maxfd = sockfd;
        const int nfds = select(maxfd + 1, &set, NULL, NULL, &timeout);
        if (nfds > 0) {
            if (FD_ISSET(sockfd, &set))
                break;
        }

        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            fprintf( stderr, "Cannot select(): %d", errno);
            goto child_cleanup;
        }

        if (nfds == 0) {
            fprintf( stderr, "Waiting for drmsend timed out\n");
            goto child_cleanup;
        }
    }

    fprintf( stderr, "Ready to accept");

    /* 4. accept() and receive data */
    int connfd;
    connfd = accept(sockfd, NULL, NULL);
    if (connfd < 0) {
        fprintf( stderr, "Cannot accept unix socket: %d\n", errno);
        goto child_cleanup;
    }

    fprintf( stderr, "Receiving message from obs-drmsend\n");

    for (;;) {
        struct msghdr msg = {0};

        struct iovec io = {
            .iov_base = &list->resp,
            .iov_len = sizeof(list->resp),
        };
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;

        char cmsg_buf[CMSG_SPACE(sizeof(int) *
                     OBS_DRMSEND_MAX_FRAMEBUFFERS)];
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len =
            CMSG_LEN(sizeof(int) * OBS_DRMSEND_MAX_FRAMEBUFFERS);

        // FIXME blocking, may hang if drmsend dies before sending anything
        const ssize_t recvd = recvmsg(connfd, &msg, 0);
        fprintf( stderr, "recvmsg = %d\n", (int)recvd);
        if (recvd <= 0) {
            fprintf( stderr, "cannot recvmsg: %d\n", errno);
            break;
        }

        if (io.iov_len != sizeof(list->resp)) {
            fprintf(stderr,
                 "Received metadata size mismatch: %d received, %d expected\n",
                 (int)io.iov_len, (int)sizeof(list->resp));
            break;
        }

        if (list->resp.tag != OBS_DRMSEND_TAG) {
            fprintf(stderr,
                 "Received metadata tag mismatch: %#x received, %#x expected\n",
                 list->resp.tag, OBS_DRMSEND_TAG);
            break;
        }

        if (cmsg->cmsg_len !=
            CMSG_LEN(sizeof(int) * list->resp.num_framebuffers)) {
            fprintf(stderr,
                 "Received fd size mismatch: %d received, %d expected\n",
                 (int)cmsg->cmsg_len,
                 (int)CMSG_LEN(sizeof(int) *
                       list->resp.num_framebuffers));
            break;
        }

        memcpy(list->fb_fds, CMSG_DATA(cmsg),
               sizeof(int) * list->resp.num_framebuffers);
        retval = 1;
        break;
    }
    close(connfd);

    if (retval) {
        printf(
             "Received %d framebuffers:\n", list->resp.num_framebuffers);
        for (int i = 0; i < list->resp.num_framebuffers; ++i) {
            const drmsend_framebuffer_t *fb =
                list->resp.framebuffers + i;
            printf(
                 "Received width=%d height=%d pitch=%u fourcc=%#x fd=%d\n",
                 fb->width, fb->height, fb->pitch, fb->fourcc,
                 list->fb_fds[i]);
        }
    }

    // TODO consider using separate thread for waitpid() on drmsend_pid
    /* 5. waitpid() on obs-drmsend w/ timeout (poll) */
child_cleanup:
    for (int i = 0; i < 10; ++i) {
        usleep(500 * 1000);
        int wstatus = 0;
        const pid_t p = waitpid(drmsend_pid, &wstatus, WNOHANG);
        if (p == drmsend_pid) {
            if (wstatus == 0 || WIFEXITED(wstatus)) {
                exited = 1;
                const int status = WEXITSTATUS(wstatus);
                if (status != 0)
                    fprintf( stderr, "%s returned %d\n",
                         drmsend_filename, status);
                break;
            }
        } else if (-1 == p) {
            const int err = errno;
            fprintf( stderr, "Cannot waitpid() on drmsend: %d\n", err);
            if (err == ECHILD) {
                exited = 1;
                break;
            }
        }
    }

    if (!exited)
        fprintf( stderr, "Couldn't wait for %s to exit, expect zombies\n",
             drmsend_filename);

    close(sockfd);
    unlink(addr.sun_path);
    return retval;
}

static void dmabuf_source_close(dmabuf_source_t *ctx)
{
    fprintf( stderr, "dmabuf_source_close %p\n", ctx);

    if (ctx->eimage != EGL_NO_IMAGE) {
        eglDestroyImage(ctx->edisp, ctx->eimage);
        ctx->eimage = EGL_NO_IMAGE;
    }

    ctx->active_fb = -1;
}

static void dmabuf_source_open(dmabuf_source_t *ctx, uint32_t fb_id)
{
    fprintf( stderr, "dmabuf_source_open %p %#x\n", ctx, fb_id);
    assert(ctx->active_fb == -1);

    int index = fb_id;
/*    for (index = 0; index < ctx->fbs.resp.num_framebuffers; ++index)
        if (fb_id == ctx->fbs.resp.framebuffers[index].fb_id)
            break;
*/
    if (index == ctx->fbs.resp.num_framebuffers) {
        fprintf( stderr, "Framebuffer id=%#x not found\n", fb_id);
        return;
    }

    fprintf( stderr, "Using framebuffer id=%#x (index=%d)\n", fb_id, index);

    const drmsend_framebuffer_t *fb = ctx->fbs.resp.framebuffers + index;

    fprintf( stderr, "%dx%d %d %d %d", fb->width, fb->height,
         ctx->fbs.fb_fds[index], fb->offset, fb->pitch);

    /* clang-format off */
    // FIXME check for EGL_EXT_image_dma_buf_import
    EGLAttrib eimg_attrs[] = {
        EGL_WIDTH, fb->width,
        EGL_HEIGHT, fb->height,
        EGL_LINUX_DRM_FOURCC_EXT, fb->fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, ctx->fbs.fb_fds[index],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, fb->offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, fb->pitch,
        EGL_NONE
    };
    /* clang-format on */

    ctx->eimage = eglCreateImage(ctx->edisp, EGL_NO_CONTEXT,
                     EGL_LINUX_DMA_BUF_EXT, 0, eimg_attrs);

    if (!ctx->eimage) {
        // FIXME stringify error
        fprintf( stderr, "Cannot create EGLImage: %d\n", eglGetError());
        dmabuf_source_close(ctx);
        goto exit;
    }

    // FIXME handle fourcc?
    glGenTextures(1, &ctx->texture);
    //glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->texture);
    //glBindTexture(GL_TEXTURE_2D, ctx->texture);

    fprintf( stderr, "gltex = %x\n", ctx->texture);
    glBindTexture(GL_TEXTURE_2D, ctx->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ctx->eimage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); //GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); //GL_NEAREST);

    ctx->active_fb = index;

exit:
    return;
}

static void dmabuf_source_update(dmabuf_source_t *ctx)
{
    fprintf( stderr, "dmabuf_source_udpate %p\n", ctx);

    dmabuf_source_close(ctx);
    dmabuf_source_open(ctx, 0);
}

static dmabuf_source_t *dmabuf_source_create(const EGLDisplay edisp)
{
    fprintf( stderr, "dmabuf_source_create");

    if (!glEGLImageTargetTexture2DOES) {
        fprintf( stderr, "GL_OES_EGL_image extension is required\n");
        return NULL;
    }

    dmabuf_source_t *ctx = new dmabuf_source_t();
    ctx->edisp = edisp;
    ctx->active_fb = -1;

#define COUNTOF(a) (sizeof(a) / sizeof(*a))
    for (int i = 0; i < (int)COUNTOF(ctx->fbs.fb_fds); ++i) {
        ctx->fbs.fb_fds[i] = -1;
    }

    if (!dmabuf_source_receive_framebuffers(&ctx->fbs)) {
        fprintf( stderr, "Unable to enumerate DRM/KMS framebuffers\n");
        free(ctx);
        return NULL;
    }


    dmabuf_source_update(ctx);
    return ctx;
}

static void dmabuf_source_close_fds(dmabuf_source_t *ctx)
{
    for (int i = 0; i < ctx->fbs.resp.num_framebuffers; ++i) {
        const int fd = ctx->fbs.fb_fds[i];
        if (fd > 0)
            close(fd);
    }
}

static void dmabuf_source_destroy(dmabuf_source_t *ctx)
{
    fprintf( stderr, "dmabuf_source_destroy %p\n", ctx);

    if (ctx->texture)
        glDeleteTextures(1, &ctx->texture);

    dmabuf_source_close(ctx);
    dmabuf_source_close_fds(ctx);

    delete ctx;
}

static uint32_t dmabuf_source_get_pitch(dmabuf_source_t *ctx)
{
    if (ctx->active_fb < 0)
        return 0;
    return ctx->fbs.resp.framebuffers[ctx->active_fb].pitch;
}

static uint32_t dmabuf_source_get_width(dmabuf_source_t *ctx)
{
    if (ctx->active_fb < 0)
        return 0;
    return ctx->fbs.resp.framebuffers[ctx->active_fb].width;
}

static uint32_t dmabuf_source_get_height(dmabuf_source_t *ctx)
{
    if (ctx->active_fb < 0)
        return 0;
    return ctx->fbs.resp.framebuffers[ctx->active_fb].height;
}

// set this to false if you want to use a pbuffer which doesn't segfault
#ifndef NO_SURFACE
#define NO_SURFACE false
#endif

// used the same way as printf
#define fail(...) { fprintf(stderr, "[ERROR] (%s:%d) ", __FILE__, __LINE__); \
                    fprintf(stderr, __VA_ARGS__); \
                    fprintf(stderr, "\n"); \
                    exit(1); }

const char* getEGLErrorString(EGLint error) {
  switch (error) {
    case EGL_SUCCESS:
      return "EGL_SUCCESS";
    case EGL_BAD_ACCESS:
      return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
      return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
      return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:
      return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:
      return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:
      return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
      return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:
      return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:
      return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:
      return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:
      return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
      return "EGL_BAD_NATIVE_WINDOW";
    default:
      return "UNKNOWN";
  }
}

const char * getGLErrorString()
{
    switch (glGetError()) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return "Unknown error";
    }
}

void printEGLErrors() {
    EGLint error = EGL_SUCCESS;
    do {
        error = eglGetError();
        if (error != EGL_SUCCESS) {
            printf("[ERROR] %s\n", getEGLErrorString(error));
        }
    } while (error != EGL_SUCCESS);
}

void printEGLInfo(EGLDisplay display) {
    const char *s;
    s = eglQueryString(display, EGL_VERSION);
    printf("EGL_VERSION = %s\n", s);
    s = eglQueryString(display, EGL_EXTENSIONS);
    printf("EGL_EXTENSIONS = %s\n", s);
}

void printGLInfo() {
    const GLubyte *s;
    s = glGetString(GL_VENDOR);
    printf("GL_VENDOR = %s\n", s);
    s = glGetString(GL_VERSION);
    printf("GL_VERSION = %s\n", s);
    s = glGetString(GL_RENDERER);
    printf("GL_REMDERER = %s\n", s);
    s = glGetString(GL_EXTENSIONS);
    printf("GL_EXTENSIONS = %s\n", s);
}

bool hasExtension(const char *name) {
    auto extensions = reinterpret_cast<const char *>
                                      (glGetString(GL_EXTENSIONS));
    if (extensions == nullptr) fail("couldn't get extension list");
    return strstr(extensions, name) != nullptr;
}

void
error_fatal(const char* format, ...) {
    printf("error: ");

    va_list va;
    va_start(va, format);
    vprintf(format, va);
    va_end(va);

    printf("\n");
    exit(1);
}

EGLConfig chooseFBConfig(EGLDisplay display) {
    const EGLint visual_attribs[] = {
                    EGL_COLOR_BUFFER_TYPE,     EGL_RGB_BUFFER,
        EGL_BUFFER_SIZE, 32,
        EGL_ALPHA_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
/*                    EGL_DEPTH_SIZE,            24,
                    EGL_STENCIL_SIZE,          8,

                    EGL_SAMPLE_BUFFERS,        0,
                    EGL_SAMPLES,               0,
                    EGL_SURFACE_TYPE,          EGL_WINDOW_BIT, //window only
*/
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    printf("Getting matching framebuffer configs\n");
    EGLint num_configs = 0;
    EGLConfig config[1];
    if (!eglChooseConfig(display, visual_attribs, config, 1, &num_configs)) {
      printEGLErrors();
      fail("eglChooseConfig failed");
    }
    if (num_configs == 0) fail("No FB config found");
    return config[0];
}

EGLSurface createSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType native_window, int width, int height) {
    EGLSurface surface  = EGL_NO_SURFACE;
    if (!native_window) {
        const EGLint surface_attribs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_NONE
        };
        surface = eglCreatePbufferSurface(display, config, surface_attribs);
    } else {
        const EGLint egl_surface_attribs[] = {
            EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
            EGL_NONE,
        };
        surface = eglCreateWindowSurface(display, config,
            native_window,
            egl_surface_attribs);
    }

    if (surface == EGL_NO_SURFACE) {
        printEGLErrors();
        fail("Couldn't create surface");
    }
    return surface;
}

EGLContext createContext(EGLDisplay display, EGLConfig config) {
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    auto context = eglCreateContext(display, config, nullptr, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        printEGLErrors();
        fail("EGL context creation failed");
    }
    return context;
}

// If you get an error please report on github. You may try different GL context version or GLSL version. See GL<>GLSL version table at the top of this file.
static bool CheckShader(GLuint handle, const char* desc)
{
    GLint status = 0, log_length = 0;
    glGetShaderiv(handle, GL_COMPILE_STATUS, &status);
    glGetShaderiv(handle, GL_INFO_LOG_LENGTH, &log_length);
    if ((GLboolean)status == GL_FALSE)
        fprintf(stderr, "ERROR: failed to compile %s!\n", desc);
    if (log_length > 1)
    {
        std::vector<char> buf;
        buf.resize((int)(log_length + 1));
        glGetShaderInfoLog(handle, log_length, NULL, (GLchar*)buf.data());
        fprintf(stderr, "%s\n", buf.data());
    }
    return (GLboolean)status == GL_TRUE;
}

// If you get an error please report on GitHub. You may try different GL context version or GLSL version.
static bool CheckProgram(GLuint handle, const char* desc, const char *version)
{
    GLint status = 0, log_length = 0;
    glGetProgramiv(handle, GL_LINK_STATUS, &status);
    glGetProgramiv(handle, GL_INFO_LOG_LENGTH, &log_length);
    if ((GLboolean)status == GL_FALSE)
        fprintf(stderr, "ERROR: failed to link %s! (with GLSL '%s')\n", desc, version);
    if (log_length > 1)
    {
        std::vector<char> buf;
        buf.resize((int)(log_length + 1));
        glGetProgramInfoLog(handle, log_length, NULL, (GLchar*)buf.data());
        fprintf(stderr, "%s\n", buf.data());
    }
    return (GLboolean)status == GL_TRUE;
}

struct KMSGrabberData
{
    KMSGrabberData()
    {
    }

    //dmabuf_source_t * dmabuf = nullptr;
};

KMSGrabber::KMSGrabber(QObject *parent, GrabberContext * gcontext)
    : GrabberBase(parent, gcontext)
{
    //_display = XOpenDisplay(NULL);
    //gladLoadEGL();
    //gladLoadGL();

    /*const int drmfd = open("/dev/dri/card0", O_RDONLY);
    if (drmfd < 0) {
        perror("Cannot open card");
        return 1;
    }*/

    Display *dpy = EGL_DEFAULT_DISPLAY;

    m_display = eglGetDisplay(dpy);
    if (m_display == EGL_NO_DISPLAY) fail("Failed to open display");
    if (!eglInitialize(m_display, nullptr, nullptr))
        fail("Failed to initialize EGL");
    printEGLInfo(m_display);
    printEGLErrors();

    m_fbconfig = chooseFBConfig(m_display);
    m_context = createContext(m_display, m_fbconfig);

    EGLBoolean res = false;
    res = eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context);
    if (!res) {
        printEGLErrors();
        fail("Couldn't make context current");
    }

    int ok = eglBindAPI(EGL_OPENGL_ES_API);
    if (!ok)
        error_fatal("eglBindAPI(0x%x) failed", EGL_OPENGL_ES_API);

    // create all objects
    program = glCreateProgram();
    vertexshader = glCreateShader(GL_VERTEX_SHADER);
    fragmentshader = glCreateShader(GL_FRAGMENT_SHADER);
    glGenVertexArrays(1, &vertexarray);
    glGenBuffers(1, &vertexbuffer);

    // shader source code
    const GLchar* vertexshader_source = {
        //"#version 450 core\n"
        "layout (location = 0) in vec3 in_position;\n"
        "layout (location = 1) in vec2 in_texcoord;\n"
        "out vec2 texcoord;\n"
        "void main () {\n"
        "gl_Position = vec4(in_position, 1);\n"
        "texcoord = in_texcoord;\n"
        "}\n"
    };

    const GLchar* fragmentshader_source = {
        //"#version 450 core\n"
        "precision mediump float;\n"
        "in vec2 texcoord;\n"
        "uniform sampler2D tex1;\n"
        "layout (location = 0) out vec4 out_color;\n"
        "void main () {\n"
        "out_color = texture(tex1, texcoord);\n"
        "}\n"
    };


    const char * g_GlslVersionString = "#version 300 es\n";
    const GLchar* vertex_shader_with_version[2] = { g_GlslVersionString, vertexshader_source };
    vertexshader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexshader, 2, vertex_shader_with_version, NULL);
    glCompileShader(vertexshader);
    CheckShader(vertexshader, "vertex shader");

    const GLchar* fragment_shader_with_version[2] = { g_GlslVersionString, fragmentshader_source };
    fragmentshader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentshader, 2, fragment_shader_with_version, NULL);
    glCompileShader(fragmentshader);
    CheckShader(fragmentshader, "fragment shader");

    program = glCreateProgram();
    glAttachShader(program, vertexshader);
    glAttachShader(program, fragmentshader);
    glLinkProgram(program);
    CheckProgram(program, "shader program", g_GlslVersionString);

    // setup vertex array
    glBindVertexArray(vertexarray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLvoid*)(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLvoid*)(sizeof(vec3)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);


    // setup vertex buffer
    float size = 1.f;
    // flipped Y texcoord
    std::vector<Vertex> vertices = {
        { { -size, size, 0 },{ 0, 1 } },
        { { -size, -size, 0 },{ 0, 0 } },
        { { +size, -size, 0 },{ 1, 0 } },

        { { -size, size, 0 },{ 0, 1 } },
        { { size, size, 0 },{ 1, 1 } },
        { { +size, -size, 0 },{ 1, 0 } },

    };
    glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /****************************/

    glClearColor(0, 0, 0, 0);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_dmabuf = dmabuf_source_create(m_display);
    m_surface = createSurface(m_display, m_fbconfig, 0,
                            dmabuf_source_get_width(m_dmabuf),
                            dmabuf_source_get_height(m_dmabuf));
    res = eglMakeCurrent(m_display, m_surface, m_surface, m_context);

}

QList<ScreenInfo> * KMSGrabber::screensWithWidgets(QList<ScreenInfo> *result, const QList<GrabWidget *> &grabWidgets)
{
    result->clear();

    //m_dmabuf = dmabuf_source_create(m_display);

    auto &resp = m_dmabuf->fbs.resp;

    for (int i = 0; i < resp.num_framebuffers; ++i) {
        int width = dmabuf_source_get_width(m_dmabuf);
        int height = dmabuf_source_get_height(m_dmabuf);

        ScreenInfo screen;
        intptr_t handle = i;
        screen.handle = reinterpret_cast<void *>(handle);
        screen.rect = QRect(0, 0, width, height);
        for (int k = 0; k < grabWidgets.size(); ++k) {
            if (screen.rect.intersects(grabWidgets[k]->rect())) {
                result->append(screen);
                break;
            }
        }
        break; //FIXME only one
    }

    return result;
}

bool KMSGrabber::reallocate(const QList<ScreenInfo> &screens)
{
    freeScreens();

    m_dmabuf = dmabuf_source_create(m_display);

    //for (int i = 0; i < screens.size(); ++i) {
    auto &resp = m_dmabuf->fbs.resp;

    for (int i = 0; i < resp.num_framebuffers; ++i) {

        int width = dmabuf_source_get_width(m_dmabuf);
        int height = dmabuf_source_get_height(m_dmabuf);
        int pitch = dmabuf_source_get_pitch(m_dmabuf);

        DEBUG_HIGH_LEVEL << "dimensions " << width << "x" << height << screens[i].handle;

        KMSGrabberData *d = new KMSGrabberData();

        int imagesize = height * pitch;

        GrabbedScreen grabScreen;
        grabScreen.imgData = new unsigned char[imagesize];
        grabScreen.imgDataSize = imagesize;
        grabScreen.bytesPerRow = pitch;
        grabScreen.imgFormat = BufferFormatAbgr;
        grabScreen.screenInfo = screens[i];
        grabScreen.associatedData = d;
        _screensWithWidgets.append(grabScreen);
        break; //FIXME only one
    }

    return true;
}

GrabResult KMSGrabber::grabScreens()
{
    for (int i = 0; i < 1; ++i) {
        KMSGrabberData * kmsgrab = reinterpret_cast<KMSGrabberData *>(_screensWithWidgets[i].associatedData);
        int dmabuf_width = dmabuf_source_get_width(m_dmabuf);
        int dmabuf_height = dmabuf_source_get_height(m_dmabuf);

        auto start = std::chrono::high_resolution_clock::now();

        glViewport(0, 0, dmabuf_width, dmabuf_height);
        glClearColor(0, 0, 0, 0);
        glClearDepthf(1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* draw textured quad */
        glUseProgram(program);
        //glActiveTexture(GL_TEXTURE0); // needed?
        //glBindTexture(GL_TEXTURE_2D, texture);
        glBindTexture(GL_TEXTURE_2D, m_dmabuf->texture);

        glBindVertexArray(vertexarray);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glFlush();

        //pixels.resize(width * height * 4);
        //glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());


        if (eglSwapBuffers(m_display, m_surface)){
            //printf("swappped\n");
        }

        glReadPixels(0, 0, dmabuf_width, dmabuf_height, GL_RGBA, GL_UNSIGNED_BYTE, (void*)_screensWithWidgets[i].imgData);

        auto end = std::chrono::high_resolution_clock::now();
        auto dur = end -start;
        std::cerr << "fps: " << 1000000 / std::chrono::duration_cast<std::chrono::microseconds>(dur).count() << "\n";
    }

    return GrabResultOk;
}

void KMSGrabber::freeScreens()
{
    for (int i = 0; i < _screensWithWidgets.size(); ++i) {
        KMSGrabberData *d = reinterpret_cast<KMSGrabberData *>(_screensWithWidgets[i].associatedData);
        delete d;
        d = NULL;
    }

    _screensWithWidgets.clear();

    if (m_dmabuf) {
        dmabuf_source_destroy(m_dmabuf);
        m_dmabuf = nullptr;
        //close(drmfd);
    }
}

KMSGrabber::~KMSGrabber()
{
    freeScreens();
    //XCloseDisplay(_display);

    // destroy all objects
    glDeleteProgram(program);
    glDeleteShader(vertexshader);
    glDeleteShader(fragmentshader);
    glDeleteVertexArrays(1, &vertexarray);
    glDeleteBuffers(1, &vertexbuffer);

    eglDestroyContext(m_display, m_context);
    eglTerminate(m_display);
}
