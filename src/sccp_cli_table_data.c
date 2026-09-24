/* Licensed under the GNU General Public License; see LICENSE. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sccp_cli_table_data.h"
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Use a thread-local UTF-8 locale without changing Asterisk's global locale.
 * Invalid bytes count as one column; combining/wide characters use wcwidth.
 */
static size_t display_width(const char *text)
{
	size_t width = 0, left = strlen(text);
	mbstate_t state = {0};
	while (left) {
		wchar_t ch;
		size_t used = mbrtowc(&ch, text, left, &state);
		if (used == (size_t)-1 || used == (size_t)-2) {
			memset(&state, 0, sizeof(state));
			used = 1;
			width++;
		} else {
			int columns = wcwidth(ch);
			width += columns < 0 ? 1 : (size_t)columns;
		}
		text += used;
		left -= used;
	}
	return width;
}

void sccp_cli_table_add(sccp_cli_table_data_t *table, const char *format, ...)
{
	if (table->failed) {
		return;
	}
	if (table->cells == table->capacity) {
		size_t capacity = table->capacity ? table->capacity * 2 : 64;
		if (capacity < table->capacity || capacity > SIZE_MAX / sizeof(*table->data)) {
			table->failed = 1;
			return;
		}
		void *data = realloc(table->data, capacity * sizeof(*table->data));
		if (!data) {
			table->failed = 1;
			return;
		}
		table->data = data;
		table->capacity = capacity;
	}
	char *text = NULL;
	va_list args;
	va_start(args, format);
	int length = vasprintf(&text, format, args);
	va_end(args);
	if (length < 0) {
		table->failed = 1;
		return;
	}
	/* Keep cells on one terminal line, including untrusted labels/variables. */
	for (char *p = text; *p; ++p) {
		if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {
			*p = ' ';
		}
	}
	table->data[table->cells++].text = text;
}

static void repeat(void (*write_text)(void *, const char *), void *context,
	char ch, size_t count)
{
	char buffer[65];
	memset(buffer, ch, sizeof(buffer) - 1);
	while (count) {
		size_t n = count < sizeof(buffer) - 1 ? count : sizeof(buffer) - 1;
		buffer[n] = '\0';
		write_text(context, buffer);
		count -= n;
	}
}

void sccp_cli_table_render(sccp_cli_table_data_t *table, const char *title,
	void (*write_text)(void *, const char *), void *context)
{
	if (table->failed || !table->columns || table->cells % table->columns) {
		write_text(context, "Unable to display table: incomplete snapshot or out of memory.\n");
		return;
	}
	size_t *widths = calloc(table->columns, sizeof(*widths));
	if (!widths) {
		write_text(context, "Unable to display table: out of memory.\n");
		return;
	}
	locale_t locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
	if (!locale) {
		locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
	}
	locale_t previous = locale ? uselocale(locale) : (locale_t)0;
	for (size_t c = 0; c < table->columns; c++) {
		widths[c] = display_width(table->headers[c]);
	}
	for (size_t i = 0; i < table->cells; i++) {
		table->data[i].width = display_width(table->data[i].text);
		if (table->data[i].width > widths[i % table->columns]) {
			widths[i % table->columns] = table->data[i].width;
		}
	}
	/* Asterisk style: title, then header and rows indented under it; no underline row */
	write_text(context, title);
	write_text(context, ":\n");
	if (!table->cells) {
		write_text(context, "  (none)\n");
	} else {
		write_text(context, "  ");
		for (size_t c = 0; c < table->columns; c++) {
			write_text(context, table->headers[c]);
			if (c + 1 < table->columns) {
				repeat(write_text, context, ' ', widths[c] - display_width(table->headers[c]) + 2);
			}
		}
		write_text(context, "\n");
	}
	for (size_t i = 0; i < table->cells; i++) {
		size_t c = i % table->columns;
		if (c == 0) {
			write_text(context, "  ");
		}
		write_text(context, table->data[i].text);
		if (c + 1 < table->columns) {
			repeat(write_text, context, ' ', widths[c] - table->data[i].width + 2);
		} else {
			write_text(context, "\n");
		}
	}
	write_text(context, "\n");
	if (locale) {
		uselocale(previous);
		freelocale(locale);
	}
	free(widths);
}

void sccp_cli_table_destroy(sccp_cli_table_data_t *table)
{
	for (size_t i = 0; i < table->cells; i++) {
		free(table->data[i].text);
	}
	free(table->data);
	memset(table, 0, sizeof(*table));
}
