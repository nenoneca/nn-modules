/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/**
 * button — On/off button input module.
 *
 * Reads a single GPIO pin via the `sw0` devicetree alias.  Two
 * behaviours, picked at compile time via the
 * NODE_MGR_BUTTON_{FLIP,TOGGLE} Kconfig choice:
 *
 *   FLIP   — value mirrors the physical button: 1 while pressed,
 *            0 when released.  Useful for "while held" rules.
 *   TOGGLE — value flips 0 <-> 1 on each press (rising edge of
 *            held state).  Useful as a latched switch.
 *
 * Both variants:
 *   - register the field with auto_engine as SENSOR (range 0..1)
 *     so it appears in /info capabilities,
 *   - call auto_engine_set_field(name, v) on each debounced
 *     transition (CONFIG_NODE_MGR_BUTTON_DEBOUNCE_MS),
 *   - therefore feed the rule engine and become readable through
 *     the Phase 6 field-relay path
 *     (GET /api/v1/devices/<dev>/field/<name>).
 *
 * Initialise from app main() with the field name to expose:
 *     button_flip_init("button");
 *   or
 *     button_toggle_init("button");
 *
 * Linker note: only one of button_flip.c / button_toggle.c is
 * compiled in (per the Kconfig choice).  Calling the other init
 * is a link-time error, which is the intended safety net.
 */

int button_flip_init(const char *field_name);
int button_toggle_init(const char *field_name);

/** Last debounced state.  0 = released/off, 1 = pressed/on. */
int button_get_state(void);
