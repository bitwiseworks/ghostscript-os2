/* Copyright (C) 2001-2012 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134, San Rafael,
   CA  94903, U.S.A., +1(415)492-9861, for further information.
*/


/* Ghostscript DLL loader for OS/2 */
/* For WINDOWCOMPAT (console mode) application */

/* Russell Lang  1996-06-05 */

/* Updated 2001-03-10 by rjl
 *  New DLL interface, uses display device.
 *  Uses same interface to gspmdrv.c as os2pm device.
 */

#define OS2EMX_PLAIN_CHAR
#define INCL_DOS
#define INCL_DOSERRORS
#define INCL_WIN	/* to get bits/pixel of display */
#define INCL_GPI	/* to get bits/pixel of display */
#include <os2.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <malloc.h>
#include "gscdefs.h"
#define GS_REVISION gs_revision
#include "ierrors.h"
#include "iapi.h"
#include "gdevdsp.h"

#define MAXSTR 256
#define BITMAPINFO2_SIZE 40
const char *szDllName = "GSDLL2.DLL";
char start_string[] = "systemdict /start get exec\n";
int debug = FALSE;
FILE *logfileFH;
char logBuf[MAXSTR];

#define MIN_COMMIT 4096		/* memory is committed in these size chunks */
#define ID_NAME "GSPMDRV_%u_%u"
#define SHARED_NAME "\\SHAREMEM\\%s"
#define SYNC_NAME   "\\SEM32\\SYNC_%s"
#define MUTEX_NAME  "\\SEM32\\MUTEX_%s"

LONG display_planes;
LONG display_bitcount;
LONG display_hasPalMan;

static int gsdll_stdin(void *instance, char *buf, int len);
static int gsdll_stdout(void *instance, const char *str, int len);
static int gsdll_stderr(void *instance, const char *str, int len);
static int display_open(void *handle, void *device);
static int display_preclose(void *handle, void *device);
static int display_close(void *handle, void *device);
static int display_presize(void *handle, void *device, int width, int height,
        int raster, unsigned int format);
static int display_size(void *handle, void *device, int width, int height,
        int raster, unsigned int format, unsigned char *pimage);
static int display_sync(void *handle, void *device);
static int display_page(void *handle, void *device, int copies, int flush);
static int display_update(void *handle, void *device, int x, int y,
        int w, int h);
void *display_memalloc(void *handle, void *device, unsigned long size);
int display_memfree(void *handle, void *device, void *mem);
void image_color(unsigned int format, int index,
    unsigned char *r, unsigned char *g, unsigned char *b);
void gs_addmess(const char *str);
BOOL gs_free_dll(void);
void gs_load_dll_cleanup(void);
BOOL gs_load_dll(void);


/* main structure with info about the GS DLL */
typedef struct tagGSDLL {
        HMODULE hmodule;	/* DLL module handle */
        PFN_gsapi_revision revision;
        PFN_gsapi_new_instance new_instance;
        PFN_gsapi_delete_instance delete_instance;
        PFN_gsapi_set_stdio set_stdio;
        PFN_gsapi_set_poll set_poll;
        PFN_gsapi_set_display_callback set_display_callback;
        PFN_gsapi_init_with_args init_with_args;
        PFN_gsapi_run_string run_string;
        PFN_gsapi_exit exit;
} GSDLL;

GSDLL gsdll;
void *instance;
TID tid;

void
gs_addmess(const char *str)
{
#if defined(__EMX__)
    if (logfileFH != NULL) {
        fprintf(logfileFH, str);
        fflush(logfileFH);
    }
    else {
        fputs(str, stderr);
        fflush(stderr);
    }
#else
    fputs(str, stdout);
    fflush(stdout);
#endif
}

/*********************************************************************/
/* load and unload the Ghostscript DLL */

/* free GS DLL */
/* This should only be called when gsdll_execute has returned */
/* TRUE means no error */
BOOL
gs_free_dll(void)
{
    APIRET rc;

    if (debug)
        gs_addmess("gs_free_dll\n");

    if (gsdll.hmodule == (HMODULE) NULL)
        return TRUE;
    rc = DosFreeModule(gsdll.hmodule);
    if (rc == 12) rc = 0; //we ignore rc == 12 (ERROR_INVALID_ACCESS)

    if (rc) {
        sprintf(logBuf, "DosFreeModule returns %d\n", rc);
        gs_addmess(logBuf);
        gs_addmess("Unloaded GSDLL\n\n");
    }
    return !rc;
}

void
gs_load_dll_cleanup(void)
{
    if (debug)
        gs_addmess("gs_load_dll_cleanup\n");

    gs_free_dll();
}

/* load GS DLL if not already loaded */
/* return TRUE if OK */
BOOL
gs_load_dll(void)
{
    char buf[MAXSTR + 40];
    APIRET rc;
    char *p;
    const char *dllname;
    PTIB pptib;
    PPIB pppib;
    char szExePath[MAXSTR];
    char fullname[1024];
    const char *shortname;
    gsapi_revision_t rv;

    if ((rc = DosGetInfoBlocks(&pptib, &pppib)) != 0) {
        sprintf(logBuf, "Couldn't get pid, rc = \n", rc);
        gs_addmess(logBuf);
        return FALSE;
    }
    /* get path to EXE */
    if ((rc = DosQueryModuleName(pppib->pib_hmte, sizeof(szExePath),
        szExePath)) != 0) {
        sprintf(logBuf, "Couldn't get module name, rc = %d\n", rc);
        gs_addmess(logBuf);
        return FALSE;
    }
    if ((p = strrchr(szExePath, '\\')) != (char *)NULL) {
        p++;
        *p = '\0';
    }
    dllname = szDllName;
    if (debug) {
        sprintf(logBuf, "Trying to load %s\n", dllname);
        gs_addmess(logBuf);
    }
    memset(buf, 0, sizeof(buf));
    rc = DosLoadModule(buf, sizeof(buf), dllname, &gsdll.hmodule);
    if (rc) {
        /* failed */
        /* try again, with path of EXE */
        if ((shortname = strrchr((char *)szDllName, '\\'))
            == (const char *)NULL)
            shortname = szDllName;
        strcpy(fullname, szExePath);
        if ((p = strrchr(fullname, '\\')) != (char *)NULL)
            p++;
        else
            p = fullname;
        *p = '\0';
        strcat(fullname, shortname);
        dllname = fullname;
        if (debug) {
            sprintf(logBuf, "Trying to load %s\n", dllname);
            gs_addmess(logBuf);
        }
        rc = DosLoadModule(buf, sizeof(buf), dllname, &gsdll.hmodule);
        if (rc) {
            /* failed again */
            /* try once more, this time on system search path */
            dllname = shortname;
            if (debug) {
                sprintf(logBuf, "Trying to load %s\n", dllname);
                gs_addmess(logBuf);
            }
            rc = DosLoadModule(buf, sizeof(buf), dllname, &gsdll.hmodule);
        }
    }
    if (rc == 0) {
        if (debug)
            gs_addmess("Loaded Ghostscript DLL\n");
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_REVISION",
                (PFN *) (&gsdll.revision))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_REVISION, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        /* check DLL version */
        if (gsdll.revision(&rv, sizeof(rv)) != 0) {
            sprintf(logBuf, "Unable to identify Ghostscript DLL revision - it must be newer than needed.\n");
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }

        if (rv.revision != GS_REVISION) {
            sprintf(logBuf, "Wrong version of DLL found.\n  Found version %ld\n  Need version  %ld\n", rv.revision, (long)GS_REVISION);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }

        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_NEW_INSTANCE",
                (PFN *) (&gsdll.new_instance))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_NEW_INSTANCE, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_DELETE_INSTANCE",
                (PFN *) (&gsdll.delete_instance))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_DELETE_INSTANCE, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_SET_STDIO",
                (PFN *) (&gsdll.set_stdio))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_SET_STDIO, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_SET_DISPLAY_CALLBACK",
                (PFN *) (&gsdll.set_display_callback))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_SET_DISPLAY_CALLBACK, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_SET_POLL",
                (PFN *) (&gsdll.set_poll))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_SET_POLL, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0,
                "GSAPI_INIT_WITH_ARGS",
                (PFN *) (&gsdll.init_with_args))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_INIT_WITH_ARGS, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_RUN_STRING",
                (PFN *) (&gsdll.run_string))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_RUN_STRING, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
        if ((rc = DosQueryProcAddr(gsdll.hmodule, 0, "GSAPI_EXIT",
                (PFN *) (&gsdll.exit))) != 0) {
            sprintf(logBuf, "Can't find GSAPI_EXIT, rc = %d\n", rc);
            gs_addmess(logBuf);
            gs_load_dll_cleanup();
            return FALSE;
        }
    } else {
        sprintf(logBuf, "Can't load Ghostscript DLL %s \nDosLoadModule rc = %d\n",
            szDllName, rc);
        gs_addmess(logBuf);
        gs_load_dll_cleanup();
        return FALSE;
    }
    if (debug)
        gs_addmess("gs_load_dll out\n");

    return TRUE;
}

/*********************************************************************/
/* stdio functions */

static int
gsdll_stdin(void *instance, char *buf, int len)
{
    return read(fileno(stdin), buf, len);
}

static int
gsdll_stdout(void *instance, const char *str, int len)
{
    fwrite(str, 1, len, stdout);
    fflush(stdout);
    return len;
}

static int
gsdll_stderr(void *instance, const char *str, int len)
{
    fwrite(str, 1, len, stderr);
    fflush(stderr);
    return len;
}

/*********************************************************************/
/* display device */

typedef struct IMAGE_S IMAGE;
struct IMAGE_S {
    void *handle;
    void *device;
    PID pid;		/* PID of our window (CMD.EXE) */
    HEV sync_event;	/* tell gspmdrv to redraw window */
    HMTX bmp_mutex;	/* protects access to bitmap */
    HQUEUE term_queue;	/* notification that gspmdrv has finished */
    ULONG session_id;	/* id of gspmdrv */
    PID process_id;	/* of gspmdrv */

    int width;
    int height;
    int raster;
    int format;

    BOOL format_known;

    unsigned char *bitmap;
    ULONG committed;
    IMAGE *next;
};

IMAGE *first_image = NULL;
static IMAGE *image_find(void *handle, void *device);

static IMAGE *
image_find(void *handle, void *device)
{
    IMAGE *img;
    for (img = first_image; img!=0; img=img->next) {
        if ((img->handle == handle) && (img->device == device))
            return img;
    }
    return NULL;
}

/* start gspmdrv.exe */
static int run_gspmdrv(IMAGE *img)
{
    PCHAR pdrvname = "gspmdrv.exe";
    CHAR error_message[256];
    CHAR term_queue_name[128];
    CHAR id[128];
    CHAR arg[1024];
    STARTDATA sdata;
    APIRET rc;
    PTIB pptib;
    PPIB pppib;
    CHAR progname[256];
    PCHAR tail;

    if (debug) {
        gs_addmess("run_gspmdrv: starting\n");
    }
    sprintf(id, ID_NAME, img->pid, (ULONG)img->device);

    /* Create termination queue - used to find out when gspmdrv terminates */
    sprintf(term_queue_name, "\\QUEUES\\TERMQ_%s", id);
    if (DosCreateQueue(&(img->term_queue), QUE_FIFO, term_queue_name)) {
        gs_addmess("run_gspmdrv: failed to create termination queue\n");
        return gs_error_limitcheck;
    }
    /* get full path to gsos2.exe and hence path to gspmdrv.exe */
    if ((rc = DosGetInfoBlocks(&pptib, &pppib)) != 0) {
        sprintf(logBuf, "run_gspmdrv: Couldn't get module handle, rc = %d\n",
            rc);
        gs_addmess(logBuf);
        return gs_error_limitcheck;
    }
    if ((rc = DosQueryModuleName(pppib->pib_hmte, sizeof(progname) - 1,
        progname)) != 0) {
        sprintf(logBuf, "run_gspmdrv: Couldn't get module name, rc = %d\n",
            rc);
        gs_addmess(logBuf);
        return gs_error_limitcheck;
    }
    if ((tail = strrchr(progname, '\\')) != (PCHAR) NULL) {
        tail++;
        *tail = '\0';
    } else
        tail = progname;
    strcat(progname, pdrvname);

    /* Open the PM driver session gspmdrv.exe */
    /* arguments are: */
    /*  (1) -d (display) option */
    /*  (2) id string */
    sprintf(arg, "-d %s", id);

    /* because gspmdrv.exe is a different EXE type to gs.exe,
     * we must use start session not DosExecPgm() */
    sdata.Length = sizeof(sdata);
    sdata.Related = SSF_RELATED_CHILD;	/* to be a child  */
    sdata.FgBg = SSF_FGBG_BACK;	/* start in background */
    sdata.TraceOpt = 0;
    sdata.PgmTitle = "Ghostscript PM driver session";
    sdata.PgmName = progname;
    sdata.PgmInputs = arg;
    sdata.TermQ = term_queue_name;
    sdata.Environment = pppib->pib_pchenv;	/* use Parent's environment */
    sdata.InheritOpt = 0;	/* Can't inherit from parent because */
                                /* different sesison type */
    sdata.SessionType = SSF_TYPE_DEFAULT;	/* default is PM */
    sdata.IconFile = NULL;
    sdata.PgmHandle = 0;
    sdata.PgmControl = 0;
    sdata.InitXPos = 0;
    sdata.InitYPos = 0;
    sdata.InitXSize = 0;
    sdata.InitYSize = 0;
    sdata.ObjectBuffer = error_message;
    sdata.ObjectBuffLen = sizeof(error_message);

    rc = DosStartSession(&sdata, &img->session_id, &img->process_id);
    if (rc == ERROR_FILE_NOT_FOUND) {
        sdata.PgmName = pdrvname;
        rc = DosStartSession(&sdata, &img->session_id, &img->process_id);
    }
    if (rc) {
        sprintf(logBuf, "run_gspmdrv: failed to run %s, rc = %d\n",
            sdata.PgmName, rc);
        gs_addmess(logBuf);
        sprintf(logBuf, "run_gspmdrv: error_message: %s\n", error_message);
        gs_addmess(logBuf);
        return gs_error_limitcheck;
    }
    if (debug)
        gs_addmess("run_gspmdrv: returning\n");
    return 0;
}

void
image_color(unsigned int format, int index,
    unsigned char *r, unsigned char *g, unsigned char *b)
{
    switch (format & DISPLAY_COLORS_MASK) {
        case DISPLAY_COLORS_NATIVE:
            switch (format & DISPLAY_DEPTH_MASK) {
                case DISPLAY_DEPTH_1:
                    *r = *g = *b = (index ? 0 : 255);
                    break;
                case DISPLAY_DEPTH_4:
                    if (index == 7)
                        *r = *g = *b = 170;
                    else if (index == 8)
                        *r = *g = *b = 85;
                    else {
                        int one = index & 8 ? 255 : 128;
                        *r = (index & 4 ? one : 0);
                        *g = (index & 2 ? one : 0);
                        *b = (index & 1 ? one : 0);
                    }
                    break;
                case DISPLAY_DEPTH_8:
                    /* palette of 96 colors */
                    /* 0->63 = 00RRGGBB, 64->95 = 010YYYYY */
                    if (index < 64) {
                        int one = 255 / 3;
                        *r = ((index & 0x30) >> 4) * one;
                        *g = ((index & 0x0c) >> 2) * one;
                        *b =  (index & 0x03) * one;
                    }
                    else {
                        int val = index & 0x1f;
                        *r = *g = *b = (val << 3) + (val >> 2);
                    }
                    break;
            }
            break;
        case DISPLAY_COLORS_GRAY:
            switch (format & DISPLAY_DEPTH_MASK) {
                case DISPLAY_DEPTH_1:
                    *r = *g = *b = (index ? 255 : 0);
                    break;
                case DISPLAY_DEPTH_4:
                    *r = *g = *b = (unsigned char)((index<<4) + index);
                    break;
                case DISPLAY_DEPTH_8:
                    *r = *g = *b = (unsigned char)index;
                    break;
            }
            break;
    }
}

static int image_palette_size(int format)
{
    int palsize = 0;
    switch (format & DISPLAY_COLORS_MASK) {
        case DISPLAY_COLORS_NATIVE:
            switch (format & DISPLAY_DEPTH_MASK) {
                case DISPLAY_DEPTH_1:
                    palsize = 2;
                    break;
                case DISPLAY_DEPTH_4:
                    palsize = 16;
                    break;
                case DISPLAY_DEPTH_8:
                    palsize = 96;
                    break;
            }
            break;
        case DISPLAY_COLORS_GRAY:
            switch (format & DISPLAY_DEPTH_MASK) {
                case DISPLAY_DEPTH_1:
                    palsize = 2;
                    break;
                case DISPLAY_DEPTH_4:
                    palsize = 16;
                    break;
                case DISPLAY_DEPTH_8:
                    palsize = 256;
                    break;
            }
            break;
    }
    return palsize;
}

/* New device has been opened */
/* Tell user to use another device */
static int display_open(void *handle, void *device)
{
    IMAGE *img;
    PTIB pptib;
    PPIB pppib;
    CHAR id[128];
    CHAR name[128];
    PBITMAPINFO2 bmi;

    if (debug) {
        sprintf(logBuf, "display_open(0x%x, 0x%x)\n", handle, device);
        gs_addmess(logBuf);
    }

    if (first_image) {
        /* gsos2.exe is a console application, and displays using
         * gspmdrv.exe which is a PM application.  To start
         * gspmdrv.exe, DosStartSession is used with SSF_RELATED_CHILD.
         * A process can have only one child session marked SSF_RELATED_CHILD.
         * When we call DosStopSession for the second session, it will
         * close, but it will not write to the termination queue.
         * When we wait for the session to end by reading the
         * termination queue, we wait forever.
         * For this reason, multiple image windows are disabled
         * for OS/2.
         * To get around this, we would need to replace the current
         * method of one gspmdrv.exe session per window, to having
         * a new PM application which can display multiple windows
         * within a single session.
         */
        return gs_error_limitcheck;
    }

    img = (IMAGE *)malloc(sizeof(IMAGE));
    if (img == NULL)
        return gs_error_limitcheck;
    memset(img, 0, sizeof(IMAGE));

    /* add to list */
    img->next = first_image;
    first_image = img;

    /* remember device and handle */
    img->handle = handle;
    img->device = device;

    /* Derive ID from process ID */
    if (DosGetInfoBlocks(&pptib, &pppib)) {
        gs_addmess("display_open: Couldn't get pid\n");
        return gs_error_limitcheck;
    }
    img->pid = pppib->pib_ulppid;	/* use parent (CMD.EXE) pid */
    sprintf(id, ID_NAME, img->pid, (ULONG) img->device);

    /* Create update event semaphore */
    sprintf(name, SYNC_NAME, id);
    if (DosCreateEventSem(name, &(img->sync_event), 0, FALSE)) {
        sprintf(logBuf, "display_open: failed to create event semaphore %s\n", name);
        gs_addmess(logBuf);
        return gs_error_limitcheck;
    }
    /* Create mutex - used for preventing gspmdrv from accessing */
    /* bitmap while we are changing the bitmap size. Initially unowned. */
    sprintf(name, MUTEX_NAME, id);
    if (DosCreateMutexSem(name, &(img->bmp_mutex), 0, FALSE)) {
        DosCloseEventSem(img->sync_event);
        sprintf(logBuf, "display_open: failed to create mutex semaphore %s\n", name);
        gs_addmess(logBuf);
        return gs_error_limitcheck;
    }

    /* Shared memory is common to all processes so we don't want to
     * allocate too much.
     */
    sprintf(name, SHARED_NAME, id);
    if (DosAllocSharedMem((PPVOID) & img->bitmap, name,
                      13 * 1024 * 1024, PAG_READ | PAG_WRITE)) {
        sprintf(logBuf, "display_open: failed allocating shared BMP memory %s\n", name);
        gs_addmess(logBuf);
        return gs_error_limitcheck;
    }

    /* commit one page so there is enough storage for a */
    /* bitmap header and palette */
    if (DosSetMem(img->bitmap, MIN_COMMIT, PAG_COMMIT | PAG_DEFAULT)) {
        DosFreeMem(img->bitmap);
        gs_addmess("display: failed committing BMP memory\n");
        return gs_error_limitcheck;
    }
    img->committed = MIN_COMMIT;

    /* write a zero pixel BMP */
    bmi = (PBITMAPINFO2) img->bitmap;
    bmi->cbFix = BITMAPINFO2_SIZE; /* OS/2 2.0 and Windows 3.0 compatible */
    bmi->cx = 0;
    bmi->cy = 0;
    bmi->cPlanes = 1;
    bmi->cBitCount = 24;
    bmi->ulCompression = BCA_UNCOMP;
    bmi->cbImage = 0;
    bmi->cxResolution = 0;
    bmi->cyResolution = 0;
    bmi->cclrUsed = 0;
    bmi->cclrImportant = 0;

    /* delay start of gspmdrv until size is known */

    if (debug)
        gs_addmess("display_open out\n");
    return 0;
}

static int display_preclose(void *handle, void *device)
{
    IMAGE *img;
    REQUESTDATA Request;
    ULONG DataLength;
    PVOID DataAddress;
    BYTE ElemPriority;

    if (debug) {
        sprintf(logBuf, "display_preclose(0x%x, 0x%x)\n", handle, device);
        gs_addmess(logBuf);
    }

    img = image_find(handle, device);
    if (img) {
        if (img->session_id) {
            /* Close gspmdrv driver */
            DosStopSession(STOP_SESSION_SPECIFIED, img->session_id);
            Request.pid = img->pid;
            Request.ulData = 0;
            /* wait for termination queue, queue is then closed */
            /* by session manager */
            DosReadQueue(img->term_queue, &Request, &DataLength,
                     &DataAddress, 0, DCWW_WAIT, &ElemPriority, (HEV) NULL);
            /* queue needs to be closed by us */
            DosCloseQueue(img->term_queue);
        }
        img->session_id = 0;
        img->term_queue = 0;

        DosCloseEventSem(img->sync_event);
        DosCloseMutexSem(img->bmp_mutex);
    }

    if (debug)
        gs_addmess("display_preclose out\n");

    return 0;
}

static int display_close(void *handle, void *device)
{
    IMAGE *img;

    if (debug) {
        sprintf(logBuf, "display_close(0x%x, 0x%x)\n", handle, device);
        gs_addmess(logBuf);
    }

    img = image_find(handle, device);
    if (img) {
        /* gspmdrv was closed by display_preclose */
        /* release memory */
        DosFreeMem(img->bitmap);
        img->bitmap = (unsigned char *)NULL;
        img->committed = 0;
    }
    if (debug)
        gs_addmess("display_close out\n");

    return 0;
}

static int display_presize(void *handle, void *device, int width, int height,
        int raster, unsigned int format)
{
    IMAGE *img;

    if (debug) {
        sprintf(logBuf, "display_presize(0x%x 0x%x, %d, %d, %d, %d)\n",
        handle, device, width, height, raster, format);
        gs_addmess(logBuf);
    }

    img = image_find(handle, device);
    if (img) {
        int color = format & DISPLAY_COLORS_MASK;
        int depth = format & DISPLAY_DEPTH_MASK;
        int alpha = format & DISPLAY_ALPHA_MASK;
        img->format_known = FALSE;
        if ( ((color == DISPLAY_COLORS_NATIVE) ||
              (color == DISPLAY_COLORS_GRAY))
                 &&
             ((depth == DISPLAY_DEPTH_1) ||
              (depth == DISPLAY_DEPTH_4) ||
              (depth == DISPLAY_DEPTH_8)) )
            img->format_known = TRUE;
        if ((color == DISPLAY_COLORS_RGB) && (depth == DISPLAY_DEPTH_8) &&
            (alpha == DISPLAY_ALPHA_NONE))
            img->format_known = TRUE;
        if (!img->format_known) {
            sprintf(logBuf, "display_presize: format %d = 0x%x is unsupported\n", format, format);
            gs_addmess(logBuf);
            return gs_error_limitcheck;
        }
        /* grab mutex to stop other thread using bitmap */
        DosRequestMutexSem(img->bmp_mutex, 120000);
        /* remember parameters so we can figure out where to allocate bitmap */
        img->width = width;
        img->height = height;
        img->raster = raster;
        img->format = format;
    }
    if (debug)
        gs_addmess("display_presize out\n");

    return 0;
}

static int display_size(void *handle, void *device, int width, int height,
        int raster, unsigned int format, unsigned char *pimage)
{
    IMAGE *img;
    PBITMAPINFO2 bmi;
    int nColors;
    int i;

    if (debug) {
        sprintf(logBuf, "display_size(0x%x 0x%x, %d, %d, %d, %d, %d, 0x%x)\n",
          handle, device, width, height, raster, format, pimage);
        gs_addmess(logBuf);
    }

    img = image_find(handle, device);
    if (img) {
        if (!img->format_known)
            return gs_error_limitcheck;

        img->width = width;
        img->height = height;
        img->raster = raster;
        img->format = format;
        /* write BMP header including palette */
        bmi = (PBITMAPINFO2) img->bitmap;
        bmi->cbFix = BITMAPINFO2_SIZE;
        bmi->cx = img->width;
        bmi->cy = img->height;
        bmi->cPlanes = 1;
        bmi->cBitCount = 24;
        bmi->ulCompression = BCA_UNCOMP;
        bmi->cbImage = 0;
        bmi->cxResolution = 0;
        bmi->cyResolution = 0;
        bmi->cclrUsed = bmi->cclrImportant = image_palette_size(format);

        switch (img->format & DISPLAY_DEPTH_MASK) {
            default:
            case DISPLAY_DEPTH_1:
                bmi->cBitCount = 1;
                break;
            case DISPLAY_DEPTH_4:
                bmi->cBitCount = 4;
                break;
            case DISPLAY_DEPTH_8:
                if ((img->format & DISPLAY_COLORS_MASK) == DISPLAY_COLORS_NATIVE)
                    bmi->cBitCount = 8;
                else if ((img->format & DISPLAY_COLORS_MASK) == DISPLAY_COLORS_GRAY)
                    bmi->cBitCount = 8;
                else
                    bmi->cBitCount = 24;
                break;
        }

        /* add palette if needed */
        nColors = bmi->cclrUsed;
        if (nColors) {
            unsigned char *p;
            p = img->bitmap + BITMAPINFO2_SIZE;
            for (i = 0; i < nColors; i++) {
                image_color(img->format, i, p+2, p+1, p);
                *(p+3) = 0;
                p += 4;
            }
        }

        /* release mutex to allow other thread to use bitmap */
        DosReleaseMutexSem(img->bmp_mutex);

        if (debug) {
            gs_addmess("\nBMP dump\n");
            sprintf(logBuf, " bitmap=%lx\n", img->bitmap);
            gs_addmess(logBuf);
            sprintf(logBuf, " committed=%lx\n", img->committed);
            gs_addmess(logBuf);
            sprintf(logBuf, " cx=%d\n", bmi->cx);
            gs_addmess(logBuf);
            sprintf(logBuf, " cy=%d\n", bmi->cy);
            gs_addmess(logBuf);
            sprintf(logBuf, " cPlanes=%d\n", bmi->cPlanes);
            gs_addmess(logBuf);
            sprintf(logBuf, " cBitCount=%d\n", bmi->cBitCount);
            gs_addmess(logBuf);
            sprintf(logBuf, " ulCompression=%d\n", bmi->ulCompression);
            gs_addmess(logBuf);
            sprintf(logBuf, " cbImage=%d\n", bmi->cbImage);
            gs_addmess(logBuf);
            sprintf(logBuf, " cxResolution=%d\n", bmi->cxResolution);
            gs_addmess(logBuf);
            sprintf(logBuf, " cyResolution=%d\n", bmi->cyResolution);
            gs_addmess(logBuf);
            sprintf(logBuf, " cclrUsed=%d\n", bmi->cclrUsed);
            gs_addmess(logBuf);
            sprintf(logBuf, " cclrImportant=%d\n", bmi->cclrImportant);
            gs_addmess(logBuf);
        }
    }
    if (debug)
        gs_addmess("display_size out\n");

    return 0;
}

static int display_sync(void *handle, void *device)
{
    IMAGE *img;

    if (debug) {
        sprintf(logBuf, "display_sync(0x%x, 0x%x)\n", handle, device);
        gs_addmess(logBuf);
    }

    img = image_find(handle, device);
    if (img) {
        if (!img->format_known)
            return gs_error_limitcheck;
        /* delay starting gspmdrv until display_size has been called */
        if (!img->session_id && (img->width != 0) && (img->height != 0))
           run_gspmdrv(img);
        DosPostEventSem(img->sync_event);
    }

    if (debug)
        gs_addmess("display_sync out\n");

    return 0;
}

static int display_page(void *handle, void *device, int copies, int flush)
{
    if (debug)
        sprintf(logBuf, "display_page(0x%x, 0x%x, copies=%d, flush=%d)\n",
          handle, device, copies, flush);
        gs_addmess(logBuf);

    display_sync(handle, device);

    if (debug)
        gs_addmess("display_page out\n");

    return 0;
}

void *display_memalloc(void *handle, void *device, unsigned long size)
{
    IMAGE *img;
    unsigned long needed;
    unsigned long header;
    APIRET rc;
    void *mem = NULL;

    if (debug) {
        sprintf(logBuf, "display_memalloc(0x%x 0x%x %d)\n",
          handle, device, size);
        gs_addmess(logBuf);
    }

    img = image_find(handle, device);
    if (img) {
        /* we don't actually allocate memory here, we only commit
         * preallocated shared memory.
         * First work out size of header + palette.
         * We allocate space for the header and tell Ghostscript
         * that the memory starts just after the header.
         * We rely on the Ghostscript memory device placing the
         * raster at the start of this memory and having a
         * raster length the same as the length of a BMP row.
         */
        header = BITMAPINFO2_SIZE + image_palette_size(img->format) * 4;

        /* Work out if we need to commit more */
        needed = (size + header + MIN_COMMIT - 1) & (~(MIN_COMMIT - 1));
        if (needed > img->committed) {
            /* commit more memory */
            if ((rc = DosSetMem(img->bitmap + img->committed,
                      needed - img->committed, PAG_COMMIT | PAG_DEFAULT))) {
                sprintf(logBuf, "No memory in display_memalloc rc = %d\n", rc);
                gs_addmess(logBuf);
                return NULL;
            }
            img->committed = needed;
        }
        mem = img->bitmap + header;
    }

    if (debug) {
        sprintf(logBuf, "  returning 0x%x\n", (int)mem);
        gs_addmess(logBuf);
    }

    return mem;
}

int display_memfree(void *handle, void *device, void *mem)
{
    /* we can't uncommit shared memory, so do nothing */
    /* memory will be released when device is closed */
    if (debug) {
        sprintf(logBuf, "display_memfree(0x%x, 0x%x, 0x%x)\n",
           handle, device, mem);
        gs_addmess(logBuf);
    }
    return 0;
}

static int display_update(void *handle, void *device,
    int x, int y, int w, int h)
{
    /* unneeded - we are running image window in a separate process */
    return 0;
}

display_callback display = {
    sizeof(display_callback),
    DISPLAY_VERSION_MAJOR,
    DISPLAY_VERSION_MINOR,
    display_open,
    display_preclose,
    display_close,
    display_presize,
    display_size,
    display_sync,
    display_page,
    display_update,
    display_memalloc,
    display_memfree
};

/*********************************************************************/

int
main(int argc, char *argv[])
{
    int exit_status;
    int code = 1, code1;
    void *instance;
    int nargc;
    char **nargv;
    char dformat[64];
    int exit_code;
    char logfile[_MAX_PATH +1];
    char *env;

    logfileFH = NULL;
    debug = FALSE;
    if (getenv("GS_DEBUG") != NULL) {
        debug = TRUE;
        env = getenv("LOGFILES");
        if (env != NULL)
           snprintf(logfile, sizeof(logfile), "%s/%s", env, "gs_debug.log");
        else
           snprintf(logfile, sizeof(logfile), "%s", "gs_debug.log");
        logfileFH = fopen(logfile, "a");
    }

    if (debug)
        gs_addmess("main init\n");

    if (!gs_load_dll()) {
        sprintf(logBuf, "Can't load %s\n", szDllName);
        gs_addmess(logBuf);
        exit_status = -1;
        goto bail;
    }

    /* insert -dDisplayFormat=XXXXX as first argument */
    {   int format = DISPLAY_COLORS_NATIVE | DISPLAY_ALPHA_NONE |
                DISPLAY_DEPTH_1 | DISPLAY_LITTLEENDIAN | DISPLAY_BOTTOMFIRST;
        int depth;
        HPS ps = WinGetPS(HWND_DESKTOP);
        HDC hdc = GpiQueryDevice(ps);
        DevQueryCaps(hdc, CAPS_COLOR_PLANES, 1, &display_planes);
        DevQueryCaps(hdc, CAPS_COLOR_BITCOUNT, 1, &display_bitcount);
        DevQueryCaps(hdc, CAPS_ADDITIONAL_GRAPHICS, 1, &display_hasPalMan);
        display_hasPalMan &= CAPS_PALETTE_MANAGER;
        depth = display_planes * display_bitcount;
        if ((depth <= 8) && !display_hasPalMan)
            depth = 24;		/* disaster: limited colours and no palette */
        WinReleasePS(ps);

        if (depth > 8)
            format = DISPLAY_COLORS_RGB | DISPLAY_ALPHA_NONE |
                DISPLAY_DEPTH_8 | DISPLAY_LITTLEENDIAN | DISPLAY_BOTTOMFIRST;
        else if (depth >= 8)
            format = DISPLAY_COLORS_NATIVE | DISPLAY_ALPHA_NONE |
                DISPLAY_DEPTH_8 | DISPLAY_LITTLEENDIAN | DISPLAY_BOTTOMFIRST;
        else if (depth >= 4)
            format = DISPLAY_COLORS_NATIVE | DISPLAY_ALPHA_NONE |
                DISPLAY_DEPTH_4 | DISPLAY_LITTLEENDIAN | DISPLAY_BOTTOMFIRST;
        sprintf(dformat, "-dDisplayFormat=%d", format);
    }
    if (debug) {
        sprintf(logBuf, "%s\n", dformat);
        gs_addmess(logBuf);
    }

    nargc = argc + 1;
    nargv = (char **)malloc((nargc + 1) * sizeof(char *));
    nargv[0] = argv[0];
    nargv[1] = dformat;
    memcpy(&nargv[2], &argv[1], argc * sizeof(char *));

    if ( (code = gsdll.new_instance(&instance, NULL)) == 0) {
        gsdll.set_stdio(instance, gsdll_stdin, gsdll_stdout, gsdll_stderr);
        gsdll.set_display_callback(instance, &display);

        if (debug)
            gs_addmess("main set_display_callback\n");

        code = gsdll.init_with_args(instance, nargc, nargv);
        if (debug) {
            sprintf(logBuf, "main init_with_args rc: %i\n", code);
            gs_addmess(logBuf);
        }

        if (code == 0)
            code = gsdll.run_string(instance, start_string, 0, &exit_code);
        if (debug) {
            sprintf(logBuf, "main run_string rc: %i\n", code);
            gs_addmess(logBuf);
        }
        code1 = gsdll.exit(instance);
        if (code == 0 || code == gs_error_Quit)
            code = code1;
        if (code == gs_error_Quit)
            code = 0;	/* user executed 'quit' */

        gsdll.delete_instance(instance);
    }

    gs_free_dll();

    free(nargv);

    exit_status = 0;
    switch (code) {
        case 0:
        case gs_error_Info:
        case gs_error_Quit:
            break;
        case gs_error_Fatal:
            exit_status = 1;
            break;
        default:
            exit_status = 255;
    }
    if (debug) {
        sprintf(logBuf, "main end rc: %i\n", code);
        gs_addmess(logBuf);
    }

bail:
    if (debug)
            fclose(logfileFH);

    return exit_status;
}
