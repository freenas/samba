/*
 * Copyright (C) 2016 iXsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "includes.h"
#include "winbindd.h"
#include "winbindd_ads.h"
#include "../libds/common/flags.h"
#include "ads.h"
#include "idmap.h"
#include "../libcli/security/dom_sid.h"
#include "../libcli/ldap/ldap_ndr.h"
#include "../libcli/security/security.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_IDMAP

#define CHECK_ALLOC_DONE(mem) do { \
	if (!mem) { \
		DEBUG(0, ("Out of memory!\n")); \
		ret = NT_STATUS_NO_MEMORY; \
		goto done; \
	} \
} while (0)

struct idmap_fruit_context {
	ADS_STRUCT *ads;
};

static int idmap_fruit_context_destructor(struct idmap_fruit_context *ctx)
{
	if (ctx->ads != NULL) {
		ctx->ads->is_mine = True;
		ads_destroy(&ctx->ads);
		ctx->ads = NULL;
	}

	return 0;
}

static bool idmap_fruit_guid_to_id(const struct GUID *guid, uint32_t *id)
{
	TALLOC_CTX *ctx = talloc_new(NULL);
	size_t slen = 512, llen = 64, ulen = 16;
	char *stripped = talloc_array(ctx, char, slen);
	char *ldap_bytes = talloc_array(ctx, char, llen);
	char *uuid_bytes = talloc_array(ctx, char, ulen);
	char *hexstr, *ptr;
	int i = 0, s = 0;
	char c;

	if (guid == NULL || id == NULL)
		return False;

	hexstr = GUID_hexstring(ctx, guid);
	DEBUG(10, ("hexstr: %s\n", hexstr));
	while ((c = hexstr[i])) {
		if((c >='a' && c <= 'f')
			|| (c >= 'A' && c <= 'F')
			|| (c >= '0' && c <= '9')) {
			stripped[s++] = tolower(c);
		}
		i++;
	}

	snprintf(ldap_bytes, llen,
		"%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c"
		"%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
		stripped[6], stripped[7], stripped[4], stripped[5],
		stripped[2], stripped[3], stripped[0], stripped[1],
		stripped[10], stripped[11], stripped[8], stripped[9],
		stripped[14], stripped[15], stripped[12], stripped[13],
		stripped[16], stripped[17], stripped[18], stripped[19],
		stripped[20], stripped[21], stripped[22], stripped[23],
		stripped[24], stripped[25], stripped[26], stripped[27],
		stripped[28], stripped[29], stripped[30], stripped[31]);

	DEBUG(10, ("ldap_bytes: %s\n", ldap_bytes));
	snprintf(uuid_bytes, 9, "%s", ldap_bytes);
	uuid_bytes[9] = 0;

	ptr = &uuid_bytes[0];
	if (uuid_bytes[0] == '0' || uuid_bytes[0] == '8') {
		ptr = &uuid_bytes[1];
	} else if (uuid_bytes[0] >= '9') {
		uuid_bytes[0] -= '0';
		uuid_bytes[0] += 1;
		ptr = uuid_bytes;
	}

	DEBUG(10, ("uuid_bytes: %s\n", uuid_bytes));

	*id = strtol(ptr, NULL, 16);
	DEBUG(10, ("id: %d\n", *id));
	talloc_free(ctx);
	return True;
}


static NTSTATUS idmap_fruit_initialize(struct idmap_domain *dom)
{
	NTSTATUS ret;
	ADS_STATUS status;
	struct idmap_fruit_context *ctx;
	char *config_option = NULL;

	ctx = talloc_zero(dom, struct idmap_fruit_context);
	if (ctx == NULL) {
		DEBUG(0, ("Out of memory!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	talloc_set_destructor(ctx, idmap_fruit_context_destructor);

	config_option = talloc_asprintf(ctx, "idmap config %s", dom->name);
	if (!config_option) {
		DEBUG(0, ("Out of memory!\n"));
		ret = NT_STATUS_NO_MEMORY;
		goto failed;
	}

	status = ads_idmap_cached_connection(&ctx->ads, dom->name);
	if (!ADS_ERR_OK(status)) {
		DEBUG(1, ("ADS uninitialized: %s\n", ads_errstr(status)));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto failed;
	}

	dom->private_data = ctx;

	talloc_free(config_option);
	return NT_STATUS_OK;

failed:
	talloc_free(ctx);
	return ret;
}


/**********************************
 lookup a set of unix ids. 
**********************************/

static NTSTATUS idmap_fruit_unixids_to_sids(struct idmap_domain *dom, struct id_map **ids)
{
	NTSTATUS ret;
	TALLOC_CTX *memctx;
	struct idmap_fruit_context *ctx;
	ADS_STATUS rc;
	const char *attrs[] = { 
		"sAMAccountType",
		"objectSid",
		"objectGUID",
		NULL
	};
	LDAPMessage *res = NULL;
	LDAPMessage *entry = NULL;
	char *filter = NULL;
	int idx = 0;
	int bidx = 0;
	int count;
	int i;
	char *u_filter = NULL;
	char *g_filter = NULL;

	/* initialize the status to avoid suprise */
	for (i = 0; ids[i]; i++) {
		ids[i]->status = ID_UNKNOWN;
	}

	/* Only do query if we are online */
	if (idmap_is_offline()) {
		return NT_STATUS_FILE_IS_OFFLINE;
	}

	ctx = talloc_get_type(dom->private_data, struct idmap_fruit_context);

	if ((memctx = talloc_new(ctx)) == NULL) {
		DEBUG(0, ("Out of memory!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	rc = ads_idmap_cached_connection(&ctx->ads, dom->name);
	if (!ADS_ERR_OK(rc)) {
		DEBUG(1, ("ADS uninitialized: %s\n", ads_errstr(rc)));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto done;
	}

again:
	bidx = idx;
	for (i = 0; (i < IDMAP_LDAP_MAX_IDS) && ids[idx]; i++, idx++) {
		fstring sidstr;

		switch (ids[idx]->xid.type) {
			case ID_TYPE_UID:
				if (!u_filter) {
					u_filter = talloc_asprintf(memctx, "(&(|"
						"(sAMAccountType=%d)"
						"(sAMAccountType=%d)"
						"(sAMAccountType=%d))(|",
						ATYPE_NORMAL_ACCOUNT,
						ATYPE_WORKSTATION_TRUST,
						ATYPE_INTERDOMAIN_TRUST);
				}
				sid_to_fstring(sidstr, ids[idx]->sid);
				u_filter = talloc_asprintf_append_buffer(u_filter,
					"(objectSid=%s)", sidstr);
				CHECK_ALLOC_DONE(u_filter);
				break;

			case ID_TYPE_GID:
				if (!g_filter) {
					g_filter = talloc_asprintf(memctx, "(&(|"
						"(sAMAccountType=%d)"
						"(sAMAccountType=%d))(|",
						ATYPE_SECURITY_GLOBAL_GROUP,
						ATYPE_SECURITY_LOCAL_GROUP);
				}
				sid_to_fstring(sidstr, ids[idx]->sid);
				g_filter = talloc_asprintf_append_buffer(u_filter,
					"(objectSid=%s)", sidstr);
				CHECK_ALLOC_DONE(u_filter);
				break;

			default:
				DEBUG(3, ("ERROR: mapping requested but unknown ID type\n"));
				ids[idx]->status = ID_UNKNOWN;
				continue;
		}
	}

	filter = talloc_asprintf(memctx, "(|");
	CHECK_ALLOC_DONE(filter);

	if (u_filter) {
		filter = talloc_asprintf_append_buffer(filter, "%s))", u_filter);
		CHECK_ALLOC_DONE(filter);
		TALLOC_FREE(u_filter);
	}

	if (g_filter) {
		filter = talloc_asprintf_append_buffer(filter, "%s))", g_filter);
		CHECK_ALLOC_DONE(filter);
		TALLOC_FREE(g_filter);
	}

	filter = talloc_asprintf_append_buffer(filter, ")");
	CHECK_ALLOC_DONE(filter);

	rc = ads_search_retry(ctx->ads, &res, filter, attrs);
	if (!ADS_ERR_OK(rc)) {
		DEBUG(1, ("ERROR: ads search returned: %s\n", ads_errstr(rc)));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto done;
	}

	if ((count = ads_count_replies(ctx->ads, res)) == 0) {
		DEBUG(10, ("No IDs found\n"));
	}

	entry = res;
	for (i = 0; (i < count) && entry; i++) {
		struct dom_sid sid;
		struct id_map *map;
		struct GUID guid;
		enum id_type type;
		uint32_t id;
		uint32_t atype;

		if (i == 0) {
			entry = ads_first_entry(ctx->ads, entry);
		} else {
			entry = ads_next_entry(ctx->ads, entry);
		}

		if (!entry) {
			DEBUG(2, ("ERROR: Unable to fetch ldap entries from results\n"));
			break;
		}

		/* first check if the SID is present */
		if (!ads_pull_sid(ctx->ads, entry, "objectSid", &sid)) {
			DEBUG(2, ("Could not retrieve SID from entry\n"));
			continue;
		}

		/* get type */
		if (!ads_pull_uint32(ctx->ads, entry, "sAMAccountType", &atype)) {
			DEBUG(1, ("could not get SAM account type\n"));
			continue;
		}

		switch (atype & 0xF0000000) {
			case ATYPE_SECURITY_GLOBAL_GROUP:
			case ATYPE_SECURITY_LOCAL_GROUP:
				type = ID_TYPE_GID;
				break;

			case ATYPE_NORMAL_ACCOUNT:
			case ATYPE_WORKSTATION_TRUST:
			case ATYPE_INTERDOMAIN_TRUST:
				type = ID_TYPE_UID;
				break;

			default:
				DEBUG(1, ("unrecognized SAM account type %08x\n", atype));
				continue;
		}

		if (!ads_pull_guid(ctx->ads, entry, &guid)) {
			DEBUG(1, ("could not get GUID for SID %s\n", sid_string_dbg(map->sid)));
			continue;
		}

		if (!idmap_fruit_guid_to_id(&guid, &id)) {
			DEBUG(1, ("could not map GUID to ID for SID %s\n", sid_string_dbg(map->sid)));
			continue;
		}

		map = idmap_find_map_by_id(&ids[bidx], type, id);
		if (!map) {
			DEBUG(2, ("WARNING: couldn't match result with requested ID\n"));
			continue;
		}

		sid_copy(map->sid, &sid);

		/* mapped */
		map->status = ID_MAPPED;

		DEBUG(10, ("Mapped %s -> %lu (%d)\n", sid_string_dbg(map->sid),
			(unsigned long)map->xid.id,
			map->xid.type));
	}

	if (res) {
		ads_msgfree(ctx->ads, res);
	}

	if (ids[idx]) { /* still some values to map */
		goto again;
	}

	ret = NT_STATUS_OK;

	/* mark all unknown/expired ones as unmapped */
	for (i = 0; ids[i]; i++) {
		if (ids[i]->status != ID_MAPPED)
			ids[i]->status = ID_UNMAPPED;
	}

done:
	talloc_free(memctx);
	return ret;
}

/**********************************
 lookup a set of sids. 
**********************************/

static NTSTATUS idmap_fruit_sids_to_unixids(struct idmap_domain *dom, struct id_map **ids)
{
	NTSTATUS ret;
	TALLOC_CTX *memctx;
	struct idmap_fruit_context *ctx;
	ADS_STATUS rc;
	const char *attrs[] = { 
		"sAMAccountType",
		"objectSid",
		"objectGUID",
		NULL
	};
	LDAPMessage *res = NULL;
	LDAPMessage *entry = NULL;
	char *filter = NULL;
	char *sidstr;
	int idx = 0;
	int bidx = 0;
	int count;
	int i;

	/* initialize the status to avoid suprise */
	for (i = 0; ids[i]; i++) {
		ids[i]->status = ID_UNKNOWN;
	}

	/* Only do query if we are online */
	if (idmap_is_offline()) {
		return NT_STATUS_FILE_IS_OFFLINE;
	}

	ctx = talloc_get_type(dom->private_data, struct idmap_fruit_context);

	if ((memctx = talloc_new(ctx)) == NULL) {
		DEBUG(0, ("Out of memory!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	rc = ads_idmap_cached_connection(&ctx->ads, dom->name);
	if (!ADS_ERR_OK(rc)) {
		DEBUG(1, ("ADS uninitialized: %s\n", ads_errstr(rc)));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto done;
	}

again:
	filter = talloc_asprintf(memctx, "(&(|"
		"(sAMAccountType=%d)(sAMAccountType=%d)(sAMAccountType=%d)" /* user account types */
		"(sAMAccountType=%d)(sAMAccountType=%d)" /* group account types */
		")(|",
		ATYPE_NORMAL_ACCOUNT, ATYPE_WORKSTATION_TRUST, ATYPE_INTERDOMAIN_TRUST,
		ATYPE_SECURITY_GLOBAL_GROUP, ATYPE_SECURITY_LOCAL_GROUP);

	CHECK_ALLOC_DONE(filter);

	bidx = idx;
	for (i = 0; (i < IDMAP_LDAP_MAX_IDS) && ids[idx]; i++, idx++) {
		ids[idx]->status = ID_UNKNOWN;

		sidstr = ldap_encode_ndr_dom_sid(talloc_tos(), ids[idx]->sid);
		filter = talloc_asprintf_append_buffer(filter, "(objectSid=%s)", sidstr);

		TALLOC_FREE(sidstr);
		CHECK_ALLOC_DONE(filter);
	}

	filter = talloc_asprintf_append_buffer(filter, "))");
	CHECK_ALLOC_DONE(filter);
	DEBUG(10, ("Filter: [%s]\n", filter));

	rc = ads_search_retry(ctx->ads, &res, filter, attrs);
	if (!ADS_ERR_OK(rc)) {
		DEBUG(1, ("ERROR: ads search returned: %s\n", ads_errstr(rc)));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto done;
	}

	if ((count = ads_count_replies(ctx->ads, res)) == 0) {
		DEBUG(10, ("No IDs found\n"));
	}

	entry = res;
	for (i = 0; (i < count) && entry; i++) {
		struct dom_sid sid;
		struct id_map *map;
		struct GUID guid;
		enum id_type type;
		uint32_t id;
		uint32_t atype;

		if (i == 0) {
			entry = ads_first_entry(ctx->ads, entry);
		} else {
			entry = ads_next_entry(ctx->ads, entry);
		}

		if (!entry) {
			DEBUG(2, ("ERROR: Unable to fetch ldap entries from results\n"));
			break;
		}

		/* first check if the SID is present */
		if (!ads_pull_sid(ctx->ads, entry, "objectSid", &sid)) {
			DEBUG(2, ("Could not retrieve SID from entry\n"));
			continue;
		}

		map = idmap_find_map_by_sid(&ids[bidx], &sid);
		if (!map) {
			DEBUG(2, ("WARNING: couldn't match result with requested SID\n"));
			continue;
		}

		/* get type */
		if (!ads_pull_uint32(ctx->ads, entry, "sAMAccountType", &atype)) {
			DEBUG(1, ("could not get SAM account type\n"));
			continue;
		}

		switch (atype & 0xF0000000) {
			case ATYPE_SECURITY_GLOBAL_GROUP:
			case ATYPE_SECURITY_LOCAL_GROUP:
				type = ID_TYPE_GID;
				break;

			case ATYPE_NORMAL_ACCOUNT:
			case ATYPE_WORKSTATION_TRUST:
			case ATYPE_INTERDOMAIN_TRUST:
				type = ID_TYPE_UID;
				break;

			default:
				DEBUG(1, ("unrecognized SAM account type %08x\n", atype));
				continue;
		}

		if (!ads_pull_guid(ctx->ads, entry, &guid)) {
			DEBUG(1, ("could not get GUID for SID %s\n", sid_string_dbg(map->sid)));
			continue;
		}

		/* mapped */
		map->xid.type = type;
		if (!idmap_fruit_guid_to_id(&guid, &map->xid.id)) {
			DEBUG(1, ("could not map GUID to ID for SID %s\n", sid_string_dbg(map->sid)));
			continue;
		}
		map->status = ID_MAPPED;

		DEBUG(10, ("Mapped %s -> %lu (%d)\n", sid_string_dbg(map->sid),
			(unsigned long)map->xid.id,
			map->xid.type));
	}

	if (res) {
		ads_msgfree(ctx->ads, res);
	}

	if (ids[idx]) { /* still some values to map */
		goto again;
	}

	ret = NT_STATUS_OK;

	/* mark all unknown/expired ones as unmapped */
	for (i = 0; ids[i]; i++) {
		if (ids[i]->status != ID_MAPPED)
			ids[i]->status = ID_UNMAPPED;
	}

done:
	talloc_free(memctx);
	return ret;
}

static struct idmap_methods fruit_methods = {
	.init = idmap_fruit_initialize,
	.unixids_to_sids = idmap_fruit_unixids_to_sids,
	.sids_to_unixids = idmap_fruit_sids_to_unixids,
};

NTSTATUS idmap_fruit_init(TALLOC_CTX *ctx)
{
	return smb_register_idmap(SMB_IDMAP_INTERFACE_VERSION, "fruit", &fruit_methods);
}
