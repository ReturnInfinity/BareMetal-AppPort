#include <stdio.h>
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

/* Minimal embedding, adapted from lexbor's own
   examples/lexbor/selectors/easy_way.c: parse a small in-memory HTML
   string, parse one CSS selector, and print every DOM node it matches
   via this port's normal stdout path (no fetch/network yet -- that's a
   later phase, see LEXBOR.md). */

static lxb_status_t
serialize_cb(const lxb_char_t *data, size_t len, void *ctx)
{
	printf("%.*s", (int) len, (const char *) data);
	return LXB_STATUS_OK;
}

static lxb_status_t
find_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
	unsigned *count = ctx;
	(*count)++;

	printf("Lexbor match %u: ", *count);
	(void) lxb_html_serialize_cb(node, serialize_cb, NULL);
	printf("\n");

	return LXB_STATUS_OK;
}

int main(void)
{
	unsigned count = 0;
	lxb_status_t status;
	lxb_dom_node_t *root;
	lxb_selectors_t *selectors;
	lxb_html_document_t *document;
	lxb_css_parser_t *parser;
	lxb_css_selector_list_t *list;

	static const lxb_char_t html[] =
		"<div><p class=\"greeting\">Hello, BareMetal!</p><p>skip me</p></div>";
	static const lxb_char_t query[] = "p.greeting";

	document = lxb_html_document_create();
	status = lxb_html_document_parse(document, html, sizeof(html) - 1);
	if (status != LXB_STATUS_OK) {
		printf("lxb_html_document_parse failed: %d\n", status);
		return 1;
	}

	parser = lxb_css_parser_create();
	status = lxb_css_parser_init(parser, NULL);
	if (status != LXB_STATUS_OK) {
		printf("lxb_css_parser_init failed: %d\n", status);
		return 1;
	}

	selectors = lxb_selectors_create();
	status = lxb_selectors_init(selectors);
	if (status != LXB_STATUS_OK) {
		printf("lxb_selectors_init failed: %d\n", status);
		return 1;
	}

	list = lxb_css_selectors_parse(parser, query, sizeof(query) - 1);
	if (parser->status != LXB_STATUS_OK) {
		printf("lxb_css_selectors_parse failed: %d\n", parser->status);
		return 1;
	}

	root = lxb_dom_interface_node(document);

	status = lxb_selectors_find(selectors, root, list, find_cb, &count);
	if (status != LXB_STATUS_OK) {
		printf("lxb_selectors_find failed: %d\n", status);
		return 1;
	}

	printf("Lexbor says: %u match(es)\n", count);

	lxb_selectors_destroy(selectors, true);
	lxb_css_parser_destroy(parser, true);
	lxb_css_selector_list_destroy_memory(list);
	lxb_html_document_destroy(document);

	return 0;
}
