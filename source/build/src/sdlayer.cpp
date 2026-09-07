// SDL interface layer for the Build Engine
// Use SDL 1.2 or 2.0 from http://www.libsdl.org

#include "compat.h"
#include <signal.h>
#include "sdl_inc.h"
#include "renderlayer.h"
#include "cache1d.h"
//#include "pragmas.h"
#include "a.h"
#include "build.h"
#include "osd.h"
#include "engine_priv.h"
#include "palette.h"

#include "softsurface.h"
#ifdef USE_OPENGL
# include "vitagl_shim.h"
# include "glbuild.h"
# include "glsurface.h"
#endif

#if defined _WIN32
# include "winbits.h"
#endif
#if defined __APPLE__
# include "osxbits.h"
# include <mach/mach.h>
# include <mach/mach_time.h>
#endif
#if defined HAVE_GTK2
# include "gtkbits.h"
#endif
#ifdef __ANDROID__
# include <android/log.h>
#endif
#if defined GEKKO
# include "wiibits.h"
# include <ogc/lwp.h>
# include <ogc/lwp_watchdog.h>
#endif

#if SDL_MAJOR_VERSION != 1
static SDL_version linked;
#endif

#ifdef __PSP2__
#include <vita2d.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include "psp2_kbdvita.h"
#if defined HAVE_VITAGL && defined __PSP2__
// NOTA toolchain (verificata su repo ufficiali): la SDL2 di vdpm e' upstream
// 2.32.x compilata SENZA backend GL (VIDEO_VITA_PIB/PVR default OFF), quindi
// SDL_GL_CreateContext fallisce SEMPRE e SDL_GL_SwapWindow non presenta nulla.
// vitaGL va guidato DIRETTAMENTE: vglInitExtended + chiamate GL linkate a
// -lvitaGL + vglSwapBuffers. Niente SDL_WINDOW_OPENGL / SDL_GL_* in-game.
// (Con la SDL2_vitagl di Northfear sarebbe l'opposto, ma non e' quella di vdpm.)
#include <vitaGL.h>
#include <psp2/kernel/sysmem.h>
#endif

// Video path GPU-only per la versione vitaGL:
// il launcher usa vita2d, il gioco usa SOLO vitaGL diretto (Polymost e
// classic presentato via glsurface) con window SDL semplice per input/eventi.
// Nessun fallback vita2d in-game: dopo vita2d_fini() le texture fb/gpu non
// esistono piu' e riusarle freeza la console (use-after-free -> hard reboot).
static int vita_gl_active = 0;
static int vita_vgl_inited = 0;
static int vita_vita2d_dead = 0;

// Minimal append-logger for on-device diagnostics (written from scratch).
// The device has no visible stdout, so every video step lands in
// ux0:data/EDuke32/vitagl.log for the black-screen triage.
static SceUID vita_log_fd = -1;
static void vita_log(const char *msg)
{
    if (vita_log_fd < 0)
        vita_log_fd = sceIoOpen("ux0:data/EDuke32/vitagl.log",
                                SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (vita_log_fd >= 0 && msg)
        sceIoWrite(vita_log_fd, msg, strlen(msg));
}

int _newlib_heap_size_user = 300 * 1024 * 1024;

#define MAX_CURDIR_PATH 512
char cur_dir[MAX_CURDIR_PATH] = "ux0:data/EDuke32/";
int can_use_IME_keyboard = 1;
char *getcwd(char *buf, size_t size) {
    if (buf != NULL) {
        strncpy(buf, cur_dir, size);
    }
    return cur_dir;
}
int chdir(const char *path) {
    return 0;
}
#endif

#if !defined STARTUP_SETUP_WINDOW
int32_t startwin_open(void) { return 0; }
int32_t startwin_close(void) { return 0; }
int32_t startwin_puts(const char *s) { UNREFERENCED_PARAMETER(s); return 0; }
int32_t startwin_idle(void *s) { UNREFERENCED_PARAMETER(s); return 0; }
int32_t startwin_settitle(const char *s) { UNREFERENCED_PARAMETER(s); return 0; }
int32_t startwin_run(void) { return 0; }
#endif

/// These can be useful for debugging sometimes...
//#define SDL_WM_GrabInput(x) SDL_WM_GrabInput(SDL_GRAB_OFF)
//#define SDL_ShowCursor(x) SDL_ShowCursor(SDL_ENABLE)

#define SURFACE_FLAGS    (SDL_SWSURFACE|SDL_HWPALETTE|SDL_HWACCEL)

// undefine to restrict windowed resolutions to conventional sizes
#define ANY_WINDOWED_SIZE

// fix for mousewheel
int32_t inputchecked = 0;

char quitevent=0, appactive=1, novideo=0;

// video
static SDL_Surface *sdl_surface/*=NULL*/;

#if SDL_MAJOR_VERSION==2
static SDL_Window *sdl_window=NULL;
static SDL_GLContext sdl_context=NULL;
#endif

#ifdef __PSP2__
// Sentinelle come su desktop: il primo videoSetMode deve creare SEMPRE
// window+context GL. Con 960/544/32 iniziali, la prima richiesta
// (960x544x32 fullscreen) matcha e setvideomode_sdlcommon ritorna 0:
// nessun context, sdl_window==NULL, vita_gl_active resta 0 -> nero fisso.
int32_t xres=-1, yres=-1, bpp=0, fullscreen=0, bytesperline = 0;
#else
int32_t xres=-1, yres=-1, bpp=0, fullscreen=0, bytesperline;
#endif
intptr_t frameplace=0;
int32_t lockcount=0;
char modechange=1;
char offscreenrendering=0;
char videomodereset = 0;
int32_t nofog=0;
#ifndef EDUKE32_GLES
static uint16_t sysgamma[3][256];
#endif
#ifdef USE_OPENGL
// OpenGL stuff
char nogl=0;
#endif
static int32_t vsync_renderlayer;
int32_t maxrefreshfreq=0;
#if SDL_MAJOR_VERSION==2
static uint32_t currentVBlankInterval=0;
#endif

// last gamma, contrast, brightness
static float lastvidgcb[3];

//#define KEY_PRINT_DEBUG

#include "sdlkeytrans.cpp"

static SDL_Surface *appicon = NULL;
#if !defined __APPLE__ && !defined EDUKE32_TOUCH_DEVICES
static SDL_Surface *loadappicon(void);
#endif

static mutex_t m_initprintf;

// Joystick dead and saturation zones
uint16_t *joydead, *joysatur;

#ifdef _WIN32
# if SDL_MAJOR_VERSION != 1
//
// win_gethwnd() -- gets the window handle
//
HWND win_gethwnd(void)
{
    struct SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);

    if (SDL_GetWindowWMInfo(sdl_window, &wmInfo) != SDL_TRUE)
        return 0;

    if (wmInfo.subsystem == SDL_SYSWM_WINDOWS)
        return wmInfo.info.win.window;

    initprintf("win_gethwnd: Unknown WM subsystem?!\n");

    return 0;
}
# endif
//
// win_gethinstance() -- gets the application instance
//
HINSTANCE win_gethinstance(void)
{
    return (HINSTANCE)GetModuleHandle(NULL);
}
#endif


int32_t wm_msgbox(const char *name, const char *fmt, ...)
{
    char buf[2048];
    va_list va;

    UNREFERENCED_PARAMETER(name);

    va_start(va,fmt);
    vsnprintf(buf,sizeof(buf),fmt,va);
    va_end(va);

#if defined EDUKE32_OSX
    return osx_msgbox(name, buf);
#elif defined _WIN32
    MessageBox(win_gethwnd(),buf,name,MB_OK|MB_TASKMODAL);
    return 0;
#elif defined EDUKE32_TOUCH_DEVICES
    initprintf("wm_msgbox called. Message: %s: %s",name,buf);
    return 0;
#elif defined GEKKO
    puts(buf);
    return 0;
#else
# if defined HAVE_GTK2
    if (gtkbuild_msgbox(name, buf) >= 0)
        return 0;
# endif
# if SDL_MAJOR_VERSION > 1
#  if !defined _WIN32
    // Replace all tab chars with spaces because the hand-rolled SDL message
    // box diplays the former as N/L instead of whitespace.
    for (size_t i=0; i<sizeof(buf); i++)
        if (buf[i] == '\t')
            buf[i] = ' ';
#  endif
    return SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, name, buf, NULL);
# else
    puts(buf);
    puts("   (press Return or Enter to continue)");
    getchar();

    return 0;
# endif
#endif
}

int32_t wm_ynbox(const char *name, const char *fmt, ...)
{
    char buf[2048];
    va_list va;

    UNREFERENCED_PARAMETER(name);

    va_start(va,fmt);
    vsnprintf(buf,sizeof(buf),fmt,va);
    va_end(va);

#if defined EDUKE32_OSX
    return osx_ynbox(name, buf);
#elif defined _WIN32
    return (MessageBox(win_gethwnd(),buf,name,MB_YESNO|MB_ICONQUESTION|MB_TASKMODAL) == IDYES);
#elif defined EDUKE32_TOUCH_DEVICES
    initprintf("wm_ynbox called, this is bad! Message: %s: %s",name,buf);
    initprintf("Returning false..");
    return 0;
#elif defined GEKKO
    puts(buf);
    puts("Assuming yes...");
    return 1;
#else
# if defined HAVE_GTK2
    int ret = gtkbuild_ynbox(name, buf);
    if (ret >= 0)
        return ret;
# endif
# if SDL_MAJOR_VERSION > 1
    int r = -1;

    const SDL_MessageBoxButtonData buttons[] = {
        {
            SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
            0,
            "No"
        },{
            SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
            1,
            "Yes"
        },
    };

    SDL_MessageBoxData data = {
        SDL_MESSAGEBOX_INFORMATION,
        NULL, /* no parent window */
        name,
        buf,
        2,
        buttons,
        NULL /* Default color scheme */
    };

    SDL_ShowMessageBox(&data, &r);

    return r;
# else
    char c;

    puts(buf);
    puts("   (type 'Y' or 'N', and press Return or Enter to continue)");
    do c = getchar(); while (c != 'Y' && c != 'y' && c != 'N' && c != 'n');
    return c == 'Y' || c == 'y';
# endif
#endif
}

void wm_setapptitle(const char *name)
{
#ifndef EDUKE32_TOUCH_DEVICES
    if (name != apptitle)
        Bstrncpyz(apptitle, name, sizeof(apptitle));

#if !defined(__APPLE__)
    if (!appicon)
        appicon = loadappicon();
#endif

#if SDL_MAJOR_VERSION == 1
    SDL_WM_SetCaption(apptitle, NULL);

    if (appicon && sdl_surface)
        SDL_WM_SetIcon(appicon, 0);
#else
    if (sdl_window)
    {
        SDL_SetWindowTitle(sdl_window, apptitle);

        if (appicon)
        {
#if defined _WIN32
        if (!EDUKE32_SDL_LINKED_PREREQ(linked, 2, 0, 5))
#endif
            SDL_SetWindowIcon(sdl_window, appicon);
        }
    }
#endif

    startwin_settitle(apptitle);
#else
    UNREFERENCED_PARAMETER(name);
#endif
}

//
//
// ---------------------------------------
//
// System
//
// ---------------------------------------
//
//

/* XXX: libexecinfo could be used on systems without gnu libc. */
#if !defined _WIN32 && defined __GNUC__ && !defined __OpenBSD__ && !(defined __APPLE__ && defined __BIG_ENDIAN__) && !defined GEKKO && !defined __PSP2__ && !defined EDUKE32_TOUCH_DEVICES && !defined __OPENDINGUX__
# define PRINTSTACKONSEGV 1
# include <execinfo.h>
#endif

static inline char grabmouse_low(char a);

#ifndef __ANDROID__
static void attach_debugger_here(void) {}

static void sighandler(int signum)
{
    UNREFERENCED_PARAMETER(signum);
    //    if (signum==SIGSEGV)
    {
        grabmouse_low(0);
#if PRINTSTACKONSEGV
        {
            void *addr[32];
            int32_t errfd = fileno(stderr);
            int32_t n=backtrace(addr, ARRAY_SIZE(addr));
            backtrace_symbols_fd(addr, n, errfd);
        }
        // This is useful for attaching the debugger post-mortem. For those pesky
        // cases where the program runs through happily when inspected from the start.
        //        usleep(15000000);
#endif
        attach_debugger_here();
        app_crashhandler();
        uninitsystem();
        Bexit(8);
    }
}
#endif

#ifdef __ANDROID__
int mobile_halted = 0;
#ifdef __cplusplus
extern "C"
{
#endif
void G_Shutdown(void);
#ifdef __cplusplus
}
#endif

int sdlayer_mobilefilter(void *userdata, SDL_Event *event)
{
    switch (event->type)
    {
        case SDL_APP_TERMINATING:
            // yes, this calls into the game, ugh
            if (mobile_halted == 1)
                G_Shutdown();

            mobile_halted = 1;
            return 0;
        case SDL_APP_LOWMEMORY:
            gltexinvalidatetype(INVALIDATE_ALL);
            return 0;
        case SDL_APP_WILLENTERBACKGROUND:
            mobile_halted = 1;
            return 0;
        case SDL_APP_DIDENTERBACKGROUND:
            gltexinvalidatetype(INVALIDATE_ALL);
            // tear down video?
            return 0;
        case SDL_APP_WILLENTERFOREGROUND:
            // restore video?
            return 0;
        case SDL_APP_DIDENTERFOREGROUND:
            mobile_halted = 0;
            return 0;
        default:
            return 1;//!halt;
    }

    UNREFERENCED_PARAMETER(userdata);
}
#endif

#ifdef __ANDROID__
# include <setjmp.h>
static jmp_buf eduke32_exit_jmp_buf;
static int eduke32_return_value;

void eduke32_exit_return(int retval)
{
    eduke32_return_value = retval;
    longjmp(eduke32_exit_jmp_buf, 1);
    EDUKE32_UNREACHABLE_SECTION(return);
}
#endif

#ifdef __PSP2__
uint8_t *framebuffer;

vita2d_texture *fb_texture, *gpu_texture;

uint32_t SCE_CTRL_CONFIRM;
uint32_t SCE_CTRL_CANCEL;

int get_x_text(vita2d_pgf *font, char *text) {
    return (960 - vita2d_pgf_text_width(font, 1.0, text)) / 2;
}

uint32_t white;
uint32_t yellow;
uint32_t green;

typedef struct credits_voice{
    int x;
    int y;
    uint32_t *color;
    char text[256];
} credits_voice;

#define INTRO_VOICES 7

credits_voice intro[INTRO_VOICES] = {
    {0, 100, &yellow, "EDuke32 Vita v.1.6"},
    {0, 120, &white,  "Port by Rinnegatamante"},
    {0, 180, &yellow, "Select a GRP file to launch:"},
	{0, 440, &green,  "Press START to insert custom launch args"},
    {0, 480, &yellow, "Thanks to my distinguished Patroners:"},
    {0, 500, &white,  "RaveHeart - drd70f14 - Polytoad"},
    {0, 520, &white,  "Tain Sueiras - TheVita3K Project"},
};

typedef struct grp_info {
	char name[64];
	int x;
} grp_info;

char empty[2] = "";
char grp_ln[16] = "-gamegrp";
grp_info grp_files[12];
uint8_t num_grp_files = 0;

int scanForGRPFiles(vita2d_pgf *font) {
	SceIoDirent g_dir;
	SceUID d = sceIoDopen("ux0:data/EDuke32");
	while (sceIoDread(d, &g_dir) > 0) {
		if ((strcasecmp(&g_dir.d_name[strlen(g_dir.d_name) - 4], ".grp") == 0) ||
			(strcasecmp(&g_dir.d_name[strlen(g_dir.d_name) - 4], ".ssi") == 0) ||
			(strcasecmp(&g_dir.d_name[strlen(g_dir.d_name) - 4], ".bat") == 0))
		{
			strcpy(grp_files[num_grp_files].name, g_dir.d_name);
			grp_files[num_grp_files].x = get_x_text(font, g_dir.d_name);
			num_grp_files++;
		}
	}
}

static uint16_t title[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static uint16_t initial_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint16_t input_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
char title_keyboard[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1] = "";

void ascii2utf(uint16_t* dst, char* src){
	if(!src || !dst)return;
	while(*src)*(dst++)=(*src++);
	*dst=0x00;
}

void utf2ascii(char* dst, uint16_t* src){
	if(!src || !dst)return;
	while(*src)*(dst++)=(*(src++))&0xFF;
	*dst=0x00;
}

int psp2_main(unsigned int argc, void *argv) {
    SceAppUtilInitParam appUtilParam;
    SceAppUtilBootParam appUtilBootParam;
	memset(&appUtilParam, 0, sizeof(SceAppUtilInitParam));
    memset(&appUtilBootParam, 0, sizeof(SceAppUtilBootParam));
    sceAppUtilInit(&appUtilParam, &appUtilBootParam);
	SceCommonDialogConfigParam cmnDlgCfgParam;
	sceCommonDialogConfigParamInit(&cmnDlgCfgParam);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cmnDlgCfgParam.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cmnDlgCfgParam.enterButtonAssign);
    sceCommonDialogSetConfigParam(&cmnDlgCfgParam);
    
    SCE_CTRL_CONFIRM = (cmnDlgCfgParam.enterButtonAssign == 0) ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    SCE_CTRL_CANCEL = (cmnDlgCfgParam.enterButtonAssign == 0) ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
    
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    vita2d_init();
    vita2d_set_vblank_wait(0);
    
    gpu_texture = vita2d_create_empty_texture_format(960, 544, SCE_GXM_TEXTURE_FORMAT_P8_1BGR);
    vita2d_texture_set_filters(gpu_texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    fb_texture = vita2d_create_empty_texture_format(960, 544, SCE_GXM_TEXTURE_FORMAT_P8_1BGR);
    
    framebuffer = (uint8_t*)vita2d_texture_get_datap(fb_texture);
    
    baselayer_init();
    
    vita2d_pgf *font = vita2d_load_default_pgf();
    white = RGBA8(0xFF, 0xFF, 0xFF, 0xFF);
    yellow = RGBA8(0xFF, 0xFF, 0x00, 0xFF);
    green = RGBA8(0x00, 0xFF, 0x00, 0xFF);
    
    int j, z, k = 0;
    for (j=0;j<INTRO_VOICES;j++){
        intro[j].x = get_x_text(font, intro[j].text);
    }
    
	char *int_argv[3];
	int_argv[0] = empty;
	int_argv[1] = grp_ln;
	scanForGRPFiles(font);
	
	SceCtrlData pad;
	uint32_t oldpad;
    for (;;) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
        vita2d_start_drawing();
		vita2d_clear_screen();
        for (z=0;z<INTRO_VOICES;z++) {
            vita2d_pgf_draw_text(font, intro[z].x, intro[z].y, *intro[z].color, 1.0, intro[z].text);
        }
		for (z=0;z<num_grp_files;z++) {
			int y = 200 + z * 20;
			if (k > 5) y -= 20 * (k - 5);
			if (y <= 400 && y >= 200) {
				vita2d_pgf_draw_text(font, grp_files[z].x, y, k == z ? yellow : white, 1.0, grp_files[z].name);
			}
		}
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
		if ((pad.buttons & SCE_CTRL_DOWN) && (!(oldpad & SCE_CTRL_DOWN))) {
			k = (k + 1) % num_grp_files;
		} else if ((pad.buttons & SCE_CTRL_UP) && (!(oldpad & SCE_CTRL_UP))) {
			k--;
			if (k < 0) k = num_grp_files - 1;
		} else if ((pad.buttons & SCE_CTRL_CONFIRM) && (!(oldpad & SCE_CTRL_CONFIRM))) {
			if (strcasecmp(&grp_files[k].name[strlen(grp_files[k].name) - 4], ".bat") == 0) {
				char fname[512];
				sprintf(fname, "ux0:data/EDuke32/%s", grp_files[k].name);
				SceUID fd = sceIoOpen(fname, SCE_O_RDONLY, 0777);
				int fsize = sceIoLseek(fd, 0, SEEK_END);
				sceIoLseek(fd, 0, SEEK_SET);
				char *content = (char*)malloc(fsize + 1);
				sceIoRead(fd, content, fsize);
				content[fsize] = 0;
				sceIoClose(fd);
				sprintf(title_keyboard, "%s", (char*)(strstr(content, "eduke32") + 8));
				free(content);
			} else {
				int_argv[2] = grp_files[k].name;
			}
			break;
		} else if ((pad.buttons & SCE_CTRL_START) && (!(oldpad & SCE_CTRL_START))) {
			memset(input_text, 0, (SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1) << 1);
			memset(initial_text, 0, (SCE_IME_DIALOG_MAX_TEXT_LENGTH) << 1);
			sprintf(title_keyboard, "Insert command-line arguments");
			ascii2utf(title, title_keyboard);
			SceImeDialogParam param;
			sceImeDialogParamInit(&param);
			param.supportedLanguages = 0x0001FFFF;
			param.languagesForced = SCE_TRUE;
			param.type = SCE_IME_TYPE_BASIC_LATIN;
			param.title = title;
			param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
			param.initialText = initial_text;
			param.inputTextBuffer = input_text;
			sceImeDialogInit(&param);
			while (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
				vita2d_start_drawing();
				vita2d_clear_screen();
				vita2d_end_drawing();
				vita2d_common_dialog_update();
				vita2d_swap_buffers();
			}
			SceImeDialogResult result;
			memset(&result, 0, sizeof(SceImeDialogResult));
			sceImeDialogGetResult(&result);
			if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
				utf2ascii(title_keyboard, input_text);
				sceImeDialogTerm();
				break;
			}
			sceImeDialogTerm();
		}
		oldpad = pad.buttons;
    }
	
	if (strlen(title_keyboard) > 1) {
		char *cmd_argv[32];
		int cmd_argc = 2;
		cmd_argv[0] = "";
		cmd_argv[1] = title_keyboard;
		char *ptr = title_keyboard;
		for (;;) {
			char *space = strstr(ptr, " ");
			if (space == NULL) break;
			*space = 0;
			cmd_argv[cmd_argc++] = ptr = space + 1;
		}
		vita2d_fini();
		vita_vita2d_dead = 1;
		fb_texture = NULL;
		gpu_texture = NULL;
		framebuffer = NULL;
		vita_log("vita: launcher done, entering app_main (custom args)\n");
		return app_main(cmd_argc, (const char **)cmd_argv);
	} else {
		vita2d_fini();
		vita_vita2d_dead = 1;
		fb_texture = NULL;
		gpu_texture = NULL;
		framebuffer = NULL;
		vita_log("vita: launcher done, entering app_main (GRP select)\n");
		return app_main(3, (const char **)int_argv);
	}
}
#endif

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpCmdLine, int nCmdShow)
#elif defined __ANDROID__
# ifdef __cplusplus
extern "C" int eduke32_android_main(int argc, char const *argv[]);
# endif
int eduke32_android_main(int argc, char const *argv[])
#elif defined GEKKO
int SDL_main(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
#ifdef __PSP2__
	SceUID main_thread = sceKernelCreateThread("EDuke32", psp2_main, 0x40, 0x800000, 0, 0, NULL);
	if (main_thread >= 0){
		sceKernelStartThread(main_thread, 0, NULL);
		sceKernelWaitThreadEnd(main_thread, NULL, NULL);
	}
    return 0;
#endif
#ifdef __ANDROID__
    if (setjmp(eduke32_exit_jmp_buf))
    {
        return eduke32_return_value;
    }
#endif

#if defined _WIN32 && defined SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING
    // Thread naming interferes with debugging using MinGW-w64's GDB.
    SDL_SetHint(SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING, "1");
#endif

    int32_t r;

#ifdef USE_OPENGL
    char *argp;

    if ((argp = Bgetenv("BUILD_NOFOG")) != NULL)
        nofog = Batol(argp);

#ifndef _WIN32
    setenv("__GL_THREADED_OPTIMIZATIONS", "1", 0);
#endif
#endif

    buildkeytranslationtable();

#ifndef __ANDROID__
    signal(SIGSEGV, sighandler);
    signal(SIGILL, sighandler);  /* clang -fcatch-undefined-behavior uses an ill. insn */
    signal(SIGABRT, sighandler);
    signal(SIGFPE, sighandler);
#else
    SDL_SetEventFilter(sdlayer_mobilefilter, NULL);
#endif

#ifdef _WIN32
    UNREFERENCED_PARAMETER(hInst);
    UNREFERENCED_PARAMETER(hPrevInst);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    win_open();

    if (!CheckWinVersion())
    {
        MessageBox(0, "This application requires a newer Windows version to run.", apptitle, MB_OK | MB_ICONSTOP);
        return -1;
    }
#elif defined(GEKKO)
    wii_open();
#elif defined(HAVE_GTK2)
    // Pre-initialize SDL video system in order to make sure XInitThreads() is called
    // before GTK starts talking to X11.
    uint32_t inited = SDL_WasInit(SDL_INIT_VIDEO);
    if (inited == 0)
        SDL_Init(SDL_INIT_VIDEO);
    else if (!(inited & SDL_INIT_VIDEO))
        SDL_InitSubSystem(SDL_INIT_VIDEO);
    gtkbuild_init(&argc, &argv);
#endif

    startwin_open();
    maybe_redirect_outputs();

#ifdef _WIN32
    char *argvbuf;
    int32_t buildargc = win_buildargs(&argvbuf);
    const char **buildargv = (const char **) Bmalloc(sizeof(char *)*(buildargc+1));
    char *wp = argvbuf;

    for (bssize_t i=0; i<buildargc; i++, wp++)
    {
        buildargv[i] = wp;
        while (*wp) wp++;
    }
    buildargv[buildargc] = NULL;

    r = app_main(buildargc, (const char **)buildargv);
#else
    r = app_main(argc, (char const * const *)argv);
#endif

    startwin_close();

#ifdef _WIN32
    win_close();
#elif defined(HAVE_GTK2)
    gtkbuild_exit(r);
#endif

    return r;
}


#if SDL_MAJOR_VERSION != 1
int32_t videoSetVsync(int32_t newSync)
{
    if (vsync_renderlayer == newSync)
        return newSync;

#ifdef USE_OPENGL
    if (sdl_context)
    {
        int result = SDL_GL_SetSwapInterval(newSync);

        if (result == -1)
        {
            if (newSync == -1)
            {
                newSync = 1;
                result = SDL_GL_SetSwapInterval(newSync);
            }

            if (result == -1)
            {
                newSync = 0;
                OSD_Printf("Unable to enable VSync!\n");
            }
        }

        vsync_renderlayer = newSync;
    }
    else
#endif
    {
        vsync_renderlayer = newSync;

        videoResetMode();
        if (videoSetGameMode(fullscreen, xres, yres, bpp, upscalefactor))
            OSD_Printf("restartvid: Reset failed...\n");
    }

    return newSync;
}
#endif

int32_t sdlayer_checkversion(void);
#if SDL_MAJOR_VERSION != 1
int32_t sdlayer_checkversion(void)
{
    SDL_version compiled;

    SDL_GetVersion(&linked);
    SDL_VERSION(&compiled);

    if (!Bmemcmp(&compiled, &linked, sizeof(SDL_version)))
        initprintf("Initializing SDL %d.%d.%d\n",
            compiled.major, compiled.minor, compiled.patch);
    else
    initprintf("Initializing SDL %d.%d.%d"
               " (built against SDL version %d.%d.%d)\n",
               linked.major, linked.minor, linked.patch, compiled.major, compiled.minor, compiled.patch);

    if (SDL_VERSIONNUM(linked.major, linked.minor, linked.patch) < SDL_REQUIREDVERSION)
    {
        /*reject running under SDL versions older than what is stated in sdl_inc.h */
        initprintf("You need at least v%d.%d.%d of SDL to run this game\n",SDL_MIN_X,SDL_MIN_Y,SDL_MIN_Z);
        return -1;
    }

    return 0;
}

//
// initsystem() -- init SDL systems
//
int32_t initsystem(void)
{
    const int sdlinitflags = SDL_INIT_VIDEO;

    mutex_init(&m_initprintf);

    if (sdlayer_checkversion())
        return -1;

    int32_t err = 0;
    uint32_t inited = SDL_WasInit(sdlinitflags);
    if (inited == 0)
        err = SDL_Init(sdlinitflags);
    else if ((inited & sdlinitflags) != sdlinitflags)
        err = SDL_InitSubSystem(sdlinitflags & ~inited);

    frameplace = 0;
    lockcount = 0;

    return 0;
}
#endif


//
// uninitsystem() -- uninit SDL systems
//
void uninitsystem(void)
{
    uninitinput();
    timerUninit();
#ifdef __PSP2__
    // Il launcher ha gia' fatto vita2d_fini(): rifarlo e' un double-free
    // che freeza la Vita. In-game siamo GPU-only, quindi qui si chiude
    // solo vitaGL (se attivo) e il file di log.
    if (!vita_vita2d_dead)
    {
        vita2d_fini();
        vita_vita2d_dead = 1;
    }
#if defined HAVE_VITAGL
    // vitaGL non ha una vglEnd(): a fine processo GXM viene reclamato
    // dall'OS. Qui marchiamo solo lo stato e chiudiamo il log.
    vita_gl_active = 0;
    vita_vgl_inited = 0;
#endif
    if (vita_log_fd >= 0)
    {
        sceIoClose(vita_log_fd);
        vita_log_fd = -1;
    }
#else
    (void)0;
#endif
}


//
// system_getcvars() -- propagate any cvars that are read post-initialization
//
void system_getcvars(void)
{
    vsync = videoSetVsync(vsync);
}

//
// initprintf() -- prints a formatted string to the intitialization window
//
void initprintf(const char *f, ...)
{
    va_list va;
    char buf[2048];

    va_start(va, f);
    Bvsnprintf(buf, sizeof(buf), f, va);
    va_end(va);

    initputs(buf);
}


//
// initputs() -- prints a string to the intitialization window
//
void initputs(const char *buf)
{
    static char dabuf[2048];

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"DUKE", "%s",buf);
#endif
    OSD_Puts(buf);
//    Bprintf("%s", buf);

    mutex_lock(&m_initprintf);
    if (Bstrlen(dabuf) + Bstrlen(buf) > 1022)
    {
        startwin_puts(dabuf);
        Bmemset(dabuf, 0, sizeof(dabuf));
    }

    Bstrcat(dabuf,buf);

    if (g_logFlushWindow || Bstrlen(dabuf) > 768)
    {
        startwin_puts(dabuf);
#ifndef _WIN32
        startwin_idle(NULL);
#else
        handleevents();
#endif
        Bmemset(dabuf, 0, sizeof(dabuf));
    }
    mutex_unlock(&m_initprintf);
}

//
// debugprintf() -- prints a formatted debug string to stderr
//
void debugprintf(const char *f, ...)
{
#if defined DEBUGGINGAIDS && !(defined __APPLE__ && defined __BIG_ENDIAN__)
    va_list va;

    va_start(va,f);
    Bvfprintf(stderr, f, va);
    va_end(va);
#else
    UNREFERENCED_PARAMETER(f);
#endif
}


//
//
// ---------------------------------------
//
// All things Input
//
// ---------------------------------------
//
//

// static int32_t joyblast=0;
static SDL_Joystick *joydev = NULL;

//
// initinput() -- init input system
//
int32_t initinput(void)
{
    int32_t i, j;

#ifdef _WIN32
    Win_GetOriginalLayoutName();
    Win_SetKeyboardLayoutUS(1);
#endif

#if defined EDUKE32_OSX
    static char sdl_has3buttonmouse[] = "SDL_HAS3BUTTONMOUSE=1";
    // force OS X to operate in >1 button mouse mode so that LMB isn't adulterated
    if (!getenv("SDL_HAS3BUTTONMOUSE"))
        putenv(sdl_has3buttonmouse);
#endif

    inputdevices = 1 | 2;  // keyboard (1) and mouse (2)
    g_mouseGrabbed = 0;

    memset(g_keyNameTable, 0, sizeof(g_keyNameTable));

#if SDL_MAJOR_VERSION == 1
#define SDL_SCANCODE_TO_KEYCODE(x) (SDLKey)(x)
#define SDL_JoystickNameForIndex(x) SDL_JoystickName(x)
#define SDL_NUM_SCANCODES SDLK_LAST
    if (SDL_EnableKeyRepeat(250, 30))
        initprintf("Error enabling keyboard repeat.\n");
    SDL_EnableUNICODE(1);  // let's hope this doesn't hit us too hard
#endif

    for (i = SDL_NUM_SCANCODES - 1; i >= 0; i--)
    {
        if (!keytranslation[i])
            continue;

        Bstrncpyz(g_keyNameTable[keytranslation[i]], SDL_GetKeyName(SDL_SCANCODE_TO_KEYCODE(i)), sizeof(g_keyNameTable[i]));
    }

    if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK))
    {
        i = SDL_NumJoysticks();
        initprintf("%d joystick(s) found\n", i);

        for (j = 0; j < i; j++)
            initprintf("  %d. %s\n", j + 1, SDL_JoystickNameForIndex(j));

        joydev = SDL_JoystickOpen(0);

        if (joydev)
        {
            SDL_JoystickEventState(SDL_ENABLE);
            inputdevices |= 4;

            // KEEPINSYNC duke3d/src/gamedefs.h, mact/include/_control.h
            joystick.numAxes = min(9, SDL_JoystickNumAxes(joydev));
            joystick.numButtons = min(32, SDL_JoystickNumButtons(joydev));
            joystick.numHats = min((36-joystick.numButtons)/4,SDL_JoystickNumHats(joydev));
            initprintf("Joystick 1 has %d axes, %d buttons, and %d hat(s).\n", joystick.numAxes, joystick.numButtons, joystick.numHats);

            joystick.pAxis = (int32_t *)Bcalloc(joystick.numAxes, sizeof(int32_t));

            if (joystick.numHats)
                joystick.pHat = (int32_t *)Bcalloc(joystick.numHats, sizeof(int32_t));

            for (i = 0; i < joystick.numHats; i++) joystick.pHat[i] = -1;  // centre

            joydead = (uint16_t *)Bcalloc(joystick.numAxes, sizeof(uint16_t));
            joysatur = (uint16_t *)Bcalloc(joystick.numAxes, sizeof(uint16_t));
        }
    }

    return 0;
}

//
// uninitinput() -- uninit input system
//
void uninitinput(void)
{
#ifdef _WIN32
    Win_SetKeyboardLayoutUS(0);
#endif
    mouseUninit();

    if (joydev)
    {
        SDL_JoystickClose(joydev);
        joydev = NULL;
    }
}

#ifndef GEKKO
const char *joyGetName(int32_t what, int32_t num)
{
    static char tmp[64];

    switch (what)
    {
        case 0:  // axis
            if ((unsigned)num > (unsigned)joystick.numAxes)
                return NULL;
            Bsprintf(tmp, "Axis %d", num);
            return (char *)tmp;

        case 1:  // button
            if ((unsigned)num > (unsigned)joystick.numButtons)
                return NULL;
            Bsprintf(tmp, "Button %d", num);
            return (char *)tmp;

        case 2:  // hat
            if ((unsigned)num > (unsigned)joystick.numHats)
                return NULL;
            Bsprintf(tmp, "Hat %d", num);
            return (char *)tmp;

        default: return NULL;
    }
}
#endif


//
// initmouse() -- init mouse input
//
int32_t mouseInit(void)
{
    g_mouseEnabled=g_mouseLockedToWindow;
    mouseGrabInput(g_mouseLockedToWindow); // FIXME - SA
    return 0;
}

//
// uninitmouse() -- uninit mouse input
//
void mouseUninit(void)
{
    mouseGrabInput(0);
    g_mouseEnabled = 0;
}


#if SDL_MAJOR_VERSION != 1
//
// grabmouse_low() -- show/hide mouse cursor, lower level (doesn't check state).
//                    furthermore return 0 if successful.
//

static inline char grabmouse_low(char a)
{
#if !defined EDUKE32_TOUCH_DEVICES
    /* FIXME: Maybe it's better to make sure that grabmouse_low
       is called only when a window is ready?                */
    if (sdl_window)
        SDL_SetWindowGrab(sdl_window, a ? SDL_TRUE : SDL_FALSE);
    return SDL_SetRelativeMouseMode(a ? SDL_TRUE : SDL_FALSE);
#else
    UNREFERENCED_PARAMETER(a);
    return 0;
#endif
}
#endif

//
// grabmouse() -- show/hide mouse cursor
//
void mouseGrabInput(bool grab)
{
    if (appactive && g_mouseEnabled)
    {
#if !defined EDUKE32_TOUCH_DEVICES
        if ((grab != g_mouseGrabbed) && !grabmouse_low(grab))
#endif
            g_mouseGrabbed = grab;
    }
    else
        g_mouseGrabbed = grab;

    g_mousePos.x = g_mousePos.y = 0;
}

void mouseLockToWindow(char a)
{
    if (!(a & 2))
    {
        mouseGrabInput(a);
        g_mouseLockedToWindow = g_mouseGrabbed;
    }

    SDL_ShowCursor((osd && osd->flags & OSD_CAPTURE) ? SDL_ENABLE : SDL_DISABLE);
}

//
// setjoydeadzone() -- sets the dead and saturation zones for the joystick
//
void joySetDeadZone(int32_t axis, uint16_t dead, uint16_t satur)
{
    joydead[axis] = dead;
    joysatur[axis] = satur;
}


//
// getjoydeadzone() -- gets the dead and saturation zones for the joystick
//
void joyGetDeadZone(int32_t axis, uint16_t *dead, uint16_t *satur)
{
    *dead = joydead[axis];
    *satur = joysatur[axis];
}


//
//
// ---------------------------------------
//
// All things Timer
// Ken did this
//
// ---------------------------------------
//
//

static uint32_t timerfreq;
static uint32_t timerlastsample;
int32_t timerticspersec=0;
static double msperu64tick = 0;
static void(*usertimercallback)(void) = NULL;


//
// inittimer() -- initialize timer
//
int32_t timerInit(int32_t tickspersecond)
{
    if (timerfreq) return 0;    // already installed

//    initprintf("Initializing timer\n");

#if defined(_WIN32) && SDL_MAJOR_VERSION == 1
    int32_t t = win_inittimer();
    if (t < 0)
        return t;
#endif

    timerfreq = 1000;
    timerticspersec = tickspersecond;
    timerlastsample = SDL_GetTicks() * timerticspersec / timerfreq;

    usertimercallback = NULL;

    msperu64tick = 1000.0 / (double)timerGetFreqU64();

    return 0;
}

//
// uninittimer() -- shut down timer
//
void timerUninit(void)
{
    timerfreq=0;
#if defined(_WIN32) && SDL_MAJOR_VERSION==1
    win_timerfreq=0;
#endif
    msperu64tick = 0;
}

//
// sampletimer() -- update totalclock
//
void timerUpdate(void)
{
    if (!timerfreq) return;

    int64_t i = SDL_GetTicks();
    int32_t n = tabledivide64(i * timerticspersec, timerfreq) - timerlastsample;

    if (n <= 0) return;

    totalclock += n;
    timerlastsample += n;

    if (usertimercallback)
        for (; n > 0; n--) usertimercallback();
}

#if defined LUNATIC
//
// getticks() -- returns the sdl ticks count
//
uint32_t timerGetTicks(void)
{
    return (uint32_t)SDL_GetTicks();
}
#endif

// high-resolution timers for profiling

#if SDL_MAJOR_VERSION != 1
uint64_t timerGetTicksU64(void)
{
    return SDL_GetPerformanceCounter();
}

uint64_t timerGetFreqU64(void)
{
    return SDL_GetPerformanceFrequency();
}
#endif

// Returns the time since an unspecified starting time in milliseconds.
// (May be not monotonic for certain configurations.)
ATTRIBUTE((flatten))
double timerGetHiTicks(void)
{
    return (double)timerGetTicksU64() * msperu64tick;
}

//
// gettimerfreq() -- returns the number of ticks per second the timer is configured to generate
//
int32_t timerGetFreq(void)
{
    return timerticspersec;
}


//
// installusertimercallback() -- set up a callback function to be called when the timer is fired
//
void(*timerSetCallback(void(*callback)(void)))(void)
{
    void(*oldtimercallback)(void);

    oldtimercallback = usertimercallback;
    usertimercallback = callback;

    return oldtimercallback;
}



//
//
// ---------------------------------------
//
// All things Video
//
// ---------------------------------------
//
//


//
// getvalidmodes() -- figure out what video modes are available
//
static int sortmodes(const void *a_, const void *b_)
{
    int32_t x;

    const struct validmode_t *a = (const struct validmode_t *)a_;
    const struct validmode_t *b = (const struct validmode_t *)b_;

    if ((x = a->fs   - b->fs)   != 0) return x;
    if ((x = a->bpp  - b->bpp)  != 0) return x;
    if ((x = a->xdim - b->xdim) != 0) return x;
    if ((x = a->ydim - b->ydim) != 0) return x;

    return 0;
}

static char modeschecked=0;

#if SDL_MAJOR_VERSION != 1
void videoGetModes(void)
{
    int32_t i, maxx = 0, maxy = 0;
    SDL_DisplayMode dispmode;

    if (modeschecked || novideo)
        return;

    validmodecnt = 0;
    //    initprintf("Detecting video modes:\n");

    // do fullscreen modes first
    for (i = 0; i < SDL_GetNumDisplayModes(0); i++)
    {
        SDL_GetDisplayMode(0, i, &dispmode);

        if (!SDL_CHECKMODE(dispmode.w, dispmode.h) ||
            (maxrefreshfreq && (dispmode.refresh_rate > maxrefreshfreq)))
            continue;

        // HACK: 8-bit == Software, 32-bit == OpenGL
        SDL_ADDMODE(dispmode.w, dispmode.h, 8, 1);
#ifdef USE_OPENGL
        if (!nogl)
            SDL_ADDMODE(dispmode.w, dispmode.h, 32, 1);
#endif
        if ((dispmode.w > maxx) || (dispmode.h > maxy))
        {
            maxx = dispmode.w;
            maxy = dispmode.h;
        }
    }

    SDL_CHECKFSMODES(maxx, maxy);

    // add windowed modes next
    for (i = 0; g_defaultVideoModes[i].x; i++)
    {
        if (!SDL_CHECKMODE(g_defaultVideoModes[i].x, g_defaultVideoModes[i].y))
            continue;

        // HACK: 8-bit == Software, 32-bit == OpenGL
        SDL_ADDMODE(g_defaultVideoModes[i].x, g_defaultVideoModes[i].y, 8, 0);

#ifdef USE_OPENGL
        if (nogl)
            continue;

        SDL_ADDMODE(g_defaultVideoModes[i].x, g_defaultVideoModes[i].y, 32, 0);
#endif
    }

    qsort((void *)validmode, validmodecnt, sizeof(struct validmode_t), &sortmodes);

    modeschecked = 1;
}
#endif

//
// checkvideomode() -- makes sure the video mode passed is legal
//
int32_t videoCheckMode(int32_t *x, int32_t *y, int32_t c, int32_t fs, int32_t forced)
{
    int32_t i, nearest=-1, dx, dy, odx=9999, ody=9999;

    videoGetModes();

    if (c>8
#ifdef USE_OPENGL
            && nogl
#endif
       ) return -1;

    // fix up the passed resolution values to be multiples of 8
    // and at least 320x200 or at most MAXXDIMxMAXYDIM
    *x = clamp(*x, 320, MAXXDIM);
    *y = clamp(*y, 200, MAXYDIM);

    for (i = 0; i < validmodecnt; i++)
    {
        if (validmode[i].bpp != c || validmode[i].fs != fs)
            continue;

        dx = klabs(validmode[i].xdim - *x);
        dy = klabs(validmode[i].ydim - *y);

        if (!(dx | dy))
        {
            // perfect match
            nearest = i;
            break;
        }

        if ((dx <= odx) && (dy <= ody))
        {
            nearest = i;
            odx = dx;
            ody = dy;
        }
    }

#ifdef ANY_WINDOWED_SIZE
    if (!forced && (fs&1) == 0 && (nearest < 0 || (validmode[nearest].xdim!=*x || validmode[nearest].ydim!=*y)))
        return 0x7fffffffl;
#endif

    if (nearest < 0)
        return -1;

    *x = validmode[nearest].xdim;
    *y = validmode[nearest].ydim;

    return nearest;
}

static void destroy_window_resources()
{
/* We should NOT destroy the window surface. This is done automatically
   when SDL_DestroyWindow or SDL_SetVideoMode is called.             */

#if SDL_MAJOR_VERSION == 2
    if (sdl_context)
        SDL_GL_DeleteContext(sdl_context);
    sdl_context = NULL;
    if (sdl_window)
        SDL_DestroyWindow(sdl_window);
    sdl_window = NULL;
#endif
}

#ifdef USE_OPENGL
void sdlayer_setvideomode_opengl(void)
{
    glsurface_destroy();
    polymost_glreset();

    glEnable(GL_TEXTURE_2D);
    glShadeModel(GL_SMOOTH);  // GL_FLAT
    glClearColor(0, 0, 0, 1.0);  // Black Background
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);  // Use FASTEST for ortho!
//    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

#ifndef EDUKE32_GLES
    glDisable(GL_DITHER);
#endif

    glinfo.vendor = (const char *) glGetString(GL_VENDOR);
    glinfo.renderer = (const char *) glGetString(GL_RENDERER);
    glinfo.version = (const char *) glGetString(GL_VERSION);
    glinfo.extensions = (const char *) glGetString(GL_EXTENSIONS);

#if defined HAVE_VITAGL && defined __PSP2__
    // Su vitaGL una stringa NULL (nessun context o token non supportato)
    // finirebbe in Bstrstr/Bstrcmp -> crash -> freeze con hard reboot.
    // Inoltre forziamo i flag che il nostro shim non implementa davvero.
    if (!glinfo.vendor) glinfo.vendor = "";
    if (!glinfo.renderer) glinfo.renderer = "";
    if (!glinfo.version) glinfo.version = "";
    if (!glinfo.extensions) glinfo.extensions = "";
#endif

#ifdef POLYMER
    if (!Bstrcmp(glinfo.vendor, "ATI Technologies Inc."))
    {
        pr_ati_fboworkaround = 1;
        initprintf("Enabling ATI FBO color attachment workaround.\n");

        if (Bstrstr(glinfo.renderer, "Radeon X1"))
        {
            pr_ati_nodepthoffset = 1;
            initprintf("Enabling ATI R520 polygon offset workaround.\n");
        }
        else
            pr_ati_nodepthoffset = 0;
#ifdef __APPLE__
        // See bug description at http://lists.apple.com/archives/mac-opengl/2005/Oct/msg00169.html
        if (!Bstrncmp(glinfo.renderer, "ATI Radeon 9600", 15))
        {
            pr_ati_textureformat_one = 1;
            initprintf("Enabling ATI Radeon 9600 texture format workaround.\n");
        }
        else
            pr_ati_textureformat_one = 0;
#endif
    }
    else
        pr_ati_fboworkaround = 0;
#endif  // defined POLYMER

    glinfo.maxanisotropy = 1.0;
    glinfo.bgra = 0;
    glinfo.clamptoedge = 1;
    glinfo.multitex = 1;

    // process the extensions string and flag stuff we recognize

    glinfo.texnpot = !!Bstrstr(glinfo.extensions, "GL_ARB_texture_non_power_of_two") || !!Bstrstr(glinfo.extensions, "GL_OES_texture_npot");
    glinfo.multisample = !!Bstrstr(glinfo.extensions, "GL_ARB_multisample");
    glinfo.nvmultisamplehint = !!Bstrstr(glinfo.extensions, "GL_NV_multisample_filter_hint");
    glinfo.arbfp = !!Bstrstr(glinfo.extensions, "GL_ARB_fragment_program");
    glinfo.depthtex = !!Bstrstr(glinfo.extensions, "GL_ARB_depth_texture");
    glinfo.shadow = !!Bstrstr(glinfo.extensions, "GL_ARB_shadow");
    glinfo.fbos = !!Bstrstr(glinfo.extensions, "GL_EXT_framebuffer_object") || !!Bstrstr(glinfo.extensions, "GL_OES_framebuffer_object");

#if !defined EDUKE32_GLES
    glinfo.texcompr = !!Bstrstr(glinfo.extensions, "GL_ARB_texture_compression") && Bstrcmp(glinfo.vendor, "ATI Technologies Inc.");
# ifdef DYNAMIC_GLEXT
    if (glinfo.texcompr && (!glCompressedTexImage2D || !glGetCompressedTexImage))
    {
        // lacking the necessary extensions to do this
        initprintf("Warning: the GL driver lacks necessary functions to use caching\n");
        glinfo.texcompr = 0;
    }
# endif

    glinfo.bgra = !!Bstrstr(glinfo.extensions, "GL_EXT_bgra");
    glinfo.clamptoedge = !!Bstrstr(glinfo.extensions, "GL_EXT_texture_edge_clamp") ||
                         !!Bstrstr(glinfo.extensions, "GL_SGIS_texture_edge_clamp");
    glinfo.rect =
    !!Bstrstr(glinfo.extensions, "GL_NV_texture_rectangle") || !!Bstrstr(glinfo.extensions, "GL_EXT_texture_rectangle");

    glinfo.multitex = !!Bstrstr(glinfo.extensions, "GL_ARB_multitexture");

    glinfo.envcombine = !!Bstrstr(glinfo.extensions, "GL_ARB_texture_env_combine");
    glinfo.vbos = !!Bstrstr(glinfo.extensions, "GL_ARB_vertex_buffer_object");
    glinfo.sm4 = !!Bstrstr(glinfo.extensions, "GL_EXT_gpu_shader4");
    glinfo.occlusionqueries = !!Bstrstr(glinfo.extensions, "GL_ARB_occlusion_query");
    glinfo.glsl = !!Bstrstr(glinfo.extensions, "GL_ARB_shader_objects");
    glinfo.debugoutput = !!Bstrstr(glinfo.extensions, "GL_ARB_debug_output");
    glinfo.bufferstorage = !!Bstrstr(glinfo.extensions, "GL_ARB_buffer_storage");
    glinfo.sync = !!Bstrstr(glinfo.extensions, "GL_ARB_sync");
#if defined HAVE_VITAGL && defined __PSP2__
    // Lo shim vitaGL implementa sync/buffer-storage come stub finti e le
    // query di occlusione rischiano di inchiodare la GPU: mai dichiararli.
    glinfo.bufferstorage = 0;
    glinfo.sync = 0;
    glinfo.occlusionqueries = 0;
    glinfo.sm4 = 0;
#endif

    if (Bstrstr(glinfo.extensions, "WGL_3DFX_gamma_control"))
    {
        static int32_t warnonce;
        // 3dfx cards have issues with fog
        nofog = 1;
        if (!(warnonce & 1))
            initprintf("3dfx card detected: OpenGL fog disabled\n");
        warnonce |= 1;
    }
#else
    // don't bother checking because ETC2 et al. are not listed in extensions anyway
    glinfo.texcompr = 1; // !!Bstrstr(glinfo.extensions, "GL_OES_compressed_ETC1_RGB8_texture");
#endif

//    if (Bstrstr(glinfo.extensions, "GL_EXT_texture_filter_anisotropic"))
#if defined HAVE_VITAGL && defined __PSP2__
    // vitaGL non espone MAX_ANISOTROPY: la query darebbe solo un GL error.
    // Resta 1.0 (nessuna anisotropia), che e' il default sicuro su GXM.
#else
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &glinfo.maxanisotropy);
#endif

    if (!glinfo.dumped)
    {
        int32_t oldbpp = bpp;
        bpp = 32;
        osdcmd_glinfo(NULL);
        glinfo.dumped = 1;
        bpp = oldbpp;
    }
}
#endif  // defined USE_OPENGL

//
// setvideomode() -- set SDL video mode
//

int32_t setvideomode_sdlcommon(int32_t *x, int32_t *y, int32_t c, int32_t fs, int32_t *regrab)
{
    if ((fs == fullscreen) && (*x == xres) && (*y == yres) && (c == bpp) && !videomodereset)
        return 0;

    if (videoCheckMode(x, y, c, fs, 0) < 0)
        return -1;

#ifdef GEKKO
    if (!sdl_surface) // only run this the first time we set a video mode
        wii_initgamevideo();
#endif

    startwin_close();

    if (g_mouseGrabbed)
    {
        *regrab = 1;
        mouseGrabInput(0);
    }

    while (lockcount) videoEndDrawing();

#ifdef USE_OPENGL
    if (sdl_surface)
    {
        if (bpp > 8)
            polymost_glreset();
    }
    if (!nogl)
    {
        if (bpp == 8)
            glsurface_destroy();
        if ((fs == fullscreen) && (*x == xres) && (*y == yres) && (bpp != 0) && !videomodereset)
            return 0;
    }
    else
#endif
    {
       softsurface_destroy();
    }

    // clear last gamma/contrast/brightness so that it will be set anew
    lastvidgcb[0] = lastvidgcb[1] = lastvidgcb[2] = 0.0f;

    return 1;
}

void setvideomode_sdlcommonpost(int32_t x, int32_t y, int32_t c, int32_t fs, int32_t regrab)
{
    wm_setapptitle(apptitle);

#ifdef USE_OPENGL
    if (!nogl)
        sdlayer_setvideomode_opengl();
#endif

//    xres = x;
//    yres = y;
//    bpp = c;
#if defined HAVE_VITAGL && defined __PSP2__
    // Su Vita teniamo traccia del modo reale: serve a videoBeginDrawing
    // (8 vs 32 bpp), all'early-out di setvideomode_sdlcommon e a
    // upscalefactor = yres/200 in videoSetGameMode. Senza, bpp resta 0 e
    // il classic scrive nel posto sbagliato -> freeze.
    xres = x;
    yres = y;
    bpp = c;
#endif
    fullscreen = fs;
    // bytesperline = sdl_surface->pitch;
    numpages = c > 8 ? 2 : 1;
    frameplace = 0;
    lockcount = 0;
    modechange = 1;
    videomodereset = 0;

    // save the current system gamma to determine if gamma is available
#ifndef EDUKE32_GLES
    if (!gammabrightness)
    {
        //        float f = 1.0 + ((float)curbrightness / 10.0);
#if SDL_MAJOR_VERSION != 1
        if (SDL_GetWindowGammaRamp(sdl_window, sysgamma[0], sysgamma[1], sysgamma[2]) == 0)
#else
        if (SDL_GetGammaRamp(sysgamma[0], sysgamma[1], sysgamma[2]) >= 0)
#endif
            gammabrightness = 1;

        // see if gamma really is working by trying to set the brightness
        if (gammabrightness && videoSetGamma() < 0)
            gammabrightness = 0;  // nope
    }
#endif

    videoFadePalette(palfadergb.r, palfadergb.g, palfadergb.b, palfadedelta);

    if (regrab)
        mouseGrabInput(g_mouseLockedToWindow);
}

#if SDL_MAJOR_VERSION!=1
void setrefreshrate(void)
{
    SDL_DisplayMode dispmode;
    SDL_GetCurrentDisplayMode(0, &dispmode);

    dispmode.refresh_rate = maxrefreshfreq;

    SDL_DisplayMode newmode;
    SDL_GetClosestDisplayMode(0, &dispmode, &newmode);

    if (dispmode.refresh_rate != newmode.refresh_rate)
    {
        initprintf("Refresh rate: %dHz\n", newmode.refresh_rate);
        SDL_SetWindowDisplayMode(sdl_window, &newmode);
    }

    if (!newmode.refresh_rate)
        newmode.refresh_rate = 60;

    currentVBlankInterval = 1000/newmode.refresh_rate;
}

int32_t videoSetMode(int32_t x, int32_t y, int32_t c, int32_t fs)
{
    int32_t regrab = 0, ret;

    ret = setvideomode_sdlcommon(&x, &y, c, fs, &regrab);
    if (ret != 1)
    {
        if (ret == 0)
        {
            setvideomode_sdlcommonpost(x, y, c, fs, regrab);
        }
        return ret;
    }

    // deinit
    destroy_window_resources();

    initprintf("Setting video mode %dx%d (%d-bpp %s)\n", x, y, c, ((fs & 1) ? "fullscreen" : "windowed"));

#ifdef USE_OPENGL
    if (c > 8 || !nogl)
    {
#if defined HAVE_VITAGL && defined __PSP2__
        // Su questa toolchain (vdpm SDL2 = upstream senza backend GL) le
        // SDL_GL_* non esistono: vitaGL diretto, window SDL semplice.
        // 'i', 'j' e gli attributi SDL_GL servono solo al path desktop.
#else
        int32_t i, j;
#endif
#ifdef USE_GLEXT
        int32_t multisamplecheck = (glmultisample > 0);
#else
        int32_t multisamplecheck = 0;
#endif
        if (nogl)
            return -1;

#if defined HAVE_VITAGL && defined __PSP2__
        // Niente tabella attributi SDL_GL su Vita: la SDL di vdpm non ha
        // backend GL, vitaGL si guida da solo (vedi sotto).
#else
        struct glattribs
        {
            SDL_GLattr attr;
            int32_t value;
        } sdlayer_gl_attributes[] =
        {
#ifdef EDUKE32_GLES
              { SDL_GL_CONTEXT_MAJOR_VERSION, 1 },
              { SDL_GL_CONTEXT_MINOR_VERSION, 1 },
#endif
              { SDL_GL_DOUBLEBUFFER, 1 },
#ifdef USE_GLEXT
              { SDL_GL_MULTISAMPLEBUFFERS, glmultisample > 0 },
              { SDL_GL_MULTISAMPLESAMPLES, glmultisample },
#endif
              { SDL_GL_STENCIL_SIZE, 1 },
              { SDL_GL_ACCELERATED_VISUAL, 1 },
        };
#endif

        do
        {
#if defined HAVE_VITAGL && defined __PSP2__
            // vitaGL DIRETTO (vedi nota in testa al file): niente
            // SDL_WINDOW_OPENGL / SDL_GL_CreateContext / SDL_GL_*.
            // La window SDL serve solo per input ed eventi, GXM e' di vitaGL.
            if (!vita_vgl_inited)
            {
                // Threshold = RAM lasciata al GIOCO: vitaGL prealloca tutto
                // il resto per i suoi pool. Duke3D + heap stanno sotto i
                // ~160MB: tarare guardando "free RAM at vglInit" nel log.
                SceKernelFreeMemorySizeInfo meminfo;
                meminfo.size = sizeof(meminfo);
                sceKernelGetFreeMemorySize(&meminfo);
                {
                    char membuf[128];
                    snprintf(membuf, sizeof(membuf),
                             "vita: free RAM at vglInit: user=%dKB cdram=%dKB\n",
                             (int)(meminfo.size_user / 1024),
                             (int)(meminfo.size_cdram / 1024));
                    vita_log(membuf);
                }
                GLboolean vgl_ok = vglInitExtended(0, 960, 544,
                    160 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
                vita_vgl_inited = 1;
                vita_log(vgl_ok ? "vita: vglInitExtended done\n" : "vita: vglInitExtended FAILED\n");
            }
            sdl_window = SDL_CreateWindow("", windowpos ? windowx : (int)SDL_WINDOWPOS_CENTERED,
                                          windowpos ? windowy : (int)SDL_WINDOWPOS_CENTERED, x, y,
                                          0);

            if (!sdl_window)
            {
                initprintf("Unable to set video mode: SDL_CreateWindow failed: %s\n", SDL_GetError());
                destroy_window_resources();
                return -1;
            }

            vita_gl_active = 1;
            xres = x; yres = y; bpp = c;
            vita_log("vita: vitaGL video mode set\n");
            {
                char glinfobuf[512];
                const char *v = (const char *)glGetString(GL_VERSION);
                const char *r = (const char *)glGetString(GL_RENDERER);
                snprintf(glinfobuf, sizeof(glinfobuf), "vita: GL version=[%s] renderer=[%s]\n",
                         v ? v : "(null)", r ? r : "(null)");
                vita_log(glinfobuf);
            }
            // Primo present: flash rosso breve SOLO alla prima init (prova
            // visibile che lo swap vitaGL funziona), poi clear neri.
            // Niente delay lunghi: 250ms una tantum, non sembra un freeze.
            glViewport(0, 0, x, y);
            {
                static int vita_first_present = 1;
                if (vita_first_present)
                {
                    vita_first_present = 0;
                    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                    vglSwapBuffers(GL_TRUE);
                    vita_log("vita: red smoke frame swapped\n");
                    sceKernelDelayThread(250 * 1000);
                }
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                vglSwapBuffers(GL_TRUE);
            }
            vita_log("vita: first GL clear swapped\n");
#else
            SDL_GL_ATTRIBUTES(i, sdlayer_gl_attributes);

            /* HACK: changing SDL GL attribs only works before surface creation,
               so we have to create a new surface in a different format first
               to force the surface we WANT to be recreated instead of reused. */
            sdl_window = SDL_CreateWindow("", windowpos ? windowx : (int)SDL_WINDOWPOS_CENTERED,
                                          windowpos ? windowy : (int)SDL_WINDOWPOS_CENTERED, x, y,
                                          SDL_WINDOW_OPENGL);

            if (sdl_window)
                sdl_context = SDL_GL_CreateContext(sdl_window);

            if (!sdl_window || !sdl_context)
            {
                initprintf("Unable to set video mode: %s failed: %s\n", sdl_window ? "SDL_GL_CreateContext" : "SDL_GL_CreateWindow",  SDL_GetError());
                destroy_window_resources();
                return -1;
            }

            // Solo desktop: su Vita niente glad/SDL_GL (vitaGL diretto).
            gladLoadGLLoader(SDL_GL_GetProcAddress);
            if (GLVersion.major < 2)
            {
                initprintf("Your computer does not support OpenGL version 2 or greater. GL modes are unavailable.\n");
                nogl = 1;
                destroy_window_resources();
                return -1;
            }
#endif
#if defined HAVE_VITAGL && defined __PSP2__
            // Display Vita = sempre fullscreen 960x544, il driver SDL non ha
            // handler fullscreen/swapinterval: niente da fare qui (e niente
            // SDL_GL, vedi sopra). setrefreshrate() usa solo SDL display API
            // sicure e serve per currentVBlankInterval (timing frame).
            (void)fs;
            setrefreshrate();
#else
            SDL_SetWindowFullscreen(sdl_window, ((fs & 1) ? SDL_WINDOW_FULLSCREEN : 0));
            SDL_GL_SetSwapInterval(vsync_renderlayer);

            setrefreshrate();
#endif
#if defined HAVE_VITAGL && defined __PSP2__
        } while (0);
        (void)multisamplecheck; // single-pass su Vita: niente retry MSAA via SDL
#else
        } while (multisamplecheck--);
#endif
    }
    else
#endif  // defined USE_OPENGL
    {
#if defined HAVE_VITAGL && defined __PSP2__
        // GPU-only: senza GL non c'e' presentazione possibile (vita2d e'
        // gia' finita dopo il launcher). Meglio un errore pulito che un
        // freeze su texture morte.
        initprintf("Vita: no GL mode available, software fallback unsupported (GPU-only port).\n");
        vita_log("vita: FATAL software fallback requested, no GL\n");
        return -1;
#else
        // init
        sdl_window = SDL_CreateWindow("", windowpos ? windowx : (int)SDL_WINDOWPOS_CENTERED,
                                      windowpos ? windowy : (int)SDL_WINDOWPOS_CENTERED, x, y,
                                      0);
        if (!sdl_window)
            SDL2_VIDEO_ERR("SDL_CreateWindow");

        setrefreshrate();

        if (!sdl_surface)
        {
            sdl_surface = SDL_GetWindowSurface(sdl_window);
            if (!sdl_surface)
                SDL2_VIDEO_ERR("SDL_GetWindowSurface");
        }

        SDL_SetWindowFullscreen(sdl_window, ((fs & 1) ? SDL_WINDOW_FULLSCREEN : 0));
#endif
    }

    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
    setvideomode_sdlcommonpost(x, y, c, fs, regrab);

    return 0;
}
#endif

//
// resetvideomode() -- resets the video system
//
void videoResetMode(void)
{
    videomodereset = 1;
    modeschecked = 0;
}

//
// begindrawing() -- locks the framebuffer for drawing
//

void videoBeginDrawing(void)
{
    if (bpp > 8)
    {
        if (offscreenrendering) return;
        frameplace = 0;
        bytesperline = 0;
        modechange = 0;
        return;
    }

    // lock the frame
    if (lockcount++ > 0)
        return;

    if (offscreenrendering) return;

#if defined USE_OPENGL && defined HAVE_VITAGL && defined __PSP2__
    // GPU-only: il classic 8-bit disegna nel buffer CPU di glsurface, che
    // ShowFrame carica su texture GL. Il vecchio framebuffer vita2d non
    // esiste piu' dopo il launcher: usarlo = scrittura su memoria GPU
    // liberata = freeze con hard reboot.
    if (vita_gl_active)
    {
        void *glbuf = glsurface_getBuffer();
        if (!glbuf)
        {
            // Non ancora inizializzato (primo modo): niente su cui disegnare.
            // Non toccare il vecchio framebuffer vita2d (liberato).
            vita_log("vita: BeginDrawing without glsurface!\n");
            lockcount--;
            frameplace = 0;
            return;
        }
        frameplace = (intptr_t)glbuf;
        if (modechange)
        {
            // Il buffer glsurface e' xdim*ydim (v. videoAllocateBuffers):
            // bytesperline e ylookup devono seguirlo, non xres/yres.
            vec2_t bres = glsurface_getBufferResolution();
            bytesperline = bres.x ? bres.x : xdim;
            calc_ylookup(bytesperline, bres.y ? bres.y : ydim);
            modechange = 0;
        }
        return;
    }
#endif
	frameplace = (intptr_t)framebuffer;

    if (modechange)
    {
        bytesperline = xres;
        calc_ylookup(bytesperline, yres);
        modechange=0;
    }
}


//
// enddrawing() -- unlocks the framebuffer
//
void videoEndDrawing(void)
{
    if (bpp > 8)
    {
        if (!offscreenrendering) frameplace = 0;
        return;
    }

    if (!frameplace) return;
    if (lockcount > 1) { lockcount--; return; }
    if (!offscreenrendering) frameplace = 0;
    if (lockcount == 0) return;
    lockcount = 0;
}

//
// showframe() -- update the display
//

#ifdef __ANDROID__
extern "C" void AndroidDrawControls();
#endif

void videoShowFrame(int32_t w)
{
    UNREFERENCED_PARAMETER(w);

    if (offscreenrendering) return;

#if defined USE_OPENGL && defined HAVE_VITAGL && defined __PSP2__
    // Presentazione DIRETTA vitaGL: SDL_GL_SwapWindow non esiste in questa
    // toolchain (SDL vdpm senza backend GL) e non presenterebbe mai nulla.
    if (vita_gl_active)
    {
        static int vita_frame_count = 0;
        if (vita_frame_count == 0)
            vita_log("vita: first GL showframe\n");
        vita_frame_count++;
        if (bpp == 8)
        {
            // Classic 8-bit in finestra GL: il frame sta nel buffer CPU di
            // glsurface (riempito tra Begin/EndDrawing). Senza questo blit
            // lo swap presenta solo il clear nero -> schermo nero ai menu.
            // In Polymost (bpp>8) il motore ha gia' disegnato via GL.
            glsurface_blitBuffer();
        }
        // GL_TRUE = supporto common dialog (IME/tastiera) sopra il GL,
        // come fa il driver SDL2_vitagl di Northfear.
        vglSwapBuffers(GL_TRUE);
        return;
    }
#endif
#ifdef __PSP2__
    // GPU-only: dopo il launcher vita2d e' morta (texture NULL). Il blit
    // legacy esiste solo per build senza vitaGL; qui logghiamo invece di
    // freezare su memoria GPU liberata.
#if !defined HAVE_VITAGL
    if (fb_texture && gpu_texture)
    {
        memcpy(vita2d_texture_get_datap(gpu_texture),vita2d_texture_get_datap(fb_texture),vita2d_texture_get_stride(gpu_texture)*vita2d_texture_get_height(gpu_texture));
        vita2d_start_drawing();
        vita2d_draw_texture(gpu_texture, 0, 0);
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
        return;
    }
#endif
    vita_log("vita: ShowFrame without GL context!\n");
#endif
}

//
// setpalette() -- set palette values
//
int32_t videoUpdatePalette(int32_t start, int32_t num)
{
    UNREFERENCED_PARAMETER(start);
    UNREFERENCED_PARAMETER(num);
#if defined USE_OPENGL && defined HAVE_VITAGL && defined __PSP2__
    if (vita_gl_active)
    {
        // GPU-only: la palette del classic vive nella texture di glsurface.
        // Toccare le palette vita2d (liberate) = freeze. In Polymost 32-bit
        // la palette e' irrilevante ma l'upload e' harmless.
        glsurface_setPalette((void*)curpalettefaded);
        return 0;
    }
#endif
#ifdef __PSP2__
#if !defined HAVE_VITAGL
    if (!fb_texture || !gpu_texture)
        return 0;
#else
    // GPU-only senza GL: niente palette da aggiornare (e vita2d e' morta).
    if (!vita_gl_active)
        return 0;
#endif
    uint8_t *pal = (uint8_t*)curpalettefaded;
    uint8_t r, g, b;
    uint32_t* palette_tbl = (uint32_t*)vita2d_texture_get_palette(fb_texture);
    uint32_t* palette_tbl2 = (uint32_t*)vita2d_texture_get_palette(gpu_texture);
    for (int i = 0; i < 256; i++) {
        r = pal[0];
        g = pal[1];
        b = pal[2];
        palette_tbl[i] = r | (g << 8) | (b << 16) | (0xFF << 24);
        palette_tbl2[i] = r | (g << 8) | (b << 16) | (0xFF << 24);
        pal += 4;
    }
    return 0;
#else
    // Build desktop: palette gestita da SDL/GL, qui niente da fare.
    (void)start; (void)num;
    return 0;
#endif
}

//
// setgamma
//
int32_t videoSetGamma(void)
{

	return 0;

    if (novideo)
        return 0;

    int32_t i;
    uint16_t gammaTable[768];
    float gamma = max(0.1f, min(4.f, g_videoGamma));
    float contrast = max(0.1f, min(3.f, g_videoContrast));
    float bright = max(-0.8f, min(0.8f, g_videoBrightness));

    float invgamma = 1.f / gamma;
    float norm = powf(255.f, invgamma - 1.f);

    if (lastvidgcb[0] == gamma && lastvidgcb[1] == contrast && lastvidgcb[2] == bright)
        return 0;

    // This formula is taken from Doomsday

    for (i = 0; i < 256; i++)
    {
        float val = i * contrast - (contrast - 1.f) * 127.f;
        if (gamma != 1.f)
            val = powf(val, invgamma) / norm;

        val += bright * 128.f;

        gammaTable[i] = gammaTable[i + 256] = gammaTable[i + 512] = (uint16_t)max(0.f, min(65535.f, val * 256.f));
    }

#if SDL_MAJOR_VERSION == 1
    i = SDL_SetGammaRamp(&gammaTable[0], &gammaTable[256], &gammaTable[512]);
    if (i != -1)
#else
    i = INT32_MIN;

    if (sdl_window)
        i = SDL_SetWindowGammaRamp(sdl_window, &gammaTable[0], &gammaTable[256], &gammaTable[512]);

    if (i < 0)
    {
#ifndef __ANDROID__  // Don't do this check, it is really supported, TODO
/*
        if (i != INT32_MIN)
            initprintf("Unable to set gamma: SDL_SetWindowGammaRamp failed: %s\n", SDL_GetError());
*/
#endif

#ifndef EDUKE32_GLES
#if SDL_MAJOR_VERSION == 1
        SDL_SetGammaRamp(&sysgamma[0][0], &sysgamma[1][0], &sysgamma[2][0]);
#else
        if (sdl_window)
            SDL_SetWindowGammaRamp(sdl_window, &sysgamma[0][0], &sysgamma[1][0], &sysgamma[2][0]);
#endif
        gammabrightness = 0;
#endif
    }
    else
#endif
    {
        lastvidgcb[0] = gamma;
        lastvidgcb[1] = contrast;
        lastvidgcb[2] = bright;

        gammabrightness = 1;
    }

    return i;
}

#if !defined __APPLE__ && !defined EDUKE32_TOUCH_DEVICES
extern struct sdlappicon sdlappicon;
static inline SDL_Surface *loadappicon(void)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceFrom((void *)sdlappicon.pixels, sdlappicon.width, sdlappicon.height, 32,
                                                 sdlappicon.width * 4, 0xffl, 0xff00l, 0xff0000l, 0xff000000l);
    return surf;
}
#endif

//
//
// ---------------------------------------
//
// Miscellany
//
// ---------------------------------------
//
//

int32_t handleevents_peekkeys(void)
{
    SDL_PumpEvents();

#if SDL_MAJOR_VERSION==1
    return SDL_PeepEvents(NULL, 1, SDL_PEEKEVENT, SDL_EVENTMASK(SDL_KEYDOWN));
#else
    return SDL_PeepEvents(NULL, 1, SDL_PEEKEVENT, SDL_KEYDOWN, SDL_KEYDOWN);
#endif
}

void handleevents_updatemousestate(uint8_t state)
{
    g_mouseClickState = state == SDL_RELEASED ? MOUSE_RELEASED : MOUSE_PRESSED;
}


//
// handleevents() -- process the SDL message queue
//   returns !0 if there was an important event worth checking (like quitting)
//

int32_t handleevents_sdlcommon(SDL_Event *ev)
{
    switch (ev->type)
    {
#if !defined EDUKE32_IOS
        case SDL_MOUSEMOTION:
#ifndef GEKKO
            g_mouseAbs.x = ev->motion.x;
            g_mouseAbs.y = ev->motion.y;
#endif
            // SDL <VER> doesn't handle relative mouse movement correctly yet as the cursor still clips to the
            // screen edges
            // so, we call SDL_WarpMouse() to center the cursor and ignore the resulting motion event that occurs
            //  <VER> is 1.3 for PK, 1.2 for tueidj
            if (appactive && g_mouseGrabbed)
            {
# if SDL_MAJOR_VERSION==1
                if (ev->motion.x != xdim >> 1 || ev->motion.y != ydim >> 1)
# endif
                {
                    g_mousePos.x += ev->motion.xrel;
                    g_mousePos.y += ev->motion.yrel;
# if SDL_MAJOR_VERSION==1
                    SDL_WarpMouse(xdim>>1, ydim>>1);
# endif
                }
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        {
            int32_t j;

            // some of these get reordered to match winlayer
            switch (ev->button.button)
            {
                default: j = -1; break;
                case SDL_BUTTON_LEFT: j = 0; handleevents_updatemousestate(ev->button.state); break;
                case SDL_BUTTON_RIGHT: j = 1; break;
                case SDL_BUTTON_MIDDLE: j = 2; break;

#if SDL_MAJOR_VERSION == 1
                case SDL_BUTTON_WHEELUP:    // 4
                case SDL_BUTTON_WHEELDOWN:  // 5
                    j = ev->button.button;
                    break;
#endif
                /* Thumb buttons. */
#if SDL_MAJOR_VERSION==1 || !defined _WIN32
                // NOTE: SDL1 does have SDL_BUTTON_X1, but that's not what is
                // generated. Neither with SDL2 on Linux. (Other OSs: not tested.)
                case 8: j = 3; break;
                case 9: j = 6; break;
#else
                // On SDL2/Windows, everything is as it should be.
                case SDL_BUTTON_X1: j = 3; break;
                case SDL_BUTTON_X2: j = 6; break;
#endif
            }

            if (j < 0)
                break;

            if (ev->button.state == SDL_PRESSED)
                g_mouseBits |= (1 << j);
            else
#if SDL_MAJOR_VERSION==1
                if (j != SDL_BUTTON_WHEELUP && j != SDL_BUTTON_WHEELDOWN)
#endif
                g_mouseBits &= ~(1 << j);

            if (g_mouseCallback)
                g_mouseCallback(j+1, ev->button.state == SDL_PRESSED);
            break;
        }
#else
# if SDL_MAJOR_VERSION != 1
        case SDL_FINGERUP:
            g_mouseClickState = MOUSE_RELEASED;
            break;
        case SDL_FINGERDOWN:
            g_mouseClickState = MOUSE_PRESSED;
        case SDL_FINGERMOTION:
            g_mouseAbs.x = Blrintf(ev->tfinger.x * xdim);
            g_mouseAbs.y = Blrintf(ev->tfinger.y * ydim);
            break;
# endif
#endif

        case SDL_JOYAXISMOTION:
            if (appactive && ev->jaxis.axis < joystick.numAxes)
            {
                joystick.pAxis[ev->jaxis.axis] = ev->jaxis.value * 10000 / 32767;
                if ((joystick.pAxis[ev->jaxis.axis] < joydead[ev->jaxis.axis]) &&
                    (joystick.pAxis[ev->jaxis.axis] > -joydead[ev->jaxis.axis]))
                    joystick.pAxis[ev->jaxis.axis] = 0;
                else if (joystick.pAxis[ev->jaxis.axis] >= joysatur[ev->jaxis.axis])
                    joystick.pAxis[ev->jaxis.axis] = 10000;
                else if (joystick.pAxis[ev->jaxis.axis] <= -joysatur[ev->jaxis.axis])
                    joystick.pAxis[ev->jaxis.axis] = -10000;
                else
                    joystick.pAxis[ev->jaxis.axis] = joystick.pAxis[ev->jaxis.axis] * 10000 / joysatur[ev->jaxis.axis];
            }
            break;

        case SDL_JOYHATMOTION:
        {
            int32_t hatvals[16] = {
                -1,     // centre
                0,      // up 1
                9000,   // right 2
                4500,   // up+right 3
                18000,  // down 4
                -1,     // down+up!! 5
                13500,  // down+right 6
                -1,     // down+right+up!! 7
                27000,  // left 8
                27500,  // left+up 9
                -1,     // left+right!! 10
                -1,     // left+right+up!! 11
                22500,  // left+down 12
                -1,     // left+down+up!! 13
                -1,     // left+down+right!! 14
                -1,     // left+down+right+up!! 15
            };
            if (appactive && ev->jhat.hat < joystick.numHats)
                joystick.pHat[ev->jhat.hat] = hatvals[ev->jhat.value & 15];
            break;
        }

        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
            if (appactive && ev->jbutton.button < joystick.numButtons)
            {
                if (ev->jbutton.state == SDL_PRESSED)
                    joystick.bits |= 1 << ev->jbutton.button;
                else
                    joystick.bits &= ~(1 << ev->jbutton.button);

#ifdef GEKKO
                if (ev->jbutton.button == 0) // WII_A
                    handleevents_updatemousestate(ev->jbutton.state);
#endif
            }
            break;

        case SDL_QUIT:
            quitevent = 1;
            return -1;
    }

    return 0;
}

int32_t handleevents_pollsdl(void);
#if SDL_MAJOR_VERSION != 1
// SDL 2.0 specific event handling
int32_t handleevents_pollsdl(void)
{
    int32_t code, rv=0, j;
    SDL_Event ev;

    while (SDL_PollEvent(&ev))
    {
        switch (ev.type)
        {
            case SDL_TEXTINPUT:
                j = 0;
                do
                {
                    code = ev.text.text[j];

                    if (code != g_keyAsciiTable[OSD_OSDKey()] && !keyBufferFull())
                    {
                        if (OSD_HandleChar(code))
                            keyBufferInsert(code);
                    }
                } while (j < SDL_TEXTINPUTEVENT_TEXT_SIZE && ev.text.text[++j]);
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
                const SDL_Scancode sc = ev.key.keysym.scancode;
                code = keytranslation[sc];

                // Modifiers that have to be held down to be effective
                // (excludes KMOD_NUM, for example).
                static const int MODIFIERS =
                    KMOD_LSHIFT|KMOD_RSHIFT|KMOD_LCTRL|KMOD_RCTRL|
                    KMOD_LALT|KMOD_RALT|KMOD_LGUI|KMOD_RGUI;

                // XXX: see osd.c, OSD_HandleChar(), there are more...
                if (ev.key.type == SDL_KEYDOWN && !keyBufferFull() &&
                    (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER ||
                     sc == SDL_SCANCODE_ESCAPE ||
                     sc == SDL_SCANCODE_BACKSPACE ||
                     sc == SDL_SCANCODE_TAB ||
                     (((ev.key.keysym.mod) & MODIFIERS) == KMOD_LCTRL &&
                      (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z))))
                {
                    char keyvalue;
                    switch (sc)
                    {
                        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: keyvalue = '\r'; break;
                        case SDL_SCANCODE_ESCAPE: keyvalue = 27; break;
                        case SDL_SCANCODE_BACKSPACE: keyvalue = '\b'; break;
                        case SDL_SCANCODE_TAB: keyvalue = '\t'; break;
                        default: keyvalue = sc - SDL_SCANCODE_A + 1; break;  // Ctrl+A --> 1, etc.
                    }
                    if (OSD_HandleChar(keyvalue))
                        keyBufferInsert(keyvalue);
                }
                else if (ev.key.type == SDL_KEYDOWN &&
                         ev.key.keysym.sym != g_keyAsciiTable[OSD_OSDKey()] && !keyBufferFull() &&
                         !SDL_IsTextInputActive())
                {
                    /*
                    Necessary for Duke 3D's method of entering cheats to work without showing IMEs.
                    SDL_TEXTINPUT is preferable overall, but with bitmap fonts it has no advantage.
                    */
                    SDL_Keycode keyvalue = ev.key.keysym.sym;

                    if ('a' <= keyvalue && keyvalue <= 'z')
                    {
                        if (!!(ev.key.keysym.mod & KMOD_SHIFT) ^ !!(ev.key.keysym.mod & KMOD_CAPS))
                            keyvalue -= 'a'-'A';
                    }
                    else if (ev.key.keysym.mod & KMOD_SHIFT)
                    {
                        switch (keyvalue)
                        {
                            case '\'': keyvalue = '"'; break;

                            case ',': keyvalue = '<'; break;
                            case '-': keyvalue = '_'; break;
                            case '.': keyvalue = '>'; break;
                            case '/': keyvalue = '?'; break;
                            case '0': keyvalue = ')'; break;
                            case '1': keyvalue = '!'; break;
                            case '2': keyvalue = '@'; break;
                            case '3': keyvalue = '#'; break;
                            case '4': keyvalue = '$'; break;
                            case '5': keyvalue = '%'; break;
                            case '6': keyvalue = '^'; break;
                            case '7': keyvalue = '&'; break;
                            case '8': keyvalue = '*'; break;
                            case '9': keyvalue = '('; break;

                            case ';': keyvalue = ':'; break;

                            case '=': keyvalue = '+'; break;

                            case '[': keyvalue = '{'; break;
                            case '\\': keyvalue = '|'; break;
                            case ']': keyvalue = '}'; break;

                            case '`': keyvalue = '~'; break;
                        }
                    }
                    else if (ev.key.keysym.mod & KMOD_NUM) // && !(ev.key.keysym.mod & KMOD_SHIFT)
                    {
                        switch (keyvalue)
                        {
                            case SDLK_KP_1: keyvalue = '1'; break;
                            case SDLK_KP_2: keyvalue = '2'; break;
                            case SDLK_KP_3: keyvalue = '3'; break;
                            case SDLK_KP_4: keyvalue = '4'; break;
                            case SDLK_KP_5: keyvalue = '5'; break;
                            case SDLK_KP_6: keyvalue = '6'; break;
                            case SDLK_KP_7: keyvalue = '7'; break;
                            case SDLK_KP_8: keyvalue = '8'; break;
                            case SDLK_KP_9: keyvalue = '9'; break;
                            case SDLK_KP_0: keyvalue = '0'; break;
                            case SDLK_KP_PERIOD: keyvalue = '.'; break;
                            case SDLK_KP_COMMA: keyvalue = ','; break;
                        }
                    }

                    switch (keyvalue)
                    {
                        case SDLK_KP_DIVIDE: keyvalue = '/'; break;
                        case SDLK_KP_MULTIPLY: keyvalue = '*'; break;
                        case SDLK_KP_MINUS: keyvalue = '-'; break;
                        case SDLK_KP_PLUS: keyvalue = '+'; break;
                    }

                    if ((unsigned)keyvalue <= 0x7Fu)
                    {
                        if (OSD_HandleChar(keyvalue))
                            keyBufferInsert(keyvalue);
                    }
                }

                // initprintf("SDL2: got key %d, %d, %u\n", ev.key.keysym.scancode, code, ev.key.type);

                // hook in the osd
                if ((j = OSD_HandleScanCode(code, (ev.key.type == SDL_KEYDOWN))) <= 0)
                {
                    if (j == -1)  // osdkey
                        for (j = 0; j < NUMKEYS; ++j)
                            if (keyGetState(j))
                            {
                                keySetState(j, 0);
                                if (keypresscallback)
                                    keypresscallback(j, 0);
                            }
                    break;
                }

                if (ev.key.type == SDL_KEYDOWN)
                {
                    if (!keyGetState(code))
                    {
                        keySetState(code, 1);
                        if (keypresscallback)
                            keypresscallback(code, 1);
                    }
                }
                else
                {
# if 1
                    // The pause key generates a release event right after
                    // the pressing one. As a result, it gets unseen
                    // by the game most of the time.
                    if (code == 0x59)  // pause
                        break;
# endif
                    keySetState(code, 0);
                    if (keypresscallback)
                        keypresscallback(code, 0);
                }
                break;
            }

            case SDL_MOUSEWHEEL:
                // initprintf("wheel y %d\n",ev.wheel.y);
                if (ev.wheel.y > 0)
                {
                    g_mouseBits |= 16;
                    if (g_mouseCallback)
                        g_mouseCallback(5, 1);
                }
                if (ev.wheel.y < 0)
                {
                    g_mouseBits |= 32;
                    if (g_mouseCallback)
                        g_mouseCallback(6, 1);
                }
                break;

            case SDL_WINDOWEVENT:
                switch (ev.window.event)
                {
                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        appactive = (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED);
                        if (g_mouseGrabbed && g_mouseEnabled)
                            grabmouse_low(appactive);
#ifdef _WIN32
                        // Win_SetKeyboardLayoutUS(appactive);

                        if (backgroundidle)
                            SetPriorityClass(GetCurrentProcess(), appactive ? NORMAL_PRIORITY_CLASS : IDLE_PRIORITY_CLASS);
#endif
                        break;

                    case SDL_WINDOWEVENT_MOVED:
                        if (windowpos)
                        {
                            windowx = ev.window.data1;
                            windowy = ev.window.data2;
                        }
                        break;
                    case SDL_WINDOWEVENT_ENTER:
                        g_mouseInsideWindow = 1;
                        break;
                    case SDL_WINDOWEVENT_LEAVE:
                        g_mouseInsideWindow = 0;
                        break;
                }
                break;

            default:
                rv = handleevents_sdlcommon(&ev);
                break;
        }
    }

    return rv;
}
#endif

int32_t handleevents(void)
{
#ifdef __ANDROID__
    if (mobile_halted) return 0;
#endif

    int32_t rv;

    if (inputchecked && g_mouseEnabled)
    {
        if (g_mouseCallback)
        {
            if (g_mouseBits & 16)
                g_mouseCallback(5, 0);
            if (g_mouseBits & 32)
                g_mouseCallback(6, 0);
        }
        g_mouseBits &= ~(16 | 32);
    }

    rv = handleevents_pollsdl();

    inputchecked = 0;
    timerUpdate();

#ifndef _WIN32
    startwin_idle(NULL);
#endif

    return rv;
}

#ifdef __PSP2__
#if SDL_MAJOR_VERSION == 1
static void PSP2_CreateAndPushKeyEvent(SDLKey key_sym, Uint8 event_type) {
    SDL_Event event;
    event.type = event_type;
    event.key.keysym.sym = key_sym;
    event.key.keysym.unicode = key_sym;
    event.key.keysym.mod = 0;
    SDL_PushEvent(&event);
}
#else
static void PSP2_CreateAndPushKeyEvent(SDL_Keycode key_sym, Uint32 event_type) {
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = event_type;
    event.key.keysym.sym = key_sym;
    event.key.keysym.mod = 0;
    event.key.state = (event_type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&event);
}
#endif

void PSP2_StartTextInput(char *initial_text) {
    if (!can_use_IME_keyboard)
        return;
    
    can_use_IME_keyboard = 0;

    char *text = kbdvita_get("Enter New Text:", initial_text, 16, 0);

    // the build engine keyboard fifo buffer can only store 32 keys
    int i = strlen(initial_text);
    if (i > 32 - strlen(text))
        i = 32 - strlen(text);

    while (i > 0) {
        // delete everything that might be there
        PSP2_CreateAndPushKeyEvent(SDLK_BACKSPACE, SDL_KEYDOWN);
        PSP2_CreateAndPushKeyEvent(SDLK_BACKSPACE, SDL_KEYUP);
        i--;
    }

    if (text != NULL)
    {
        // enter the new text
        int i=0;
        while (text[i]!=0 && i<16) {
            if (text[i]>='A' && text[i]<='Z')
                text[i]+=32;
            // convert lf to return
            if (text[i]==10)
                text[i]=SDLK_RETURN;
            PSP2_CreateAndPushKeyEvent((SDL_Keycode) text[i], SDL_KEYDOWN);
            PSP2_CreateAndPushKeyEvent((SDL_Keycode) text[i], SDL_KEYUP);
            i++;
        }
    }

    // append return
    PSP2_CreateAndPushKeyEvent(SDLK_RETURN, SDL_KEYDOWN);
    PSP2_CreateAndPushKeyEvent(SDLK_RETURN, SDL_KEYUP);
}

void PSP2_StopTextInput() {
    can_use_IME_keyboard = 1;
}
#endif

#if SDL_MAJOR_VERSION == 1
#include "sdlayer12.cpp"
#endif
