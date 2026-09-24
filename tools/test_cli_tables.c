/* Standalone renderer regression test; no Asterisk instance required.
 * cc -D_GNU_SOURCE -Isrc -Wall -Wextra -Werror -fsanitize=address,undefined \
 *    tools/test_cli_tables.c src/sccp_cli_table_data.c -o /tmp/test_cli_tables
 */
#include "sccp_cli_table_data.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_text(void *context, const char *text)
{
	fputs(text, context);
}

static char *render(sccp_cli_table_data_t *table)
{
	char *output = NULL;
	size_t length = 0;
	FILE *stream = open_memstream(&output, &length);
	assert(stream);
	sccp_cli_table_render(table, "Test", write_text, stream);
	fclose(stream);
	sccp_cli_table_destroy(table);
	return output;
}

int main(void)
{
	const char *const headers[] = { "Name", "Count" };
	sccp_cli_table_data_t table = { .headers = headers, .columns = 2 };
	sccp_cli_table_add(&table, "%s", "A very long device description");
	sccp_cli_table_add(&table, "%d", 123456789);
	sccp_cli_table_add(&table, "%s", "short");
	sccp_cli_table_add(&table, "%d", 1);
	char *text = render(&table);
	assert(!strcmp(text,
		"Test:\n"
		"  Name                            Count\n"
		"  A very long device description  123456789\n"
		"  short                           1\n\n"));
	free(text);

	table = (sccp_cli_table_data_t){ .headers = headers, .columns = 2 };
	sccp_cli_table_add(&table, "%s", "電話");
	sccp_cli_table_add(&table, "%d", 1);
	sccp_cli_table_add(&table, "%s", "e\xcc\x81");
	sccp_cli_table_add(&table, "%d", 2);
	sccp_cli_table_add(&table, "%s", "bad\xff");
	sccp_cli_table_add(&table, "%d", 3);
	text = render(&table);
	assert(strstr(text, "電話  1\n"));
	assert(strstr(text, "e\xcc\x81     2\n"));
	assert(strstr(text, "bad\xff  3\n"));
	free(text);

	table = (sccp_cli_table_data_t){ .headers = headers, .columns = 2 };
	sccp_cli_table_add(&table, "%s", "a\nb\tc\033d");
	sccp_cli_table_add(&table, "%s", "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535");
	text = render(&table);
	assert(strstr(text, "a b c d  [ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535\n"));
	free(text);

	table = (sccp_cli_table_data_t){ .headers = headers, .columns = 2 };
	text = render(&table);
	assert(!strcmp(text, "Test:\n  (none)\n\n"));
	free(text);

	table = (sccp_cli_table_data_t){ .headers = headers, .columns = 2 };
	sccp_cli_table_add(&table, "%s", "incomplete");
	text = render(&table);
	assert(strstr(text, "Unable to display table:"));
	free(text);

	table = (sccp_cli_table_data_t){ .headers = headers, .columns = 2, .failed = 1 };
	sccp_cli_table_add(&table, "%s", "ignored after allocation failure");
	text = render(&table);
	assert(strstr(text, "Unable to display table:"));
	free(text);

	/* Exercise reallocation and separators wider than the output chunk. */
	table = (sccp_cli_table_data_t){ .headers = headers, .columns = 2 };
	char long_value[257];
	memset(long_value, 'x', sizeof(long_value) - 1);
	long_value[sizeof(long_value) - 1] = '\0';
	for (int row = 0; row < 1000; row++) {
		sccp_cli_table_add(&table, "%s", long_value);
		sccp_cli_table_add(&table, "%d", row);
	}
	text = render(&table);
	assert(strstr(text, long_value));
	assert(strstr(text, "  999\n"));
	free(text);
	puts("CLI table tests passed");
	return 0;
}
