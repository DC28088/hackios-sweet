/* Sweet hardware input -> HakiOS's BackBoard input queue.
 * Hardware producers originate in DC28088/ios-sweet fe987bf05e0d.
 * Coordinates from the physical 1080x2400 touch panel are normalized by the
 * queue ABI; BackBoard maps them to its current logical display size.
 */
#include <sys/hakios.input.h>
#include <sys/systm.h>

void sweet_keys_init(void);
void sweet_touch_init(void);
void hakios_sweet_input_init(void);
void sweet_hid_key(int key, int down);
void sweet_hid_touch(int finger, int down, int x, int y);

void
sweet_hid_key(int key, int down)
{
	static const uint32_t buttons[] = {
		HAKIOS_BUTTON_LOCK, HAKIOS_BUTTON_VOLUME_UP,
		HAKIOS_BUTTON_VOLUME_DOWN, HAKIOS_BUTTON_HOME
	};
	if (key >= 0 && (unsigned)key < sizeof(buttons) / sizeof(buttons[0])) {
		hakios_touch_post(down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP,
		    buttons[key], 0, 0);
	}
}

void
sweet_hid_touch(int finger, int down, int x, int y)
{
	/* The hardware driver calls this serially from its polling thread. */
	static uint32_t active;
	if (finger < 0 || finger >= 10 || x < 0 || x > 1079 || y < 0 || y > 2399) {
		return;
	}
	uint32_t bit = 1u << finger;
	uint32_t type = down ? ((active & bit) ? HAKIOS_TOUCH_MOVE : HAKIOS_TOUCH_DOWN) : HAKIOS_TOUCH_UP;
	if (!down && !(active & bit)) {
		return;
	}
	if (down) {
		active |= bit;
	} else {
		active &= ~bit;
	}
	hakios_touch_post(type, (uint32_t)finger,
	    (uint32_t)x * 65535u / 1079u, (uint32_t)y * 65535u / 2399u);
}

void
hakios_sweet_input_init(void)
{
	sweet_keys_init();
	sweet_touch_init();
}
