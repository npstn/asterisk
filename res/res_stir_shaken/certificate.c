/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2020, Sangoma Technologies Corporation
 *
 * Kevin Harwell <kharwell@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

#include "asterisk.h"

#include <sys/stat.h>

#include "asterisk/cli.h"
#include "asterisk/sorcery.h"

#include "stir_shaken.h"
#include "certificate.h"
#include "asterisk/res_stir_shaken.h"

#define CONFIG_TYPE "certificate"

struct stir_shaken_certificate {
	SORCERY_OBJECT(details);
	AST_DECLARE_STRING_FIELDS(
		/*! Path to a directory containing certificates */
		AST_STRING_FIELD(path);
		/*! URL to the public key */
		AST_STRING_FIELD(public_key_url);
		/*! The caller ID number associated with the certificate */
		AST_STRING_FIELD(caller_id_number);
	);
	/*! The private key for the certificate */
	EVP_PKEY *private_key;
};

static struct stir_shaken_certificate *stir_shaken_certificate_get(const char *id)
{
	return ast_sorcery_retrieve_by_id(ast_stir_shaken_sorcery(), CONFIG_TYPE, id);
}

static struct ao2_container *stir_shaken_certificate_get_all(void)
{
	return ast_sorcery_retrieve_by_fields(ast_stir_shaken_sorcery(), CONFIG_TYPE,
		AST_RETRIEVE_FLAG_MULTIPLE | AST_RETRIEVE_FLAG_ALL, NULL);
}

static void stir_shaken_certificate_destructor(void *obj)
{
	struct stir_shaken_certificate *cfg = obj;

	EVP_PKEY_free(cfg->private_key);
	ast_string_field_free_memory(cfg);
}

static void *stir_shaken_certificate_alloc(const char *name)
{
	struct stir_shaken_certificate *cfg;

	cfg = ast_sorcery_generic_alloc(sizeof(*cfg), stir_shaken_certificate_destructor);
	if (!cfg) {
		return NULL;
	}

	if (ast_string_field_init(cfg, 512)) {
		ao2_ref(cfg, -1);
		return NULL;
	}

	return cfg;
}

EVP_PKEY *stir_shaken_certificate_get_private_key(const char *caller_id_number)
{
	struct stir_shaken_certificate *cert;
	struct ast_variable fields = {
		.name = "caller_id_number",
		.value = caller_id_number,
		.next = NULL,
	};

	cert = ast_sorcery_retrieve_by_fields(ast_stir_shaken_sorcery(),
		"certificate", AST_RETRIEVE_FLAG_DEFAULT, &fields);

	if (cert) {
		return cert->private_key;
	}

	return NULL;
}

static int stir_shaken_certificate_apply(const struct ast_sorcery *sorcery, void *obj)
{
	EVP_PKEY *private_key;
	struct stir_shaken_certificate *cert = obj;

	if (strlen(cert->caller_id_number) == 0) {
		ast_log(LOG_ERROR, "Caller ID must be present\n");
		return -1;
	}

	private_key = read_private_key(cert->path);
	if (!private_key) {
		return -1;
	}

	cert->private_key = private_key;

	return 0;
}

static char *stir_shaken_certificate_show(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct stir_shaken_certificate *cfg;

	switch(cmd) {
	case CLI_INIT:
		e->command = "stir_shaken show certificate";
		e->usage =
			"Usage: stir_shaken show certificate <id>\n"
			"       Show the certificate stir/shaken settings for a given id\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 3) {
			return stir_shaken_tab_complete_name(a->word, stir_shaken_certificate_get_all());
		} else {
			return NULL;
		}
	}

	if (a->argc != 4) {
		return CLI_SHOWUSAGE;
	}

	cfg = stir_shaken_certificate_get(a->argv[3]);
	stir_shaken_cli_show(cfg, a, 0);
	ao2_cleanup(cfg);

	return CLI_SUCCESS;
}

static char *stir_shaken_certificate_show_all(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ao2_container *container;

	switch(cmd) {
	case CLI_INIT:
		e->command = "stir_shaken show certificates";
		e->usage =
			"Usage: stir_shaken show certificates\n"
			"       Show all configured certificates for stir/shaken\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}

	container = stir_shaken_certificate_get_all();
	if (!container || ao2_container_count(container) == 0) {
		ast_cli(a->fd, "No stir/shaken certificates found\n");
		ao2_cleanup(container);
		return CLI_SUCCESS;
	}

	ao2_callback(container, OBJ_NODATA, stir_shaken_cli_show, a);
	ao2_ref(container, -1);

	return CLI_SUCCESS;
}

static struct ast_cli_entry stir_shaken_certificate_cli[] = {
	AST_CLI_DEFINE(stir_shaken_certificate_show, "Show stir/shaken certificate configuration by id"),
	AST_CLI_DEFINE(stir_shaken_certificate_show_all, "Show all stir/shaken certificate configurations"),
};

static int on_load_path(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct stir_shaken_certificate *cfg = obj;
	struct stat statbuf;

	if (stat(var->value, &statbuf)) {
		ast_log(LOG_ERROR, "stir/shaken - path '%s' not found\n", var->value);
		return -1;
	}

	if (!S_ISREG(statbuf.st_mode)) {
		ast_log(LOG_ERROR, "stir/shaken - path '%s' is not a file\n", var->value);
		return -1;
	}

	return ast_string_field_set(cfg, path, var->value);
}

static int path_to_str(const void *obj, const intptr_t *args, char **buf)
{
	const struct stir_shaken_certificate *cfg = obj;

	*buf = ast_strdup(cfg->path);

	return 0;
}

static int on_load_public_key_url(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct stir_shaken_certificate *cfg = obj;

	if (!ast_begins_with(var->value, "http")) {
		ast_log(LOG_ERROR, "stir/shaken - public_key_url scheme must be 'http[s]'\n");
		return -1;
	}

	return ast_string_field_set(cfg, public_key_url, var->value);
}

static int public_key_url_to_str(const void *obj, const intptr_t *args, char **buf)
{
	const struct stir_shaken_certificate *cfg = obj;

	*buf = ast_strdup(cfg->public_key_url);

	return 0;
}

int stir_shaken_certificate_unload(void)
{
	ast_cli_unregister_multiple(stir_shaken_certificate_cli,
		ARRAY_LEN(stir_shaken_certificate_cli));

	return 0;
}

int stir_shaken_certificate_load(void)
{
	struct ast_sorcery *sorcery = ast_stir_shaken_sorcery();

	ast_sorcery_apply_default(sorcery, CONFIG_TYPE, "config", "stir_shaken.conf,criteria=type=certificate");

	if (ast_sorcery_object_register(sorcery, CONFIG_TYPE, stir_shaken_certificate_alloc,
			NULL, stir_shaken_certificate_apply)) {
		ast_log(LOG_ERROR, "stir/shaken - failed to register '%s' sorcery object\n", CONFIG_TYPE);
		return -1;
	}

	ast_sorcery_object_field_register(sorcery, CONFIG_TYPE, "type", "", OPT_NOOP_T, 0, 0);
	ast_sorcery_object_field_register_custom(sorcery, CONFIG_TYPE, "path", "",
		on_load_path, path_to_str, NULL, 0, 0);
	ast_sorcery_object_field_register_custom(sorcery, CONFIG_TYPE, "public_key_url", "",
		on_load_public_key_url, public_key_url_to_str, NULL, 0, 0);
	ast_sorcery_object_field_register(sorcery, CONFIG_TYPE, "caller_id_number", "", OPT_STRINGFIELD_T, 0, STRFLDSET(struct stir_shaken_certificate, caller_id_number));

	ast_cli_register_multiple(stir_shaken_certificate_cli,
		ARRAY_LEN(stir_shaken_certificate_cli));

	return 0;
}
