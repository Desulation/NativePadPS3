/*
 * DS45PAD - passive raw-report cross-check. See btcheck.h.
 *
 * Everything is file-static state; the plugin polls from one thread.
 */
#include "btcheck.h"

#define BTCHECK_PORTS 8   /* PS3 has 7 pad ports; index by port & ignore the rest */

typedef struct {
	int      valid;
	uint16_t lx, ly, rx, ry, d1, d2;
} vsh_sample_t;

static struct {
	btcheck_state_t state;
	int      src_port;
	int      matches, mismatches;
	int      vsh_samples;
	vsh_sample_t prev_vsh[BTCHECK_PORTS];

	ds45_btraw_t hist[BTCHECK_RAW_HISTORY];
	int      hist_count, hist_head;     /* ring of connected raw samples */

	ds45_btraw_t last_raw;              /* previous raw sample, any connection */
	int      have_last_raw;
	int      connected;                 /* port_connected of the latest raw sample */
	int      was_connected;             /* 1 until the disconnect is first seen */
	int      disc_valid;                /* freeze watch armed */
	uint64_t disc_us;                   /* time of the first disconnected sample */
} g_bc;

/* ---- decode ------------------------------------------------------------ */

int bt_report_decode(uint64_t w0, uint64_t w1, ds45_btraw_t *out)
{
	/* Same layout and mapping as ds45_inject_bt_pad in passthru.c */
	uint8_t b0, b1, b2, hat;
	uint16_t d1 = 0, d2 = 0;

	if (!out || (uint8_t)(w0 >> 56) != 0xA1 || (uint8_t)(w0 >> 48) != 0x01)
		return -1;
	b0 = (uint8_t)(w0 >> 8);    /* D-pad hat + Square/Cross/Circle/Triangle */
	b1 = (uint8_t)w0;           /* L1/R1/L2/R2/Create/Options/L3/R3 */
	b2 = (uint8_t)(w1 >> 56);   /* PS/Touchpad/Mute */
	hat = b0 & 0x0F;
	if (hat > 8)
		return -1;

	if (b2 & 0x01) d1 |= DS45_CTRL_PS;

	if (hat == 0 || hat == 1 || hat == 7) d1 |= DS45_CTRL_UP;
	if (hat == 1 || hat == 2 || hat == 3) d1 |= DS45_CTRL_RIGHT;
	if (hat == 3 || hat == 4 || hat == 5) d1 |= DS45_CTRL_DOWN;
	if (hat == 5 || hat == 6 || hat == 7) d1 |= DS45_CTRL_LEFT;

	if (b0 & 0x10) d2 |= DS45_CTRL_SQUARE;
	if (b0 & 0x20) d2 |= DS45_CTRL_CROSS;
	if (b0 & 0x40) d2 |= DS45_CTRL_CIRCLE;
	if (b0 & 0x80) d2 |= DS45_CTRL_TRIANGLE;

	if (b1 & 0x01) d2 |= DS45_CTRL_L1;
	if (b1 & 0x02) d2 |= DS45_CTRL_R1;
	if (b1 & 0x04) d2 |= DS45_CTRL_L2;
	if (b1 & 0x08) d2 |= DS45_CTRL_R2;
	if (b1 & 0x10) d1 |= DS45_CTRL_SELECT;
	if (b1 & 0x20) d1 |= DS45_CTRL_START;
	if (b1 & 0x40) d1 |= DS45_CTRL_L3;
	if (b1 & 0x80) d1 |= DS45_CTRL_R3;

	out->lx  = (uint8_t)(w0 >> 40);
	out->ly  = (uint8_t)(w0 >> 32);
	out->rx  = (uint8_t)(w0 >> 24);
	out->ry  = (uint8_t)(w0 >> 16);
	out->l2  = (uint8_t)(w1 >> 48);
	out->r2  = (uint8_t)(w1 >> 40);
	out->hat = hat;
	out->d1  = d1;
	out->d2  = d2;
	out->ps  = (b2 & 0x01) ? 1 : 0;
	return 0;
}

/* ---- cross-check ------------------------------------------------------- */

static int stick_close(uint16_t vsh, uint8_t raw)
{
	int diff = (int)vsh - (int)raw;
	if (diff < 0) diff = -diff;
	return diff <= BTCHECK_STICK_TOLERANCE;
}

static int raw_equal(const ds45_btraw_t *a, const ds45_btraw_t *b)
{
	return a->lx == b->lx && a->ly == b->ly && a->rx == b->rx && a->ry == b->ry &&
	       a->l2 == b->l2 && a->r2 == b->r2 && a->hat == b->hat &&
	       a->d1 == b->d1 && a->d2 == b->d2 && a->ps == b->ps;
}

static int matches_raw(const vsh_sample_t *v, const ds45_btraw_t *r)
{
	return stick_close(v->lx, r->lx) && stick_close(v->ly, r->ly) &&
	       stick_close(v->rx, r->rx) && stick_close(v->ry, r->ry) &&
	       (uint16_t)(v->d1 & ~DS45_CTRL_PS) == (uint16_t)(r->d1 & ~DS45_CTRL_PS) &&
	       v->d2 == r->d2;
}

void btcheck_reset(int src_port)
{
	/* zero everything (state = PENDING = 0) */
	unsigned char *p = (unsigned char *)&g_bc;
	unsigned i;
	for (i = 0; i < sizeof(g_bc); i++)
		p[i] = 0;
	g_bc.src_port = src_port;
}

void btcheck_observe_vsh(uint32_t port, const CellPadData *d)
{
	vsh_sample_t cur, *prev;
	int i, matched = 0;

	if (!d || (int)port != g_bc.src_port || port >= BTCHECK_PORTS)
		return;
	if (d->len <= DS45_BTN_ANALOG_LEFT_Y)    /* no data this poll */
		return;

	cur.valid = 1;
	cur.lx = d->button[DS45_BTN_ANALOG_LEFT_X];
	cur.ly = d->button[DS45_BTN_ANALOG_LEFT_Y];
	cur.rx = d->button[DS45_BTN_ANALOG_RIGHT_X];
	cur.ry = d->button[DS45_BTN_ANALOG_RIGHT_Y];
	cur.d1 = d->button[DS45_BTN_DIGITAL1];
	cur.d2 = d->button[DS45_BTN_DIGITAL2];

	prev = &g_bc.prev_vsh[port];
	if (prev->valid && prev->lx == cur.lx && prev->ly == cur.ly &&
	    prev->rx == cur.rx && prev->ry == cur.ry &&
	    prev->d1 == cur.d1 && prev->d2 == cur.d2)
		return;                          /* same as last time: not a sample */
	*prev = cur;
	g_bc.vsh_samples++;                     /* counted in every state */

	/* nothing to compare against until the pad is up and a raw was seen */
	if (g_bc.state != BTCHECK_PENDING || !g_bc.connected || g_bc.hist_count == 0)
		return;

	for (i = 0; i < g_bc.hist_count; i++) {
		if (matches_raw(&cur, &g_bc.hist[i])) {
			matched = 1;
			break;
		}
	}
	if (matched) {
		if (++g_bc.matches >= BTCHECK_MATCHES_REQUIRED)
			g_bc.state = BTCHECK_PASSED;
	} else if (++g_bc.mismatches > BTCHECK_MISMATCH_LIMIT) {
		g_bc.state = BTCHECK_FAILED;
	}
}

btcheck_state_t btcheck_feed_raw(const ds45_btraw_t *r, int port_connected, uint64_t now_us)
{
	if (!r || g_bc.state == BTCHECK_FAILED)
		return g_bc.state;

	if (port_connected) {
		g_bc.connected = 1;
		g_bc.was_connected = 1;
		g_bc.disc_valid = 0;                /* back: the freeze watch is over */
		/* keep the last N DISTINCT raw states: a fast feed of an
		 * unchanged report must not push an older state out */
		if (g_bc.hist_count == 0 ||
		    !raw_equal(r, &g_bc.hist[(g_bc.hist_head + BTCHECK_RAW_HISTORY - 1) %
					     BTCHECK_RAW_HISTORY])) {
			g_bc.hist[g_bc.hist_head] = *r;
			g_bc.hist_head = (g_bc.hist_head + 1) % BTCHECK_RAW_HISTORY;
			if (g_bc.hist_count < BTCHECK_RAW_HISTORY)
				g_bc.hist_count++;
		}
	} else {
		g_bc.connected = 0;
		if (g_bc.was_connected) {           /* first disconnected sample */
			g_bc.was_connected = 0;
			g_bc.disc_valid = 1;
			g_bc.disc_us = now_us;
			if (g_bc.state == BTCHECK_PENDING) {
				/* not a failure: start the evidence over; the stale
				 * raw history goes too, the freeze watch stays */
				g_bc.matches = g_bc.mismatches = 0;
				g_bc.hist_count = g_bc.hist_head = 0;
			}
		} else if (g_bc.disc_valid && g_bc.have_last_raw && !raw_equal(r, &g_bc.last_raw) &&
			   now_us > g_bc.disc_us && now_us - g_bc.disc_us > BTCHECK_FREEZE_US) {
			/* a live report cannot keep changing with the pad gone */
			g_bc.state = BTCHECK_FAILED;
		}
	}
	g_bc.last_raw = *r;
	g_bc.have_last_raw = 1;
	return g_bc.state;
}

btcheck_state_t btcheck_state(void)
{
	return g_bc.state;
}

int btcheck_vsh_samples_seen(void)
{
	return g_bc.vsh_samples;
}
