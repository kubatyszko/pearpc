/* 
 *	PearPC
 *	sysdisplay.cc - screen access functions for POSIX
 *
 *	Copyright (C) 1999-2002 Stefan Weyergraf (stefan@weyergraf.de)
 *	Copyright (C) 1999-2004 Sebastian Biallas (sb@biallas.net)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <cstring>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "system/display.h"
#include "system/types.h"
#include "system/systhread.h"
#include "system/sysexcept.h"
#include "tools/data.h"
#include "tools/snprintf.h"

#define DPRINTF(a...)
//#define DPRINTF(a...) ht_printf(a)

byte *framebuffer = NULL;

struct {
	uint64 r_mask;
	uint64 g_mask;
	uint64 b_mask;
} PACKED gPosixRGBMask;

//extern "C" void __attribute__((regparm (3))) posix_vaccel_15_to_15(uint32 pixel, byte *input, byte *output);
//extern "C" void __attribute__((regparm (3))) posix_vaccel_15_to_32(uint32 pixel, byte *input, byte *output);

static uint8 x11_key_to_adb_key[256] = {
	// 0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x35,0x12,0x13,0x14,0x15,0x17,0x16,
	0x1a,0x1c,0x19,0x1d,0x1b,0x18,0x33,0x30,0x0c,0x0d,0x0e,0x0f,0x11,0x10,0x20,0x22,
	0x1f,0x23,0x21,0x1e,0x24,0x36,0x00,0x01,0x02,0x03,0x05,0x04,0x26,0x28,0x25,0x29,
	0x27,0x32,0x38,0x2a,0x06,0x07,0x08,0x09,0x0b,0x2d,0x2e,0x2b,0x2f,0x2c,0x38/*0x7b*/,0xff,
	0x3a,0x31,0xff,0x7a,0x78,0x63,0x76,0x60,0x61,0x62,0x64,0x65,0x6d,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x67,
	0x6f,0x73,0x3e,0x74,0x3b,0xff,0x3c,0x77,0x3d,0x79,0x72,0x75,0x4c,0x36,0xff,0xff,
	0xff,0x37,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
};

static void findMaskShiftAndSize(uint &shift, uint &size, uint bitmask)
{
	if (!bitmask) {
		shift = 0;
		size = 0;
		return;
	}
	shift = 0;
	while (!(bitmask & 1)) {
		shift++;
		bitmask >>= 1;
	}
	size = 0;
	while (bitmask & 1) {
		size++;
		bitmask >>= 1;
	}
}


static byte *Xframebuffer;
static Display *gXDisplay;
static Window gXWindow;
static GC gWhiteGC, gBlackGC;
static GC gGC;
static XImage *gXImage;
static XImage *gMenuXImage;
static XImage *gMouseXImage;
static Colormap gDefaultColormap;

static Queue *mEventQueue;
static DisplayCharacteristics mXChar;
static int mLastMouseX, mLastMouseY;
static int mCurMouseX, mCurMouseY;
static int mResetMouseX, mResetMouseY;
static int mHomeMouseX, mHomeMouseY;
static bool mMouseButton[3];
static bool mMouseEnabled;
static char *mTitle;
static char mCurTitle[200];
static byte *mouseData;
static byte *menuData;
class X11SystemDisplay: public SystemDisplay
{
	sys_thread redrawthread;
	sys_mutex mutex;
	
#define MASK(shift, size) (((1 << (size))-1)<<(shift))
	void dumpDisplayChar(const DisplayCharacteristics &chr)
	{
		fprintf(stderr, "\tdimensions:          %d x %d pixels\n", chr.width, chr.height);
		fprintf(stderr, "\tpixel size in bytes: %d\n", chr.bytesPerPixel);
		fprintf(stderr, "\tpixel size in bits:  %d\n", chr.bytesPerPixel*8);
		fprintf(stderr, "\tred_mask:            %08x (%d bits)\n", MASK(chr.redShift, chr.redSize), chr.redSize);
		fprintf(stderr, "\tgreen_mask:          %08x (%d bits)\n", MASK(chr.greenShift, chr.greenSize), chr.greenSize);
		fprintf(stderr, "\tblue_mask:           %08x (%d bits)\n", MASK(chr.blueShift, chr.blueSize), chr.blueSize);
		fprintf(stderr, "\tdepth:               %d\n", chr.redSize + chr.greenSize + chr.blueSize);
	}

	uint bitsPerPixelToXBitmapPad(uint bitsPerPixel)
	{
		if (bitsPerPixel <= 8) {
			return 8;
		} else if (bitsPerPixel <= 16) {
			return 16;
		} else {
			return 32;
		}
	}
public:
	X11SystemDisplay(const char *name, const DisplayCharacteristics &chr)
		:SystemDisplay(chr)
	{
		sys_create_mutex(&mutex);
		mEventQueue = new Queue(true);
		mMouseEnabled = false;
		gXDisplay = XOpenDisplay(NULL);		
		if (!gXDisplay) {
			printf("can't open X display!\n");
			exit(1);
		}

		if (bitsPerPixelToXBitmapPad(mClientChar.bytesPerPixel*8) != mClientChar.bytesPerPixel*8) {
			printf("nope. bytes per pixel is: %d. only 1,2 or 4 are allowed.\n", mClientChar.bytesPerPixel);
			exit(1);
		}
		
		mMenuHeight = 28;

		framebuffer = (byte*)malloc(mClientChar.width *
			mClientChar.height * mClientChar.bytesPerPixel);
		memset(framebuffer, 0, mClientChar.width *
			mClientChar.height * mClientChar.bytesPerPixel);
		
		mHomeMouseX = mClientChar.width / 2;
		mHomeMouseY = mClientChar.height / 2;
		
		mouseData = (byte*)malloc(2 * 2 * mClientChar.bytesPerPixel);
		memset(mouseData, 0, 2 * 2 * mClientChar.bytesPerPixel);
		
		int screen_num = DefaultScreen(gXDisplay);
		gXWindow = XCreateSimpleWindow(gXDisplay, 
			RootWindow(gXDisplay, screen_num), 0, 0,
				mClientChar.width, mClientChar.height+mMenuHeight,
				0, BlackPixel(gXDisplay, screen_num),
				BlackPixel(gXDisplay, screen_num));

		XVisualInfo info_tmpl;
		int ninfo;
		info_tmpl.screen = screen_num;
		info_tmpl.visualid = XVisualIDFromVisual(DefaultVisual(gXDisplay, screen_num));
		XVisualInfo *info = XGetVisualInfo(gXDisplay,
			VisualScreenMask | VisualIDMask, &info_tmpl, &ninfo);
#if 0
		fprintf(stderr, "got %d XVisualInfo's:\n", ninfo);
		for (int i=0; i<ninfo; i++) {
			fprintf(stderr, "XVisualInfo %d:\n", i);
			fprintf(stderr, "\tscreen = %d:\n", info[i].screen);
			fprintf(stderr, "\tdepth = %d:\n", info[i].depth);
			fprintf(stderr, "\tred   = %x:\n", info[i].red_mask);
			fprintf(stderr, "\tgreen = %x:\n", info[i].green_mask);
			fprintf(stderr, "\tblue  = %x:\n", info[i].blue_mask);
		}
#endif
		mXChar = mClientChar;
		if (ninfo) {
			mXChar.bytesPerPixel = bitsPerPixelToXBitmapPad(info->depth) >> 3;
			findMaskShiftAndSize(mXChar.redShift, mXChar.redSize, info->red_mask);
			findMaskShiftAndSize(mXChar.greenShift, mXChar.greenSize, info->green_mask);
			findMaskShiftAndSize(mXChar.blueShift, mXChar.blueSize, info->blue_mask);
		} else {
			printf("ARGH! Couldn't get XVisualInfo...\n");
			exit(1);
		}
		XFree(info);
#if 0
		fprintf(stderr, "X Server display characteristics:\n");
		dumpDisplayChar(mXChar);
		fprintf(stderr, "Client display characteristics:\n");
		dumpDisplayChar(mClientChar);
#endif

		XSetWindowAttributes attr;
		attr.save_under = 1;
		attr.backing_store = Always;
		XChangeWindowAttributes(gXDisplay, gXWindow, CWSaveUnder|CWBackingStore, &attr);
		
		mTitle = strdup(name);
		updateTitle();
		
		gDefaultColormap = DefaultColormap(gXDisplay, screen_num);
		
		XSelectInput(gXDisplay, gXWindow, 
			ExposureMask | KeyPressMask | KeyReleaseMask 
			| ButtonPressMask | ButtonReleaseMask | PointerMotionMask
			| EnterWindowMask | LeaveWindowMask);


		XGCValues values;
		gWhiteGC = XCreateGC(gXDisplay, gXWindow, 0, &values);
		XSetForeground(gXDisplay, gWhiteGC, WhitePixel(gXDisplay, screen_num));

		gBlackGC = XCreateGC(gXDisplay, gXWindow, 0, &values);
		XSetForeground(gXDisplay, gBlackGC, BlackPixel(gXDisplay, screen_num));

		XMapWindow(gXDisplay, gXWindow);

		XEvent event;
		XNextEvent(gXDisplay, &event);

		uint XDepth = mXChar.redSize + mXChar.greenSize + mXChar.blueSize;
		// Maybe client and (X-)server display characeristics match
		if (0 && memcmp(&mClientChar, &mXChar, sizeof (mClientChar)) == 0) {
//			fprintf(stderr, "client and server display characteristics match!!\n");
			Xframebuffer = NULL;

			gXImage = XCreateImage(gXDisplay, DefaultVisual(gXDisplay, screen_num),
				XDepth, ZPixmap, 0, (char*)framebuffer,
				mXChar.width, mXChar.height,
				mXChar.bytesPerPixel*8, 0);
		} else {
			// Otherwise we need a second framebuffer
//			fprintf(stderr, "client and server display characteristics DONT match :-(\n");
			Xframebuffer = (byte*)malloc(mXChar.width
				* mXChar.height * mXChar.bytesPerPixel);

			gXImage = XCreateImage(gXDisplay, DefaultVisual(gXDisplay, screen_num),
				XDepth, ZPixmap, 0, (char*)Xframebuffer,
				mXChar.width, mXChar.height,
				mXChar.bytesPerPixel*8, 0);
		}

		
		gMouseXImage = XCreateImage(gXDisplay, DefaultVisual(gXDisplay, screen_num),
			XDepth, ZPixmap, 0, (char*)mouseData,
			2, 2,
			mXChar.bytesPerPixel*8, 0);
			
		gGC = DefaultGC(gXDisplay, screen_num);
		
		switch (mClientChar.bytesPerPixel) {
			case 1:
				ht_printf("nyi: %s::%d", __FILE__, __LINE__);
				exit(-1);
				break;
			case 2:
				gPosixRGBMask.r_mask = 0x7c007c007c007c00ULL;
				gPosixRGBMask.g_mask = 0x03e003e003e003e0ULL;
				gPosixRGBMask.b_mask = 0x001f001f001f001fULL;
				break;
			case 4:
				gPosixRGBMask.r_mask = 0x00ff000000ff0000ULL;
				gPosixRGBMask.g_mask = 0x0000ff000000ff00ULL;
				gPosixRGBMask.b_mask = 0x000000ff000000ffULL;
				break;
		}
		menuData = NULL;
	}

	virtual ~X11SystemDisplay()
	{
		if (!gXDisplay) return;
		XCloseDisplay(gXDisplay);
		gXDisplay = NULL;
		free(mTitle);
		free(mouseData);
		free(framebuffer);
		if (menuData) free(menuData);
	}

	virtual	void finishMenu()
	{
		menuData = (byte*)malloc(mXChar.width *
			mMenuHeight * mXChar.bytesPerPixel);
		gMenuXImage = XCreateImage(gXDisplay, DefaultVisual(gXDisplay, DefaultScreen(gXDisplay)),
			mXChar.redSize+mXChar.greenSize+mXChar.blueSize, ZPixmap, 0, (char*)menuData,
			mXChar.width, mMenuHeight,
			mXChar.bytesPerPixel*8, 0);

		// Is this ugly? Yes!
		fillRGB(0, 0, mClientChar.width, mClientChar.height, MK_RGB(0xff, 0xff, 0xff));
		drawMenu();
		convertDisplayClientToServer();
		if (Xframebuffer) {
			memmove(menuData, Xframebuffer, mXChar.width * mMenuHeight
				* mXChar.bytesPerPixel);
		} else {
			memmove(menuData, framebuffer, mXChar.width * mMenuHeight
				* mXChar.bytesPerPixel);
		}
	}
	
	void updateTitle() 
	{
		ht_snprintf(mCurTitle, sizeof mCurTitle, "%s - [F12 %s mouse]", mTitle, mMouseEnabled ? "disables" : "enables");
		XTextProperty name_prop;
		char *mCurTitlep = mCurTitle;
		XStringListToTextProperty(&mCurTitlep, 1, &name_prop);
		XSetWMName(gXDisplay, gXWindow, &name_prop);
	}
	
	virtual	int toString(char *buf, int buflen) const
	{
		return snprintf(buf, buflen, "POSIX X11");
	}

	void clientMouseEnable(bool enable)
	{
		mMouseEnabled = enable;
		updateTitle();
		if (enable) {
			mResetMouseX = mCurMouseX;
			mResetMouseY = mCurMouseY;

			static Cursor cursor;
			static unsigned cursor_created = 0;

			static char shape_bits[16*16] = {0, };
			static char mask_bits[16*16] = {0, };

			if (!cursor_created) {
				Pixmap shape, mask;
				XColor white, black;
				shape = XCreatePixmapFromBitmapData(gXDisplay,
						RootWindow(gXDisplay, DefaultScreen(gXDisplay)),
						shape_bits, 16, 16, 1, 0, 1);
				mask =  XCreatePixmapFromBitmapData(gXDisplay,
						RootWindow(gXDisplay, DefaultScreen(gXDisplay)),
						mask_bits, 16, 16, 1, 0, 1);
				XParseColor(gXDisplay, gDefaultColormap, "black", &black);
				XParseColor(gXDisplay, gDefaultColormap, "white", &white);
				cursor = XCreatePixmapCursor(gXDisplay, shape, mask,
						&white, &black, 1, 1);
				cursor_created = 1;
			}

			XDefineCursor(gXDisplay, gXWindow, cursor);
			XWarpPointer(gXDisplay, gXWindow, gXWindow, 0, 0, 0, 0, mHomeMouseX, mHomeMouseY);
		} else {
			XWarpPointer(gXDisplay, gXWindow, gXWindow, 0, 0, 0, 0, mResetMouseX, mResetMouseY);
			XUndefineCursor(gXDisplay, gXWindow);
		}
//		mLastMouseX = mCurMouseX = mLastMouseY = mCurMouseY = -1;
	}
	
	virtual void getSyncEvent(DisplayEvent &ev)
	{
		if (!gXDisplay) return;
		XEvent event;
		XComposeStatus compose;
		KeySym keysym;
		char buffer[4];
		while (1) {
			sys_lock_mutex(mutex);
			XWindowEvent(gXDisplay, gXWindow, 
			KeyPressMask | KeyReleaseMask | ExposureMask | ButtonPressMask 
			| ButtonReleaseMask | PointerMotionMask
			| EnterWindowMask | LeaveWindowMask, &event);
			sys_unlock_mutex(mutex);
			switch (event.type) {
			case GraphicsExpose:
			case Expose: 
				displayShow();
				break;
			case KeyRelease: 
				ev.keyEvent.keycode = x11_key_to_adb_key[event.xkey.keycode];
				if (ev.keyEvent.keycode == KEY_F12) break;
				if ((ev.keyEvent.keycode & 0xff) == 0xff) break;
				ev.type = evKey;
				ev.keyEvent.pressed = false;
				sys_lock_mutex(mutex);
				XLookupString((XKeyEvent*)&event, buffer, sizeof buffer, &keysym, &compose);
				ev.keyEvent.chr = buffer[0];
				sys_unlock_mutex(mutex);
				return;
			case KeyPress:
				ev.keyEvent.keycode = x11_key_to_adb_key[event.xkey.keycode];
				if (ev.keyEvent.keycode == KEY_F12) {
					clientMouseEnable(!mMouseEnabled);
					break;
				}
				if ((ev.keyEvent.keycode & 0xff) == 0xff) break;
				ev.type = evKey;
				ev.keyEvent.pressed = true;
				ev.keyEvent.keycode = x11_key_to_adb_key[event.xkey.keycode];
				sys_lock_mutex(mutex);
				XLookupString((XKeyEvent*)&event, buffer, sizeof buffer, &keysym, &compose);
				ev.keyEvent.chr = buffer[0];
				sys_unlock_mutex(mutex);
				return;
			}
		}
	}
	
	virtual bool getEvent(DisplayEvent &ev)
	{
		if (!gXDisplay) return false;
		if (!mEventQueue->isEmpty()) {
			Pointer *p = (Pointer *)mEventQueue->deQueue();
			ev = *(DisplayEvent *)p->value;
			free(p->value);
			delete p;
			return true;
		}
		sys_lock_mutex(mutex);
		XEvent event;
		XComposeStatus compose;
		KeySym keysym;
		char buffer[4];
		while (XCheckWindowEvent(gXDisplay, gXWindow, 
			KeyPressMask | KeyReleaseMask | ExposureMask | ButtonPressMask 
			| ButtonReleaseMask | PointerMotionMask 
			| EnterWindowMask | LeaveWindowMask, &event)) {

			switch (event.type) {
			case Expose: 
				sys_unlock_mutex(mutex);
				displayShow();
				sys_lock_mutex(mutex);
				break;
			case KeyRelease: 
				ev.keyEvent.keycode = x11_key_to_adb_key[event.xkey.keycode];
				if (ev.keyEvent.keycode == KEY_F12) break;
				if ((ev.keyEvent.keycode & 0xff) == 0xff) break;
				ev.type = evKey;
				ev.keyEvent.pressed = false;
				XLookupString((XKeyEvent*)&event, buffer, sizeof buffer, &keysym, &compose);
				ev.keyEvent.chr = buffer[0];
				sys_unlock_mutex(mutex);
				return true;
			case KeyPress:
				ev.keyEvent.keycode = x11_key_to_adb_key[event.xkey.keycode];
				if (ev.keyEvent.keycode == KEY_F12 && mCurMouseX != -1) {
					clientMouseEnable(!mMouseEnabled);
					break;
				}
				if ((ev.keyEvent.keycode & 0xff) == 0xff) break;
				ev.type = evKey;
				ev.keyEvent.pressed = true;
				ev.keyEvent.keycode = x11_key_to_adb_key[event.xkey.keycode];
				XLookupString((XKeyEvent*)&event, buffer, sizeof buffer, &keysym, &compose);
				ev.keyEvent.chr = buffer[0];
				sys_unlock_mutex(mutex);
				return true;
			case ButtonPress:
				if (!mMouseEnabled) break;
				ev.type = evMouse;
				switch (((XButtonEvent *)&event)->button) {
				case Button1:
					mMouseButton[0] = true;
					break;
				case Button2:
					mMouseButton[1] = true;
					break;
				case Button3:
					mMouseButton[2] = true;
					break;
				}
				ev.mouseEvent.button1 = mMouseButton[0];
				ev.mouseEvent.button2 = mMouseButton[2];
				ev.mouseEvent.button3 = mMouseButton[1];
				ev.mouseEvent.x = mCurMouseX;
				ev.mouseEvent.y = mCurMouseY;
				ev.mouseEvent.relx = 0;
				ev.mouseEvent.rely = 0;
				sys_unlock_mutex(mutex);
				return true;
			case ButtonRelease: 
				if (!mMouseEnabled) {
					if (mCurMouseY < mMenuHeight) {
						sys_unlock_mutex(mutex);
						clickMenu(mCurMouseX, mCurMouseY);
						sys_lock_mutex(mutex);
					}
				} else {
					ev.type = evMouse;
					switch (((XButtonEvent *)&event)->button) {
					case Button1:
						mMouseButton[0] = false;
						break;
					case Button2:
						mMouseButton[1] = false;
						break;
					case Button3:
						mMouseButton[2] = false;
						break;
					}
					ev.mouseEvent.button1 = mMouseButton[0];
					ev.mouseEvent.button2 = mMouseButton[2];
					ev.mouseEvent.button3 = mMouseButton[1];
					ev.mouseEvent.x = mCurMouseX;
					ev.mouseEvent.y = mCurMouseY;
					ev.mouseEvent.relx = 0;
					ev.mouseEvent.rely = 0;
					sys_unlock_mutex(mutex);
					return true;
				}
				break;
			case MotionNotify:
				mCurMouseX = ev.mouseEvent.x = ((XPointerMovedEvent *)&event)->x;
				mCurMouseY = ev.mouseEvent.y = ((XPointerMovedEvent *)&event)->y;
				if (mCurMouseX == mHomeMouseX && mCurMouseY == mHomeMouseY) break;
				if (!mMouseEnabled) break;
				mLastMouseX = mCurMouseX;
				mLastMouseY = mCurMouseY;
				if (mLastMouseX == -1) break;
				ev.type = evMouse;
				ev.mouseEvent.button1 = mMouseButton[0];
				ev.mouseEvent.button2 = mMouseButton[1];
				ev.mouseEvent.button3 = mMouseButton[2];
				ev.mouseEvent.relx = mCurMouseX - mHomeMouseX;
				ev.mouseEvent.rely = mCurMouseY - mHomeMouseY;
				XWarpPointer(gXDisplay, gXWindow, gXWindow, 0, 0, 0, 0, mHomeMouseX, mHomeMouseY);
				sys_unlock_mutex(mutex);
				return true;
			case EnterNotify:
				mLastMouseX = mCurMouseX = ((XEnterWindowEvent *)&event)->x;
				mLastMouseY = mCurMouseY = ((XEnterWindowEvent *)&event)->y;
				break;
			case LeaveNotify:
				mLastMouseX = mCurMouseX = mLastMouseY = mCurMouseY = -1;
				break;
			}
		}
		sys_unlock_mutex(mutex);
		return false;
	}

	virtual void displayShow()
	{
		convertDisplayClientToServer();
		sys_lock_mutex(mutex);
		XPutImage(gXDisplay, gXWindow, gGC, gMenuXImage, 0, 0, 0, 0,
			gDisplay->mClientChar.width,
			mMenuHeight);
		XPutImage(gXDisplay, gXWindow, gGC, gXImage, 0, 0, 0, mMenuHeight,
			gDisplay->mClientChar.width,
			gDisplay->mClientChar.height);
		if (mHWCursorVisible) {
			XPutImage(gXDisplay, gXWindow, gGC, gMouseXImage, 0, 0, 
				mHWCursorX, mHWCursorY, 2, 2);
		}
		sys_unlock_mutex(mutex);
	}

	void convertDisplayClientToServer()
	{
		if (!Xframebuffer) return;	// great! nothing to do.
		byte *buf = framebuffer;
		byte *xbuf = Xframebuffer;
/*		if ((mClientChar.bytesPerPixel == 2) && (mXChar.bytesPerPixel == 2)) {
			posix_vaccel_15_to_15(mClientChar.height*mClientChar.width, buf, xbuf);
		} else if ((mClientChar.bytesPerPixel == 2) && (mXChar.bytesPerPixel == 4)) {
			posix_vaccel_15_to_32(mClientChar.height*mClientChar.width, buf, xbuf);
		} else */for (int y=0; y < mClientChar.height; y++) {
			for (int x=0; x < mClientChar.width; x++) {
				uint r, g, b;
				uint p;
				switch (mClientChar.bytesPerPixel) {
					case 1:
						p = palette[buf[0]];
						break;
					case 2:
						p = (buf[0] << 8) | buf[1];
						break;
					case 4:
						p = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
						break;
					default:
						ht_printf("internal error in %s:%d\n", __FILE__, __LINE__);
						exit(1);
						break;
				}
				r = (p >> mClientChar.redShift) & ((1<<mClientChar.redSize)-1);
				g = (p >> mClientChar.greenShift) & ((1<<mClientChar.greenSize)-1);
				b = (p >> mClientChar.blueShift) & ((1<<mClientChar.blueSize)-1);
				convertBaseColor(r, mClientChar.redSize, mXChar.redSize);
				convertBaseColor(g, mClientChar.greenSize, mXChar.greenSize);
				convertBaseColor(b, mClientChar.blueSize, mXChar.blueSize);
				p = (r << mXChar.redShift) | (g << mXChar.greenShift)
					| (b << mXChar.blueShift);
				switch (mXChar.bytesPerPixel) {
					case 1:
						xbuf[0] = p;
						break;
					case 2:
						xbuf[1] = p>>8; xbuf[0] = p;
						break;
					case 4:
						*(uint32*)xbuf = p;
						break;
				}
				xbuf += mXChar.bytesPerPixel;
				buf += mClientChar.bytesPerPixel;
			}
		}
	}

	virtual void queueEvent(DisplayEvent &ev)
	{
		DisplayEvent *e = (DisplayEvent *)malloc(sizeof (DisplayEvent));
		*e = ev;
		mEventQueue->enQueue(new Pointer(e));
	}

	static void *redrawThread(void *p)
	{
		int msec = *((int *)p);
		while (1) {
			timespec ts;
			ts.tv_sec = 0;
			ts.tv_nsec = msec*1000*1000;
			gDisplay->displayShow();
			nanosleep(&ts, NULL);
		}
		return NULL;
	}

	static void *eventLoop(void *p)
	{
	}
	
	virtual void startRedrawThread(int msec)
	{
		sys_create_thread(&redrawthread, 0, redrawThread, &msec);
	}

};

SystemDisplay *allocSystemDisplay(const char *title, const DisplayCharacteristics &chr)
{
	if (gDisplay) return gDisplay;
	return new X11SystemDisplay(title, chr);
}

