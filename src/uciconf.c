/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include <uci.h>

#include "config.h"

struct section_ctx {
	struct uci_context *uci;
	struct uci_section *s;
};

static const char *section_get(void *ctx, const char *name, bool *is_list)
{
	struct section_ctx *sc = ctx;
	struct uci_option *o = uci_lookup_option(sc->uci, sc->s, name);

	*is_list = false;
	if (!o)
		return NULL;
	if (o->type != UCI_TYPE_STRING) {
		*is_list = true;
		return NULL;
	}
	return o->v.string;
}

int fg_uci_load(const char *package, struct fg_config *cfg, char *err, size_t errlen)
{
	struct uci_context *uci = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_section *guest = NULL;
	struct uci_element *e;
	int ret = -1;

	if (!uci) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	if (uci_load(uci, package, &pkg) != UCI_OK) {
		snprintf(err, errlen, "cannot load UCI package '%s'", package);
		goto out;
	}
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *enabled;

		if (!strcmp(s->e.name, FG_SECTION)) {
			if (strcmp(s->type, FG_SECTION_TYPE)) {
				snprintf(err, errlen, "section '%s' must have type '%s'", FG_SECTION, FG_SECTION_TYPE);
				goto out;
			}
			guest = s;
			continue;
		}
		if (strcmp(s->type, FG_SECTION_TYPE))
			continue;
		enabled = uci_lookup_option_string(uci, s, "enabled");
		if (!enabled || strcmp(enabled, "0")) {
			snprintf(err, errlen, "only the '%s' connection is supported; "
				 "set enabled '0' on or remove section '%s'", FG_SECTION, s->e.name);
			goto out;
		}
	}
	if (!guest) {
		snprintf(err, errlen, "missing 'config %s %s' section", FG_SECTION_TYPE, FG_SECTION);
		goto out;
	}

	struct section_ctx sc = { uci, guest };
	if (fg_config_parse(cfg, section_get, &sc, err, errlen))
		goto out;
	if (cfg->enabled && fg_uci_bridge_state(cfg->bridge) < 0) {
		snprintf(err, errlen, "network device '%s' is not of type bridge", cfg->bridge);
		goto out;
	}
	ret = 0;
out:
	uci_free_context(uci);
	return ret;
}

int fg_uci_bridge_state(const char *bridge)
{
	struct uci_context *uci = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	int state = 0;

	if (!uci)
		return 0;
	if (uci_load(uci, "network", &pkg) != UCI_OK)
		goto out;
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		const char *name, *type;

		if (strcmp(s->type, "device"))
			continue;
		name = uci_lookup_option_string(uci, s, "name");
		if (!name || strcmp(name, bridge))
			continue;
		type = uci_lookup_option_string(uci, s, "type");
		if (!type || strcmp(type, "bridge") || state) {
			state = -1;
			break;
		}
		state = 1;
	}
out:
	uci_free_context(uci);
	return state;
}
