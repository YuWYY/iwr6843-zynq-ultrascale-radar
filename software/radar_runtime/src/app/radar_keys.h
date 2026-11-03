/* SPDX-License-Identifier: MIT */
#ifndef RADAR_KEYS_H
#define RADAR_KEYS_H
/* F20 KEY3 only. No GPIO reset, interrupt installation or acquisition action.
 * Button behavior is deliberately not tested in this delivery, per request. */
void radar_keys_init(void);
void radar_keys_poll(void);
#endif
