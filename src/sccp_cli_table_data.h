/* Licensed under the GNU General Public License, version 2 or later; see LICENSE. */
#pragma once
#include <stddef.h>

typedef struct sccp_cli_table_cell {
	char *text;
	size_t width;
} sccp_cli_table_cell_t;

typedef struct sccp_cli_table_data {
	const char *const *headers;
	size_t columns, cells, capacity;
	sccp_cli_table_cell_t *data;
	int failed;
} sccp_cli_table_data_t;

void sccp_cli_table_add(sccp_cli_table_data_t *table, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
void sccp_cli_table_render(sccp_cli_table_data_t *table, const char *title,
	void (*write_text)(void *, const char *), void *context);
void sccp_cli_table_destroy(sccp_cli_table_data_t *table);
