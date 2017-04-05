/* Copyright (C) 2016-2017 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * This is a runtime IV server test that implements IV framework callbacks
 * TODOs:
 * - Randomize size of keys and values
 * - Add RPC to shutdown server & cleanup on shutdown
 * - Return shared buffer instead of a copy during fetch
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "crt_util/list.h"

#define _SERVER
#include "iv_common.h"

static char hostname[100];

static crt_rank_t my_rank;
static uint32_t group_size;

static int verbose_mode;

#define DBG_PRINT(x...)						\
	do {							\
		printf("[%s:%d:SERV]\t", hostname, my_rank);\
		printf(x);					\
	} while (0)


/* Verbose mode:
 * 0 - disabled
 * 1 - Entry/Exists
 * 2 - Dump keys
 **/
#define DBG_ENTRY()						\
do {								\
	if (verbose_mode >= 1) {				\
		DBG_PRINT(">>>> Entered %s\n", __func__);	\
	}							\
} while (0)

#define DBG_EXIT()							\
do {									\
	if (verbose_mode >= 1) {					\
		DBG_PRINT("<<<< Exited %s:%d\n\n", __func__, __LINE__);	\
	}								\
} while (0)

struct iv_value_struct {
	/* IV value embeds root rank for verification purposes */
	crt_rank_t	root_rank;

	/* Actual data string */
	char		str_data[MAX_DATA_SIZE];
};

#define NUM_WORK_CTX 9
crt_context_t work_ctx[NUM_WORK_CTX];
crt_context_t main_ctx;
static int do_shutdown;
pthread_t progress_thread[NUM_WORK_CTX + 1];
pthread_t shutdown_thread;
pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK_KEYS() pthread_mutex_lock(&key_lock)
#define UNLOCK_KEYS() pthread_mutex_unlock(&key_lock)

static void *
progress_function(void *data)
{
	crt_context_t *p_ctx = (crt_context_t *)data;

	while (do_shutdown == 0)
		crt_progress(*p_ctx, 1000, NULL, NULL);

	crt_context_destroy(*p_ctx, 1);

	return NULL;
}

void *
shutdown_threads(void *data)
{
	int i;

	DBG_PRINT("Joining threads\n");
	do_shutdown = 1;

	for (i = 0; i < NUM_WORK_CTX + 1; i++)
		pthread_join(progress_thread[i], NULL);

	DBG_PRINT("Finished joining all threads\n");

	return NULL;
}

static void
init_work_contexts(void)
{
	int i;
	int rc;

	rc = crt_context_create(NULL, &main_ctx);
	assert(rc == 0);

	rc = pthread_create(&progress_thread[0], 0,
			progress_function, &main_ctx);
	assert(rc == 0);

	for (i = 0; i < NUM_WORK_CTX; i++) {
		rc = crt_context_create(NULL, &work_ctx[i]);
		assert(rc == 0);

		rc = pthread_create(&progress_thread[i + 1], 0,
					progress_function, &work_ctx[i]);
		assert(rc == 0);
	}
}

#define NUM_LOCAL_IVS 10

/* TODO: Change to hash table instead of list */
static CRT_LIST_HEAD(kv_pair_head);

/* Key-value pair */
struct kv_pair_entry {
	crt_iv_key_t	key;
	crt_sg_list_t	value;
	bool		valid;
	crt_list_t	link;
};

static crt_iv_key_t *
alloc_key(int root, int key_id)
{
	crt_iv_key_t		*key;
	struct iv_key_struct	*key_struct;

	key = malloc(sizeof(crt_iv_key_t));
	assert(key != NULL);

	key->iov_buf = malloc(sizeof(struct iv_key_struct));
	assert(key->iov_buf != NULL);

	key->iov_buf_len = sizeof(struct iv_key_struct);
	key->iov_len = key->iov_buf_len;

	key_struct = (struct iv_key_struct *)key->iov_buf;

	key_struct->rank = root;
	key_struct->key_id = key_id;

	return key;
}

static void
verify_key_value_pair(crt_iv_key_t *key, crt_sg_list_t *value)
{
	struct iv_key_struct	*key_struct;
	struct iv_value_struct	*value_struct;

	key_struct = (struct iv_key_struct *)key->iov_buf;
	value_struct = (struct iv_value_struct *)value->sg_iovs[0].iov_buf;

	assert(key_struct->rank == value_struct->root_rank);
}

void
deinit_iv_storage(void)
{
	/* TODO: Implement graceful shutdown + storage freeing in stage2 */
}

/* Generate storage for iv keys */
static void
init_iv_storage(void)
{
	int			i;
	struct kv_pair_entry	*entry;
	struct iv_key_struct	*key_struct;
	struct iv_value_struct	*value_struct;
	crt_iv_key_t		*key;
	crt_sg_list_t		*value;
	int			size;

	/* First NUM_LOCAL_IVS are owned by the current rank */
	for (i = 0; i < NUM_LOCAL_IVS; i++) {
		entry = malloc(sizeof(struct kv_pair_entry));
		assert(entry != NULL);

		key = &entry->key;
		value = &entry->value;

		key->iov_buf = malloc(sizeof(struct iv_key_struct));
		assert(key->iov_buf != NULL);

		/* Fill in the key */
		key_struct = (struct iv_key_struct *)key->iov_buf;
		key_struct->rank = my_rank;
		key_struct->key_id = i;

		key->iov_len = sizeof(struct iv_key_struct);
		key->iov_buf_len = key->iov_len;

		entry->valid = true;

		/* Fill in the value */
		value->sg_nr.num = 1;
		value->sg_iovs = malloc(sizeof(crt_iov_t));
		assert(value->sg_iovs != NULL);

		size = sizeof(struct iv_value_struct);
		value->sg_iovs[0].iov_buf = malloc(size);
		assert(value->sg_iovs[0].iov_buf != NULL);

		value->sg_iovs[0].iov_len = size;
		value->sg_iovs[0].iov_buf_len = size;

		assert(value->sg_iovs[0].iov_buf != NULL);

		value_struct = (struct iv_value_struct *)
					value->sg_iovs[0].iov_buf;

		value_struct->root_rank = my_rank;

		sprintf(value_struct->str_data,
			"Default value for key %d:%d", my_rank, i);

		crt_list_add_tail(&entry->link, &kv_pair_head);
	}

	crt_list_for_each_entry(entry, &kv_pair_head, link) {
		key = &entry->key;
		value = &entry->value;

		verify_key_value_pair(key, value);
	}

	DBG_PRINT("Default %d keys for rank %d initialized\n",
		NUM_LOCAL_IVS, my_rank);
}

static bool
keys_equal(crt_iv_key_t *key1, crt_iv_key_t *key2)
{
	struct iv_key_struct *key_struct1;
	struct iv_key_struct *key_struct2;

	key_struct1 = (struct iv_key_struct *)key1->iov_buf;
	key_struct2 = (struct iv_key_struct *)key2->iov_buf;

	if ((key_struct1->rank == key_struct2->rank) &&
		(key_struct1->key_id == key_struct2->key_id)) {
		return true;
	}

	return false;
}

static int
copy_iv_value(crt_sg_list_t *dst, crt_sg_list_t *src)
{
	int i;

	assert(dst != NULL);
	assert(src != NULL);

	if (dst->sg_nr.num != src->sg_nr.num) {
		DBG_PRINT("dst = %d, src = %d\n",
			dst->sg_nr.num, src->sg_nr.num);

		assert(dst->sg_nr.num == src->sg_nr.num);
	}

	for (i = 0; i < dst->sg_nr.num; i++) {

		assert(dst->sg_iovs[i].iov_buf != NULL);
		assert(src->sg_iovs[i].iov_buf != NULL);

		memcpy(dst->sg_iovs[i].iov_buf, src->sg_iovs[i].iov_buf,
			src->sg_iovs[i].iov_buf_len);

		assert(dst->sg_iovs[i].iov_buf_len ==
			src->sg_iovs[i].iov_buf_len);

		assert(dst->sg_iovs[i].iov_len ==
			src->sg_iovs[i].iov_len);
	}

	return 0;
}

static void
verify_key(crt_iv_key_t *iv_key)
{
	assert(iv_key != NULL);
	assert(iv_key->iov_buf_len == sizeof(struct iv_key_struct));
	assert(iv_key->iov_len == sizeof(struct iv_key_struct));
	assert(iv_key->iov_buf != NULL);
}

static void
verify_value(crt_sg_list_t *iv_value)
{
	int size;

	size = sizeof(struct iv_value_struct);

	assert(iv_value != NULL);
	assert(iv_value->sg_nr.num == 1);
	assert(iv_value->sg_iovs != NULL);
	assert(iv_value->sg_iovs[0].iov_buf_len == size);
	assert(iv_value->sg_iovs[0].iov_len == size);
	assert(iv_value->sg_iovs[0].iov_buf != NULL);
}

static int
add_new_kv_pair(crt_iv_key_t *iv_key, crt_sg_list_t *iv_value,
		bool is_valid_entry)
{
	struct kv_pair_entry	*entry;
	int			size;
	int			i;

	/* If we are here it means we dont have this key cached yet */
	entry = malloc(sizeof(struct kv_pair_entry));
	assert(entry != NULL);

	entry->valid = is_valid_entry;

	/* Allocate space for iv key and copy it over*/
	entry->key.iov_buf = malloc(iv_key->iov_buf_len);
	assert(entry->key.iov_buf != NULL);

	memcpy(entry->key.iov_buf, iv_key->iov_buf, iv_key->iov_buf_len);

	entry->key.iov_buf_len = iv_key->iov_buf_len;
	entry->key.iov_len = iv_key->iov_len;

	/* Allocate space for iv value */
	entry->value.sg_nr.num = 1;
	entry->value.sg_iovs = malloc(sizeof(crt_iov_t));
	assert(entry->value.sg_iovs != NULL);

	size = sizeof(struct iv_value_struct);

	for (i = 0; i < entry->value.sg_nr.num; i++) {
		entry->value.sg_iovs[i].iov_buf = malloc(size);
		assert(entry->value.sg_iovs[i].iov_buf != NULL);

		entry->value.sg_iovs[i].iov_buf_len = size;
		entry->value.sg_iovs[i].iov_len = size;
	}

	if (is_valid_entry)
		copy_iv_value(&entry->value, iv_value);
	else {
		iv_value->sg_nr = entry->value.sg_nr;
		iv_value->sg_iovs = entry->value.sg_iovs;
	}

	crt_list_add_tail(&entry->link, &kv_pair_head);

	return 0;
}

static void
print_key_value(char *hdr, crt_iv_key_t *iv_key, crt_sg_list_t *iv_value)
{
	struct iv_key_struct *key_struct;
	struct iv_value_struct *value_struct;

	printf("[%s:%d:SERV]\t", hostname, my_rank);

	printf(hdr);

	if (iv_key == NULL) {
		printf("key=NULL");
	} else {

		key_struct = (struct iv_key_struct *)iv_key->iov_buf;
		if (key_struct == NULL)
			printf("key=EMPTY");
		else
			printf("key=[%d:%d]", key_struct->rank,
				key_struct->key_id);
	}

	printf(" ");

	if (iv_value == NULL) {
		printf("value=NULL");
	} else {

		value_struct = (struct iv_value_struct *)
					iv_value->sg_iovs[0].iov_buf;

		if (value_struct == NULL)
			printf("value=EMPTY");
		else
			printf("value='%s'", value_struct->str_data);
	}

	printf("\n");
}

static void
dump_all_keys(char *msg)
{
	struct kv_pair_entry *entry;

	if (verbose_mode < 2)
		return;

	DBG_PRINT("Dumping keys from %s\n", msg);

	LOCK_KEYS();
	crt_list_for_each_entry(entry, &kv_pair_head, link) {
		print_key_value("Entry = ", &entry->key, &entry->value);
	}
	UNLOCK_KEYS();

	DBG_PRINT("\n\n");
}

static int
iv_on_fetch(crt_iv_namespace_t ivns, crt_iv_key_t *iv_key,
	crt_iv_ver_t *iv_ver, bool root_flag, crt_sg_list_t *iv_value)
{
	struct kv_pair_entry *entry;
	struct iv_key_struct *key_struct;

	DBG_ENTRY();

	verify_key(iv_key);
	assert(iv_value != NULL);

	key_struct = (struct iv_key_struct *)iv_key->iov_buf;

	dump_all_keys("ON_FETCH");

	LOCK_KEYS();
	crt_list_for_each_entry(entry, &kv_pair_head, link) {

		if (keys_equal(iv_key, &entry->key) == true) {

			if (entry->valid) {
				copy_iv_value(iv_value, &entry->value);
				print_key_value("FETCH found key ", iv_key,
						iv_value);

				UNLOCK_KEYS();
				DBG_EXIT();
				return 0;
			}

			if (key_struct->rank == my_rank) {
				DBG_PRINT("Was my key, but its not valid\n");
				UNLOCK_KEYS();
				DBG_EXIT();
				return -1;
			}

			DBG_PRINT("Found key, but wasnt valid, forwarding\n");
			UNLOCK_KEYS();
			DBG_EXIT();
			return -CER_IVCB_FORWARD;
		}
	}
	UNLOCK_KEYS();

	DBG_PRINT("FETCH: Key [%d:%d] not found\n",
		key_struct->rank, key_struct->key_id);

	if (key_struct->rank == my_rank) {
		DBG_EXIT();
		return -1;
	}

	DBG_EXIT();
	return -CER_IVCB_FORWARD;
}

static int
iv_on_update(crt_iv_namespace_t ivns, crt_iv_key_t *iv_key,
	crt_iv_ver_t iv_ver, bool root_flag, crt_sg_list_t *iv_value)
{
	struct kv_pair_entry *entry;
	struct iv_key_struct *key_struct;
	int rc;

	DBG_ENTRY();

	verify_key(iv_key);
	verify_value(iv_value);

	print_key_value("UPDATE called ", iv_key, iv_value);

	dump_all_keys("ON_UPDATE");
	key_struct = (struct iv_key_struct *)iv_key->iov_buf;

	if (key_struct->rank == my_rank)
		rc = 0;
	else
		rc = -CER_IVCB_FORWARD;

	LOCK_KEYS();
	crt_list_for_each_entry(entry, &kv_pair_head, link) {
		if (keys_equal(iv_key, &entry->key) == true) {
			copy_iv_value(&entry->value, iv_value);
			UNLOCK_KEYS();

			dump_all_keys("ON_UPDATE; after copy");
			DBG_EXIT();
			return rc;
		}
	}

	add_new_kv_pair(iv_key, iv_value, true);
	UNLOCK_KEYS();
	DBG_EXIT();
	return rc;
}

static int
iv_on_refresh(crt_iv_namespace_t ivns, crt_iv_key_t *iv_key,
	crt_iv_ver_t iv_ver, crt_sg_list_t *iv_value, bool invalidate)
{
	struct kv_pair_entry	*entry = NULL;
	bool			valid;
	struct iv_key_struct	*key_struct;
	int			rc;

	DBG_ENTRY();
	valid = invalidate ? false : true;

	verify_key(iv_key);
	print_key_value("REFRESH called ", iv_key, iv_value);
	dump_all_keys("ON_REFRESH");

	key_struct = (struct iv_key_struct *)iv_key->iov_buf;

	if (key_struct->rank == my_rank)
		rc = 0;
	else
		rc = -CER_IVCB_FORWARD;


	LOCK_KEYS();
	crt_list_for_each_entry(entry, &kv_pair_head, link) {

		if (keys_equal(iv_key, &entry->key) == true) {

			if (iv_value == NULL) {
				DBG_PRINT("Marking entry as invalid!\n");
				entry->valid = false;
			} else {
				copy_iv_value(&entry->value, iv_value);

				entry->valid = valid;
			}

			UNLOCK_KEYS();
			DBG_EXIT();
			return rc;
		}
	}

	if (iv_value != NULL)
		add_new_kv_pair(iv_key, iv_value, valid);

	UNLOCK_KEYS();

	DBG_EXIT();

	return rc;
}

static int
iv_on_hash(crt_iv_namespace_t ivns, crt_iv_key_t *iv_key, crt_rank_t *root)
{
	struct iv_key_struct *key_struct;

	DBG_ENTRY();
	verify_key(iv_key);

	dump_all_keys("ON_HASH");
	key_struct = (struct iv_key_struct *)iv_key->iov_buf;
	*root = key_struct->rank;

	DBG_EXIT();
	return 0;
}

/* TODO: later change tmp_iv_value to be per-key */
static crt_sg_list_t *tmp_iv_value;

static int
iv_on_get(crt_iv_namespace_t ivns, crt_iv_key_t *iv_key,
		crt_iv_ver_t iv_ver, crt_iv_perm_t permission,
		crt_sg_list_t *iv_value)
{
	int size;

	DBG_ENTRY();
	dump_all_keys("ON_GETVALUE");

	if (tmp_iv_value == NULL) {

		tmp_iv_value = malloc(sizeof(crt_sg_list_t));
		assert(tmp_iv_value != NULL);

		size = sizeof(struct iv_value_struct);

		tmp_iv_value->sg_iovs = malloc(sizeof(crt_iov_t));
		assert(tmp_iv_value->sg_iovs != NULL);

		tmp_iv_value->sg_iovs[0].iov_buf = malloc(size);
		assert(tmp_iv_value->sg_iovs[0].iov_buf != NULL);

		tmp_iv_value->sg_iovs[0].iov_len = size;
		tmp_iv_value->sg_iovs[0].iov_buf_len = size;

		tmp_iv_value->sg_nr.num = 1;
	} else {
		DBG_PRINT("IV_GET_VALUE called before it was released\n");
		C_ASSERT(0);
	}

	*iv_value = *tmp_iv_value;

	DBG_EXIT();
	return 0;
}

static int
iv_on_put(crt_iv_namespace_t ivns, crt_iv_key_t *iv_key,
		crt_iv_ver_t iv_ver, crt_sg_list_t *iv_value)
{
	DBG_ENTRY();

	if (tmp_iv_value == 0) {
		DBG_PRINT("IV_PUT_VALUE called without acquired\n");
		C_ASSERT(0);
	}

	free(tmp_iv_value->sg_iovs);
	free(tmp_iv_value);
	tmp_iv_value = NULL;

	dump_all_keys("ON_PUTVALUE");
	DBG_EXIT();
	return 0;
}

struct crt_iv_ops ivc_ops = {
	.ivo_on_fetch = iv_on_fetch,
	.ivo_on_update = iv_on_update,
	.ivo_on_refresh = iv_on_refresh,
	.ivo_on_hash = iv_on_hash,
	.ivo_on_get = iv_on_get,
	.ivo_on_put = iv_on_put,
};

static crt_iv_namespace_t ivns;

static void
init_iv(void)
{
	struct crt_iv_class	iv_class;
	crt_iov_t		g_ivns;
	crt_endpoint_t		server_ep;
	struct rpc_set_ivns_in	*input;
	struct rpc_set_ivns_out	*output;
	int			rc;
	int			rank;
	crt_rpc_t		*rpc;
	int			tree_topo;

	tree_topo = crt_tree_topo(CRT_TREE_KNOMIAL, 2);

	if (my_rank == 0) {
		iv_class.ivc_id = 0;
		iv_class.ivc_feats = 0;
		iv_class.ivc_ops = &ivc_ops;

		rc = crt_iv_namespace_create(main_ctx, NULL, tree_topo,
				&iv_class, 1, &ivns, &g_ivns);
		assert(rc == 0);

		for (rank = 1; rank < group_size; rank++) {

			server_ep.ep_grp = NULL;
			server_ep.ep_rank = rank;
			server_ep.ep_tag = 0;

			rc = prepare_rpc_request(main_ctx, RPC_SET_IVNS,
					server_ep, (void *)&input, &rpc);
			assert(rc == 0);

			input->global_ivns_iov.iov_buf = g_ivns.iov_buf;
			input->global_ivns_iov.iov_buf_len =
							g_ivns.iov_buf_len;
			input->global_ivns_iov.iov_len = g_ivns.iov_len;

			rc = send_rpc_request(main_ctx, rpc, (void *)&output);
			assert(rc == 0);

			assert(output->rc == 0);
		}
	}
}

/* handler for RPC_SET_IVNS */
int
iv_set_ivns(crt_rpc_t *rpc)
{
	struct crt_iv_class	iv_class;
	struct rpc_set_ivns_in	*input;
	struct rpc_set_ivns_out	*output;
	int			rc;

	DBG_ENTRY();

	input = crt_req_get(rpc);
	output = crt_reply_get(rpc);

	assert(input != NULL);
	assert(output != NULL);

	iv_class.ivc_id = 0;
	iv_class.ivc_feats = 0;
	iv_class.ivc_ops = &ivc_ops;

	rc = crt_iv_namespace_attach(main_ctx, &input->global_ivns_iov,
				&iv_class, 1,  &ivns);
	assert(rc == 0);

	output->rc = 0;

	rc = crt_reply_send(rpc);
	assert(rc == 0);

	DBG_EXIT();
	return 0;
}

struct fetch_done_cb_info {
	crt_iv_key_t	*key;
	crt_rpc_t	*rpc;
};

static int
fetch_done(crt_iv_namespace_t ivns, uint32_t class_id,
	crt_iv_key_t *iv_key, crt_iv_ver_t *iv_ver, crt_sg_list_t *iv_value,
	int fetch_rc, void *cb_args)
{
	struct iv_key_struct		*key_struct;
	struct iv_value_struct		*value_struct;
	crt_iv_key_t			*expected_key;
	struct iv_key_struct		*expected_key_struct;
	struct fetch_done_cb_info	*cb_info;
	struct rpc_test_fetch_iv_out	*output;
	int				rc;

	cb_info = (struct fetch_done_cb_info *)cb_args;
	assert(cb_info != NULL);

	output = crt_reply_get(cb_info->rpc);
	assert(output != NULL);

	if (fetch_rc != 0) {
		DBG_PRINT("----------------------------------\n");
		print_key_value("Fetch failed: ", iv_key, iv_value);
		DBG_PRINT("----------------------------------\n");

		output->rc = fetch_rc;
		rc = crt_reply_send(cb_info->rpc);
		assert(rc == 0);

		rc = crt_req_decref(cb_info->rpc);
		assert(rc == 0);

		free(cb_info);
		return 0;
	}

	expected_key = cb_info->key;
	assert(expected_key != NULL);

	expected_key_struct = (struct iv_key_struct *)iv_key->iov_buf;
	assert(expected_key_struct != NULL);

	key_struct = (struct iv_key_struct *)iv_key->iov_buf;
	value_struct = (struct iv_value_struct *)iv_value->sg_iovs[0].iov_buf;

	assert(key_struct->rank == expected_key_struct->rank);
	assert(key_struct->key_id == expected_key_struct->key_id);
	assert(value_struct->root_rank == key_struct->rank);

	DBG_PRINT("----------------------------------\n");
	print_key_value("Fetch result: ", iv_key, iv_value);
	DBG_PRINT("----------------------------------\n");

	output->rc = 0;

	rc = crt_reply_send(cb_info->rpc);
	assert(rc == 0);

	rc = crt_req_decref(cb_info->rpc);
	assert(rc == 0);

	free(cb_info);
	return 0;
}

struct update_done_cb_info {
	crt_iv_key_t	*key;
	crt_rpc_t	*rpc;
};

static int
update_done(crt_iv_namespace_t ivns, uint32_t class_id,
	crt_iv_key_t *iv_key, crt_iv_ver_t *iv_ver, crt_sg_list_t *iv_value,
	int update_rc, void *cb_args)
{
	struct update_done_cb_info	*cb_info;
	struct rpc_test_update_iv_out	*output;
	int				rc;

	DBG_ENTRY();
	dump_all_keys("ON_UPDATE_DONE");

	cb_info = (struct update_done_cb_info *)cb_args;

	print_key_value("UPDATE_DONE called ", iv_key, iv_value);

	output = crt_reply_get(cb_info->rpc);
	output->rc = update_rc;

	rc = crt_reply_send(cb_info->rpc);
	assert(rc == 0);

	rc = crt_req_decref(cb_info->rpc);
	assert(rc == 0);

	free(cb_info);

	DBG_EXIT();
	return 0;
}

/* handler for RPC_TEST_UPDATE_IV */
int
iv_test_update_iv(crt_rpc_t *rpc)
{
	struct rpc_test_update_iv_in	*input;
	crt_iv_key_t			*key;
	struct iv_key_struct		*key_struct;
	int				rc;
	crt_sg_list_t			iv_value;
	struct iv_value_struct		*value_struct;
	struct update_done_cb_info	*update_cb_info;
	crt_iv_sync_t			*sync;

	input = crt_req_get(rpc);
	assert(input != NULL);

	key_struct = (struct iv_key_struct *)input->iov_key.iov_buf;
	key = alloc_key(key_struct->rank, key_struct->key_id);
	assert(key != NULL);

	DBG_PRINT("Performing update for %d:%d value=%s\n",
		key_struct->rank, key_struct->key_id, input->str_value);

	iv_value.sg_nr.num = 1;
	iv_value.sg_iovs = malloc(sizeof(crt_iov_t));
	assert(iv_value.sg_iovs != NULL);

	iv_value.sg_iovs[0].iov_buf = malloc(sizeof(struct iv_value_struct));
	assert(iv_value.sg_iovs[0].iov_buf != NULL);

	iv_value.sg_iovs[0].iov_buf_len = sizeof(struct iv_value_struct);
	iv_value.sg_iovs[0].iov_len = sizeof(struct iv_value_struct);

	value_struct = (struct iv_value_struct *)iv_value.sg_iovs[0].iov_buf;
	value_struct->root_rank = key_struct->rank;

	strncpy(value_struct->str_data, input->str_value, MAX_DATA_SIZE);

	sync = (crt_iv_sync_t *)input->iov_sync.iov_buf;
	assert(sync != NULL);

	update_cb_info = malloc(sizeof(struct update_done_cb_info));
	assert(update_cb_info != NULL);

	update_cb_info->key = key;
	update_cb_info->rpc = rpc;

	rc = crt_req_addref(rpc);
	assert(rc == 0);

	rc = crt_iv_update(ivns, 0, key, 0, &iv_value, 0, *sync, update_done,
			update_cb_info);

	return 0;
}

/* handler for RPC_TEST_FETCH_IV */
int
iv_test_fetch_iv(crt_rpc_t *rpc)
{
	struct rpc_test_fetch_iv_in	*input;
	crt_iv_key_t			*key;
	crt_sg_list_t			*iv_value;
	struct fetch_done_cb_info	*cb_info;
	int				rc;
	struct iv_key_struct		*key_struct;

	input = crt_req_get(rpc);
	assert(input != NULL);

	key_struct = (struct iv_key_struct *)input->iov_key.iov_buf;

	key = alloc_key(key_struct->rank, key_struct->key_id);
	assert(key != NULL);

	iv_value = malloc(sizeof(crt_sg_list_t));
	assert(iv_value != NULL);

	iv_value->sg_nr.num = 1;
	iv_value->sg_iovs = malloc(sizeof(crt_iov_t));
	assert(iv_value->sg_iovs != NULL);

	iv_value->sg_iovs[0].iov_buf = malloc(sizeof(struct iv_value_struct));
	assert(iv_value->sg_iovs[0].iov_buf != NULL);

	iv_value->sg_iovs[0].iov_buf_len = sizeof(struct iv_value_struct);
	iv_value->sg_iovs[0].iov_len = sizeof(struct iv_value_struct);

	cb_info = malloc(sizeof(struct fetch_done_cb_info));
	assert(cb_info != NULL);

	cb_info->key = key;
	cb_info->rpc = rpc;

	rc = crt_req_addref(rpc);
	assert(rc == 0);

	rc = crt_iv_fetch(ivns, 0, key, 0, iv_value, 0, fetch_done, cb_info);

	return 0;
}

struct invalidate_cb_info {
	crt_iv_key_t	*expect_key;
	crt_rpc_t	*rpc;
};

static int
invalidate_done(crt_iv_namespace_t ivns, uint32_t class_id,
	crt_iv_key_t *iv_key, crt_iv_ver_t *iv_ver, crt_sg_list_t *iv_value,
	int invalidate_rc, void *cb_args)
{
	struct invalidate_cb_info		*cb_info;
	struct rpc_test_invalidate_iv_out	*output;
	struct iv_key_struct			*key_struct;
	struct iv_key_struct			*expect_key_struct;
	int					rc;

	DBG_ENTRY();

	cb_info = (struct invalidate_cb_info *)cb_args;
	assert(cb_info != NULL);

	output = crt_reply_get(cb_info->rpc);
	assert(output != NULL);

	key_struct = (struct iv_key_struct *)iv_key->iov_buf;
	expect_key_struct = (struct iv_key_struct *)
					cb_info->expect_key->iov_buf;

	assert(key_struct->rank == expect_key_struct->rank);
	assert(key_struct->key_id == expect_key_struct->key_id);

	if (invalidate_rc != 0) {
		DBG_PRINT("----------------------------------\n");
		DBG_PRINT("Key = [%d,%d] Failed\n", key_struct->rank,
			key_struct->key_id);
		DBG_PRINT("----------------------------------\n");
	} else {
		DBG_PRINT("----------------------------------\n");
		DBG_PRINT("Key = [%d,%d] PASSED\n", key_struct->rank,
			key_struct->key_id);
		DBG_PRINT("----------------------------------\n");
	}

	output->rc = invalidate_rc;

	rc = crt_reply_send(cb_info->rpc);
	assert(rc == 0);

	rc = crt_req_decref(cb_info->rpc);
	assert(rc == 0);

	free(cb_info);
	DBG_EXIT();

	return 0;
}

int iv_test_invalidate_iv(crt_rpc_t *rpc)
{
	struct rpc_test_invalidate_iv_in	*input;
	struct iv_key_struct			*key_struct;
	crt_iv_key_t				*key;
	struct invalidate_cb_info		*cb_info;
	crt_iv_sync_t				sync = CRT_IV_SYNC_MODE_NONE;
	int					rc;

	input = crt_req_get(rpc);
	assert(input != NULL);

	key_struct = (struct iv_key_struct *)input->iov_key.iov_buf;

	key = alloc_key(key_struct->rank, key_struct->key_id);
	assert(key != NULL);

	rc = crt_req_addref(rpc);
	assert(rc == 0);

	cb_info = malloc(sizeof(struct invalidate_cb_info));
	assert(cb_info != NULL);

	cb_info->rpc = rpc;
	cb_info->expect_key = key;


	rc = crt_iv_invalidate(ivns, 0, key, 0, CRT_IV_SHORTCUT_NONE,
			sync, invalidate_done, cb_info);
	return 0;
}

static void
show_usage(char *app_name)
{
	printf("Usage: %s [options]\n", app_name);
	printf("Options are:\n");
	printf("-v <num> : verbose mode\n");
	printf("Verbose numbers are 0,1,2\n\n");
}

int main(int argc, char **argv)
{
	char	*arg_verbose = NULL;
	int	c;
	int	rc;

	while ((c = getopt(argc, argv, "v:")) != -1) {
		switch (c) {
		case 'v':
			arg_verbose = optarg;
			break;
		default:
			printf("Unknown option %c\n", c);
			show_usage(argv[0]);
			return -1;
		}
	}

	if (arg_verbose == NULL)
		verbose_mode = 0;
	else
		verbose_mode = atoi(arg_verbose);

	if (verbose_mode < 0 || verbose_mode > 3) {
		printf("-v verbose mode is between 0 and 3\n");
		return -1;
	}

	init_hostname(hostname, sizeof(hostname));

	rc = crt_init(NULL, CRT_FLAG_BIT_SERVER);
	assert(rc == 0);

	rc = crt_save_singleton_attach_info(NULL);
	assert(rc == 0);

	rc = RPC_REGISTER(RPC_TEST_FETCH_IV);
	assert(rc == 0);

	rc = RPC_REGISTER(RPC_TEST_UPDATE_IV);
	assert(rc == 0);

	rc = RPC_REGISTER(RPC_TEST_INVALIDATE_IV);
	assert(rc == 0);

	rc = RPC_REGISTER(RPC_SET_IVNS);
	assert(rc == 0);

	rc = crt_group_rank(NULL, &my_rank);
	assert(rc == 0);

	rc = crt_group_size(NULL, &group_size);
	assert(rc == 0);

	init_work_contexts();
	init_iv_storage();
	init_iv();

	while (1)
		sleep(1);

	return 0;
}