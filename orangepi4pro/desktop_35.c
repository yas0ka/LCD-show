/* Orange Pi 4 Pro のローカル画面（X11 :0）を 3.5インチ SPI 液晶へ送り、
 * タッチをその画面のクリックとして渡す。
 *
 * 命令は tft35a と同じ 16bit（上位バイト 0）。画素は RGB565。
 * 表示は /dev/spidev3.0。リセットは wPi 13、命令とデータは wPi 10。
 * タッチは /dev/spidev3.1（XPT2046）。割り込みのピン11は使わない。
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#define SPI_LCD "/dev/spidev3.0"
#define SPI_TOUCH "/dev/spidev3.1"
#define LCD_HZ 12000000u
#define TOUCH_HZ 1000000u
#define DC_WPI "10"
#define RST_WPI "13"
#define VIEW_W 480
#define VIEW_H 320

/* イメージの較正を 180 度回した値。左上の指が右下、左下の指が右上に出た。
 * 画面の 0x36 は 0x28。 */
#define TOUCH_SWAP 1
#define RAW_X_LEFT 3936
#define RAW_X_RIGHT 227
#define RAW_Y_TOP 268
#define RAW_Y_BOTTOM 3880
#define TOUCH_Z_MIN 80

static volatile sig_atomic_t g_stop;
static int g_down;
static int g_cur_x = -1;
static int g_cur_y = -1;

static int iow(int nr, int size)
{
	return (1 << 30) | (size << 16) | ((int)'k' << 8) | nr;
}

static int on_xerr(Display *dpy, XErrorEvent *ev)
{
	(void)dpy;
	fprintf(stderr, "X error code %u\n", ev->error_code);
	return 0;
}

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void gpio3(const char *a, const char *b, const char *c)
{
	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		execlp("gpio", "gpio", a, b, c, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
}

static int spi_open(const char *path, uint32_t hz)
{
	int fd = open(path, O_RDWR);
	uint8_t mode = 0;
	uint8_t bits = 8;
	uint32_t speed = hz;
	if (fd < 0)
		return -1;
	if (ioctl(fd, iow(1, 1), &mode) < 0 ||
	    ioctl(fd, iow(3, 1), &bits) < 0 ||
	    ioctl(fd, iow(4, 4), &speed) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int spi_xfer(int fd, uint32_t hz, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	uint8_t scratch[4096];
	struct {
		uint64_t tx_buf;
		uint64_t rx_buf;
		uint32_t len;
		uint32_t speed_hz;
		uint16_t delay_usecs;
		uint8_t bits_per_word;
		uint8_t cs_change;
		uint8_t tx_nbits;
		uint8_t rx_nbits;
		uint16_t pad;
	} tr;
	if (len > sizeof scratch)
		return -1;
	if (!rx)
		rx = scratch;
	memset(&tr, 0, sizeof tr);
	tr.tx_buf = (uint64_t)(uintptr_t)tx;
	tr.rx_buf = (uint64_t)(uintptr_t)rx;
	tr.len = len;
	tr.speed_hz = hz;
	tr.bits_per_word = 8;
	if (ioctl(fd, iow(0, 32), &tr) < 0)
		return -1;
	return 0;
}

static int spi_write(int fd, uint32_t hz, const uint8_t *tx, uint32_t len)
{
	uint8_t rx[4096];
	uint32_t off = 0;
	while (off < len) {
		uint32_t n = len - off;
		if (n > 960)
			n = 960;
		if (spi_xfer(fd, hz, tx + off, rx, n) < 0)
			return -1;
		off += n;
	}
	return 0;
}

/* fbtft_write_reg16_bus8。命令も各パラメータも 16bit ビッグエンディアン。 */
static void wr16(int fd, uint8_t cmd, const uint8_t *data, uint32_t n)
{
	uint8_t cb[2] = {0x00, cmd};
	uint8_t db[80];
	uint32_t i;
	gpio3("write", DC_WPI, "0");
	spi_xfer(fd, LCD_HZ, cb, NULL, 2);
	if (!n)
		return;
	for (i = 0; i < n; i++) {
		db[i * 2] = 0x00;
		db[i * 2 + 1] = data[i];
	}
	gpio3("write", DC_WPI, "1");
	spi_write(fd, LCD_HZ, db, n * 2);
}

static void lcd_init(int fd)
{
	uint8_t f1[] = {0x36, 0x04, 0x00, 0x3C, 0x0F, 0x8F};
	uint8_t f2[] = {0x18, 0xA3, 0x12, 0x02, 0xB2, 0x12, 0xFF, 0x10, 0x00};
	uint8_t f8[] = {0x21, 0x04};
	uint8_t f9[] = {0x00, 0x08};
	uint8_t m08[] = {0x08};
	uint8_t b4[] = {0x00};
	uint8_t c1[] = {0x41};
	uint8_t c5[] = {0x00, 0x91, 0x80, 0x00};
	uint8_t e0[] = {0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
			0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00};
	uint8_t e1[] = {0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
			0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00};
	uint8_t p55[] = {0x55};
	uint8_t m28[] = {0x28};

	gpio3("mode", DC_WPI, "out");
	gpio3("mode", RST_WPI, "out");
	gpio3("write", RST_WPI, "0");
	usleep(20 * 1000);
	gpio3("write", RST_WPI, "1");
	usleep(150 * 1000);

	wr16(fd, 0xF1, f1, 6);
	wr16(fd, 0xF2, f2, 9);
	wr16(fd, 0xF8, f8, 2);
	wr16(fd, 0xF9, f9, 2);
	wr16(fd, 0x36, m08, 1);
	wr16(fd, 0xB4, b4, 1);
	wr16(fd, 0xC1, c1, 1);
	wr16(fd, 0xC5, c5, 4);
	wr16(fd, 0xE0, e0, 15);
	wr16(fd, 0xE1, e1, 15);
	wr16(fd, 0x3A, p55, 1);
	wr16(fd, 0x11, NULL, 0);
	wr16(fd, 0x36, m28, 1);
	usleep(255 * 1000);
	wr16(fd, 0x29, NULL, 0);
	/* rotate=90 は 0x20。この個体は赤と青が入れ替わるので、bgr の 0x08 を足して 0x28。 */
	wr16(fd, 0x36, m28, 1);
}

static void lcd_window(int fd)
{
	uint8_t col[] = {0x00, 0x00, 0x01, 0xDF};
	uint8_t page[] = {0x00, 0x00, 0x01, 0x3F};
	wr16(fd, 0x2A, col, 4);
	wr16(fd, 0x2B, page, 4);
	wr16(fd, 0x2C, NULL, 0);
	gpio3("write", DC_WPI, "1");
}

static int chan8(uint32_t p, uint32_t mask)
{
	int shift = mask ? __builtin_ctz(mask) : 0;
	uint32_t vmax = mask >> shift;
	uint32_t v = mask ? (p & mask) >> shift : 0;
	if (vmax == 0)
		return 0;
	if (vmax == 255)
		return (int)v;
	return (int)(v * 255 / vmax);
}

static void put565(uint8_t *frame, int x, int y, int r, int g, int b)
{
	size_t off;
	uint16_t c;
	if (x < 0 || y < 0 || x >= VIEW_W || y >= VIEW_H)
		return;
	c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
	off = ((size_t)y * VIEW_W + (size_t)x) * 2;
	frame[off] = (uint8_t)(c >> 8);
	frame[off + 1] = (uint8_t)c;
}

static void draw_cursor(uint8_t *frame, int x, int y)
{
	int i;
	for (i = -4; i <= 4; i++) {
		put565(frame, x + i, y, 255, 255, 255);
		put565(frame, x, y + i, 255, 255, 255);
		put565(frame, x + i, y + 1, 0, 0, 0);
		put565(frame, x + 1, y + i, 0, 0, 0);
	}
}

static void build_frame(uint8_t *frame, XImage *im)
{
	int x, y;
	int bpp = im->bits_per_pixel / 8;
	if (bpp < 3)
		bpp = 4;
	for (y = 0; y < VIEW_H; y++) {
		int sy = y * im->height / VIEW_H;
		if (sy >= im->height)
			sy = im->height - 1;
		for (x = 0; x < VIEW_W; x++) {
			int sx = x * im->width / VIEW_W;
			uint32_t p;
			if (sx >= im->width)
				sx = im->width - 1;
			p = *(uint32_t *)(im->data + sy * im->bytes_per_line + sx * bpp);
			put565(frame, x, y, chan8(p, im->red_mask), chan8(p, im->green_mask),
			       chan8(p, im->blue_mask));
		}
	}
	if (g_cur_x >= 0)
		draw_cursor(frame, g_cur_x * VIEW_W / (im->width ? im->width : 1),
			    g_cur_y * VIEW_H / (im->height ? im->height : 1));
}

static int xpt_read(int fd, uint8_t cmd)
{
	uint8_t tx[3] = {cmd, 0, 0};
	uint8_t rx[3];
	int v;
	if (spi_xfer(fd, TOUCH_HZ, tx, rx, 3) < 0)
		return -1;
	v = ((rx[1] << 8) | rx[2]) >> 3;
	return v & 0x0FFF;
}

static int map_axis(int raw, int r0, int r1, int out1)
{
	long v;
	if (r0 == r1)
		return 0;
	v = (long)(raw - r0) * out1 / (r1 - r0);
	if (v < 0)
		v = 0;
	if (v > out1)
		v = out1;
	return (int)v;
}

static void touch_loop(Display *dpy, int fd)
{
	int stable = 0;
	fprintf(stderr, "touch idle");
	for (int i = 0; i < 3; i++) {
		int z = xpt_read(fd, 0xB0);
		int x = xpt_read(fd, 0xD0);
		int y = xpt_read(fd, 0x90);
		fprintf(stderr, " z=%d x=%d y=%d", z, x, y);
	}
	fprintf(stderr, "\n");

	while (!g_stop) {
		int z, x, y, ax, ay, sx, sy;
		struct timespec ts = {0, 20 * 1000 * 1000};
		z = xpt_read(fd, 0xB0);
		x = xpt_read(fd, 0xD0);
		y = xpt_read(fd, 0x90);
		if (z < TOUCH_Z_MIN || x < 0 || y < 0) {
			stable = 0;
			if (g_down) {
				XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
				XFlush(dpy);
				g_down = 0;
			}
			nanosleep(&ts, NULL);
			continue;
		}
		ax = TOUCH_SWAP ? y : x;
		ay = TOUCH_SWAP ? x : y;
		sx = map_axis(ax, RAW_X_LEFT, RAW_X_RIGHT, DisplayWidth(dpy, 0) - 1);
		sy = map_axis(ay, RAW_Y_TOP, RAW_Y_BOTTOM, DisplayHeight(dpy, 0) - 1);
		g_cur_x = sx;
		g_cur_y = sy;
		XTestFakeMotionEvent(dpy, -1, sx, sy, CurrentTime);
		if (!g_down) {
			if (++stable < 2) {
				nanosleep(&ts, NULL);
				continue;
			}
			XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
			g_down = 1;
			fprintf(stderr, "touch raw x=%d y=%d z=%d -> %d,%d\n", x, y, z, sx, sy);
		}
		XFlush(dpy);
		nanosleep(&ts, NULL);
	}
	if (g_down)
		XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
	XFlush(dpy);
}

static void *touch_thread(void *arg)
{
	int *touch_fd = arg;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "touch display open failed\n");
		return NULL;
	}
	touch_loop(dpy, *touch_fd);
	XCloseDisplay(dpy);
	return NULL;
}

int main(void)
{
	Display *dpy = NULL;
	Window root;
	int lcd, touch;
	pthread_t tid;
	uint8_t *frame;

	setenv("DISPLAY", ":0", 0);
	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	XInitThreads();
	XSetErrorHandler(on_xerr);

	for (int i = 0; i < 30 && !dpy; i++) {
		dpy = XOpenDisplay(NULL);
		if (!dpy)
			sleep(1);
	}
	if (!dpy) {
		fprintf(stderr, "XOpenDisplay failed\n");
		return 1;
	}
	if (system("xrandr --fb 480x320 >/dev/null 2>&1") != 0)
		fprintf(stderr, "xrandr resize failed\n");
	if (system("xset s off -dpms s noblank >/dev/null 2>&1") != 0)
		fprintf(stderr, "xset failed\n");
	XCloseDisplay(dpy);
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "XOpenDisplay failed after resize\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);
	lcd = spi_open(SPI_LCD, LCD_HZ);
	touch = spi_open(SPI_TOUCH, TOUCH_HZ);
	if (lcd < 0 || touch < 0) {
		perror("spidev");
		return 1;
	}
	frame = calloc((size_t)VIEW_W * VIEW_H * 2, 1);
	if (!frame)
		return 1;

	fprintf(stderr, "init lcd\n");
	lcd_init(lcd);
	fprintf(stderr, "lcd ready, screen %dx%d\n", DisplayWidth(dpy, 0), DisplayHeight(dpy, 0));

	if (pthread_create(&tid, NULL, touch_thread, &touch) != 0)
		return 1;

	while (!g_stop) {
		XImage *im = XGetImage(dpy, root, 0, 0,
				       (unsigned)DisplayWidth(dpy, 0),
				       (unsigned)DisplayHeight(dpy, 0),
				       AllPlanes, ZPixmap);
		if (!im) {
			usleep(200 * 1000);
			continue;
		}
		build_frame(frame, im);
		XDestroyImage(im);
		lcd_window(lcd);
		if (spi_write(lcd, LCD_HZ, frame, (uint32_t)VIEW_W * VIEW_H * 2) < 0) {
			perror("spi write");
			break;
		}
	}
	gpio3("write", DC_WPI, "0");
	free(frame);
	return 0;
}
