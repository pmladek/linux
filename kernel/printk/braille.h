/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PRINTK_BRAILLE_H
#define _PRINTK_BRAILLE_H

#ifdef CONFIG_A11Y_BRAILLE_CONSOLE

static inline void
braille_update_options(struct preferred_console *pc, char *brl_options)
{
	if (brl_options)
		pc->brl_options = brl_options;
}

static inline bool
is_braille_console_preferred(struct preferred_console *pc)
{
	return (!!pc->brl_options);
}

/*
 * Setup console according to braille options.
 * Return -EINVAL on syntax error, 0 on success (or no braille option was
 * actually given).
 * Modifies str to point to the serial options
 * Sets brl_options to the parsed braille options.
 */
int
_braille_console_setup(char **str, char **brl_options);

int
_braille_register_console(struct console *console, struct preferred_console *pc);

int
_braille_unregister_console(struct console *console);

#else

static inline void
braille_update_options(struct preferred_console *pc, char *brl_options)
{
}

static inline bool
is_braille_console_preferred(struct preferred_console *pc)
{
	return false;
}

static inline int
_braille_console_setup(char **str, char **brl_options)
{
	return 0;
}

static inline int
_braille_register_console(struct console *console, struct preferred_console *pc)
{
	return 0;
}

static inline int
_braille_unregister_console(struct console *console)
{
	return 0;
}

#endif

#endif
