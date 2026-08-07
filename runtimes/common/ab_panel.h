/* The built-in error panel, shared by all four scripting runtimes.
 *
 * This is the surface a developer reads when their script has failed, and
 * it is often the ONLY signal they have -- so it is deliberately the most
 * legible thing the runtime draws: a real TrueType face at a size that
 * survives a downscaled screenshot, on an opaque backing so it never has to
 * compete with the game behind it.
 */
#ifndef AB_PANEL_H
#define AB_PANEL_H
/* `lang` names the runtime ("lua", "python", ...). `message` is the error;
 * it is wrapped to the panel width. `hint` is one line of what to do. */
void ab_error_panel(const char *lang, const char *message, const char *hint);
#endif
