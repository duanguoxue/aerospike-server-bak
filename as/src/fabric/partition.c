/*
 * partition.c
 *
 * Copyright (C) 2008-2016 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

//==========================================================
// Includes.
//

#include "fabric/partition.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_b64.h"

#include "fault.h"
#include "node.h"

#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/index.h"
#include "fabric/partition_balance.h"


//==========================================================
// Constants and typedefs.
//

// XXX JUMP - remove in "six months".
const as_partition_vinfo NULL_VINFO = { 0 };


//==========================================================
// Forward declarations.
//

cf_node find_best_node(const as_partition* p, bool is_read);
void accumulate_replica_stats(const as_partition* p, bool is_ldt_enabled, uint64_t* p_n_objects, uint64_t* p_n_sub_objects, uint64_t* p_n_tombstones);
int partition_reserve_read_write(as_namespace* ns, uint32_t pid, as_partition_reservation* rsv, cf_node* node, bool is_read, uint64_t* cluster_key);
void partition_reserve_lockfree(as_partition* p, as_namespace* ns, as_partition_reservation* rsv);
cf_node partition_getreplica_prole(as_namespace* ns, uint32_t pid);
char partition_getstate_str(const as_partition* p);
int partition_get_replica_self_lockfree(const as_namespace* ns, uint32_t pid);

int
find_self_in_replicas(const as_partition* p)
{
	return index_of_node(p->replicas, p->n_replicas, g_config.self_node);
}


//==========================================================
// Public API.
//

void
as_partition_init(as_namespace* ns, uint32_t pid)
{
	as_partition* p = &ns->partitions[pid];

	// Note - as_partition has been zeroed since it's a member of as_namespace.
	// Set non-zero members.

	pthread_mutex_init(&p->lock, NULL);

	p->id = pid;

	if (! as_new_clustering()) {
		p->state = AS_PARTITION_STATE_ABSENT;
	}

	if (ns->cold_start) {
		p->vp = as_index_tree_create(&ns->tree_shared, ns->arena);

		if (ns->ldt_enabled) {
			p->sub_vp = as_index_tree_create(&ns->tree_shared, ns->arena);
		}
	}
	else {
		p->vp = as_index_tree_resume(&ns->tree_shared, ns->arena,
				&ns->xmem_roots[pid * ns->tree_shared.n_sprigs]);

		if (ns->ldt_enabled) {
			p->sub_vp = as_index_tree_resume(&ns->tree_shared, ns->arena,
					&ns->sub_tree_roots[pid * ns->tree_shared.n_sprigs]);
		}
	}
}


void
as_partition_shutdown(as_namespace* ns, uint32_t pid)
{
	as_partition* p = &ns->partitions[pid];

	pthread_mutex_lock(&p->lock);

	as_index_tree_shutdown(p->vp,
			&ns->xmem_roots[pid * ns->tree_shared.n_sprigs]);

	if (ns->ldt_enabled) {
		as_index_tree_shutdown(p->sub_vp,
				&ns->sub_tree_roots[pid * ns->tree_shared.n_sprigs]);
	}
}


// Get a list of all nodes (excluding self) that are replicas for a specified
// partition: place the list in *nv and return the number of nodes found.
uint32_t
as_partition_get_other_replicas(as_partition* p, cf_node* nv)
{
	uint32_t n_other_replicas = 0;

	pthread_mutex_lock(&p->lock);

	for (uint32_t repl_ix = 0; repl_ix < p->n_replicas; repl_ix++) {
		// Don't ever include yourself.
		if (p->replicas[repl_ix] == g_config.self_node) {
			continue;
		}

		// Copy the node ID into the user-supplied vector.
		nv[n_other_replicas++] = p->replicas[repl_ix];
	}

	pthread_mutex_unlock(&p->lock);

	return n_other_replicas;
}


cf_node
as_partition_writable_node(as_namespace* ns, uint32_t pid)
{
	as_partition* p = &ns->partitions[pid];

	pthread_mutex_lock(&p->lock);

	cf_node best_node = find_best_node(p, false);

	pthread_mutex_unlock(&p->lock);

	return best_node;
}


// If this node is an eventual master, return the acting master, else return 0.
cf_node
as_partition_proxyee_redirect(as_namespace* ns, uint32_t pid)
{
	as_partition* p = &ns->partitions[pid];

	pthread_mutex_lock(&p->lock);

	bool is_final_master = p->replicas[0] == g_config.self_node;
	cf_node acting_master = p->origin; // 0 if final master is working master

	pthread_mutex_unlock(&p->lock);

	return is_final_master ? acting_master : (cf_node)0;
}


// TODO - deprecate in "six months".
void
as_partition_get_replicas_prole_str(cf_dyn_buf* db)
{
	uint8_t prole_bitmap[CLIENT_BITMAP_BYTES];
	char b64_bitmap[CLIENT_B64MAP_BYTES];

	size_t db_sz = db->used_sz;

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace* ns = g_config.namespaces[ns_ix];

		memset(prole_bitmap, 0, sizeof(uint8_t) * CLIENT_BITMAP_BYTES);
		cf_dyn_buf_append_string(db, ns->name);
		cf_dyn_buf_append_char(db, ':');

		for (uint32_t pid = 0; pid < AS_PARTITIONS; pid++) {
			if (g_config.self_node == partition_getreplica_prole(ns, pid) ) {
				prole_bitmap[pid >> 3] |= (0x80 >> (pid & 7));
			}
		}

		cf_b64_encode(prole_bitmap, CLIENT_BITMAP_BYTES, b64_bitmap);
		cf_dyn_buf_append_buf(db, (uint8_t*)b64_bitmap, CLIENT_B64MAP_BYTES);
		cf_dyn_buf_append_char(db, ';');
	}

	if (db_sz != db->used_sz) {
		cf_dyn_buf_chomp(db);
	}
}


void
as_partition_get_replicas_master_str(cf_dyn_buf* db)
{
	size_t db_sz = db->used_sz;

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace* ns = g_config.namespaces[ns_ix];

		cf_dyn_buf_append_string(db, ns->name);
		cf_dyn_buf_append_char(db, ':');
		cf_dyn_buf_append_buf(db, (uint8_t*)ns->replica_maps[0].b64map,
				sizeof(ns->replica_maps[0].b64map));
		cf_dyn_buf_append_char(db, ';');
	}

	if (db_sz != db->used_sz) {
		cf_dyn_buf_chomp(db);
	}
}


void
as_partition_get_replicas_all_str(cf_dyn_buf* db)
{
	size_t db_sz = db->used_sz;

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace* ns = g_config.namespaces[ns_ix];

		cf_dyn_buf_append_string(db, ns->name);
		cf_dyn_buf_append_char(db, ':');

		uint32_t repl_factor = ns->replication_factor;

		cf_dyn_buf_append_uint32(db, repl_factor);

		for (uint32_t repl_ix = 0; repl_ix < repl_factor; repl_ix++) {
			cf_dyn_buf_append_char(db, ',');
			cf_dyn_buf_append_buf(db,
					(uint8_t*)&ns->replica_maps[repl_ix].b64map,
					sizeof(ns->replica_maps[repl_ix].b64map));
		}

		cf_dyn_buf_append_char(db, ';');
	}

	if (db_sz != db->used_sz) {
		cf_dyn_buf_chomp(db);
	}
}


void
as_partition_get_replica_stats(as_namespace* ns, repl_stats* p_stats)
{
	memset(p_stats, 0, sizeof(repl_stats));

	for (uint32_t pid = 0; pid < AS_PARTITIONS; pid++) {
		as_partition* p = &ns->partitions[pid];

		pthread_mutex_lock(&p->lock);

		int self_n = find_self_in_replicas(p); // -1 if not
		bool is_working_master = (self_n == 0 && p->origin == (cf_node)0) ||
				p->target != (cf_node)0;

		if (is_working_master) {
			accumulate_replica_stats(p, ns->ldt_enabled,
					&p_stats->n_master_objects,
					&p_stats->n_master_sub_objects,
					&p_stats->n_master_tombstones);
		}
		else if (self_n >= 0) {
			accumulate_replica_stats(p, ns->ldt_enabled,
					&p_stats->n_prole_objects,
					&p_stats->n_prole_sub_objects,
					&p_stats->n_prole_tombstones);
		}
		else {
			accumulate_replica_stats(p, ns->ldt_enabled,
					&p_stats->n_non_replica_objects,
					&p_stats->n_non_replica_sub_objects,
					&p_stats->n_non_replica_tombstones);
		}

		pthread_mutex_unlock(&p->lock);
	}
}


int
as_partition_reserve_write(as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv, cf_node* node, uint64_t* cluster_key)
{
	return partition_reserve_read_write(ns, pid, rsv, node, false, cluster_key);
}


int
as_partition_reserve_read(as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv, cf_node* node, uint64_t* cluster_key)
{
	return partition_reserve_read_write(ns, pid, rsv, node, true, cluster_key);
}


void
as_partition_reserve_migrate(as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv, cf_node* node)
{
	as_partition* p = &ns->partitions[pid];

	pthread_mutex_lock(&p->lock);

	partition_reserve_lockfree(p, ns, rsv);

	pthread_mutex_unlock(&p->lock);

	if (node) {
		*node = g_config.self_node;
	}
}


int
as_partition_reserve_migrate_timeout(as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv, cf_node* node, int timeout_ms)
{
	as_partition* p = &ns->partitions[pid];

	struct timespec tp;
	cf_set_wait_timespec(timeout_ms, &tp);

	if (0 != pthread_mutex_timedlock(&p->lock, &tp)) {
		return -1;
	}

	partition_reserve_lockfree(p, ns, rsv);

	pthread_mutex_unlock(&p->lock);

	if (node) {
		*node = g_config.self_node;
	}

	return 0;
}


// Reserves all query-able partitions.
// Returns the number of partitions reserved.
int
as_partition_prereserve_query(as_namespace* ns, bool can_partition_query[],
		as_partition_reservation rsv[])
{
	int reserved = 0;

	for (uint32_t pid = 0; pid < AS_PARTITIONS; pid++) {
		if (as_partition_reserve_query(ns, pid, &rsv[pid])) {
			can_partition_query[pid] = false;
		}
		else {
			can_partition_query[pid] = true;
			reserved++;
		}
	}

	return reserved;
}


// Reserve a partition for query.
// Return value 0 means the reservation was taken, -1 means not.
int
as_partition_reserve_query(as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv)
{
	return as_partition_reserve_write(ns, pid, rsv, NULL, NULL);
}


// Obtain a partition reservation for XDR reads. Succeeds, if we are sync or
// zombie for the partition.
int
as_partition_reserve_xdr_read(as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv)
{
	as_partition* p = &ns->partitions[pid];

	pthread_mutex_lock(&p->lock);

	int res = -1;

	if (as_new_clustering()) {
		if (! as_partition_version_is_null(&p->version)) {
			partition_reserve_lockfree(p, ns, rsv);
			res = 0;
		}
	}
	else {
		if (! as_partition_is_null(&p->version_info)) {
			partition_reserve_lockfree(p, ns, rsv);
			res = 0;
		}
	}

	pthread_mutex_unlock(&p->lock);

	return res;
}


void
as_partition_reservation_copy(as_partition_reservation* dst,
		as_partition_reservation* src)
{
	dst->ns = src->ns;
	dst->p = src->p;
	dst->tree = src->tree;
	dst->sub_tree = src->sub_tree;
	dst->cluster_key = src->cluster_key;
	dst->reject_repl_write = src->reject_repl_write;
	dst->n_dupl = src->n_dupl;

	if (dst->n_dupl != 0) {
		memcpy(dst->dupl_nodes, src->dupl_nodes, sizeof(cf_node) * dst->n_dupl);
	}
}


void
as_partition_release(as_partition_reservation* rsv)
{
	as_index_tree_release(rsv->tree);

	if (rsv->ns->ldt_enabled) {
		as_index_tree_release(rsv->sub_tree);
	}
}


void
as_partition_getinfo_str(cf_dyn_buf* db)
{
	size_t db_sz = db->used_sz;

	cf_dyn_buf_append_string(db, "namespace:partition:state:replica:n_dupl:"
			"origin:target:emigrates:immigrates:records:sub_records:tombstones:"
			"ldt_version:version:final_version;");

	for (uint32_t ns_ix = 0; ns_ix < g_config.n_namespaces; ns_ix++) {
		as_namespace* ns = g_config.namespaces[ns_ix];

		for (uint32_t pid = 0; pid < AS_PARTITIONS; pid++) {
			as_partition* p = &ns->partitions[pid];

			pthread_mutex_lock(&p->lock);

			char state_c = partition_getstate_str(p);
			int self_n = find_self_in_replicas(p);

			cf_dyn_buf_append_string(db, ns->name);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint32(db, pid);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_char(db, state_c);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_int(db, self_n == -1 ?
					(int)p->n_replicas : self_n);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint32(db, p->n_dupl);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint64_x(db, p->origin);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint64_x(db, p->target);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_int(db, p->pending_emigrations);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_int(db, p->pending_immigrations);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint32(db, as_index_tree_size(p->vp));
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint32(db, ns->ldt_enabled ?
					as_index_tree_size(p->sub_vp) : 0);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint64(db, p->n_tombstones);
			cf_dyn_buf_append_char(db, ':');
			cf_dyn_buf_append_uint64_x(db, p->current_outgoing_ldt_version);
			cf_dyn_buf_append_char(db, ':');

			if (as_new_clustering()) {
				cf_dyn_buf_append_string(db,
						VERSION_AS_STRING(&p->version));
				cf_dyn_buf_append_char(db, ':');
				cf_dyn_buf_append_string(db,
						VERSION_AS_STRING(&p->final_version));
			}
			else {
				cf_dyn_buf_append_uint64_x(db, p->version_info.iid);
				cf_dyn_buf_append_char(db, '-');
				cf_dyn_buf_append_uint64_x(db,
						*(uint64_t*)&p->version_info.vtp[0]);
				cf_dyn_buf_append_char(db, '-');
				cf_dyn_buf_append_uint64_x(db,
						*(uint64_t*)&p->version_info.vtp[8]);
				cf_dyn_buf_append_char(db, ':');
				cf_dyn_buf_append_uint64_x(db, p->primary_version_info.iid);
				cf_dyn_buf_append_char(db, '-');
				cf_dyn_buf_append_uint64_x(db,
						*(uint64_t*)&p->primary_version_info.vtp[0]);
				cf_dyn_buf_append_char(db, '-');
				cf_dyn_buf_append_uint64_x(db,
						*(uint64_t*)&p->primary_version_info.vtp[8]);
			}

			cf_dyn_buf_append_char(db, ';');

			pthread_mutex_unlock(&p->lock);
		}
	}

	if (db_sz != db->used_sz) {
		cf_dyn_buf_chomp(db); // take back the final ';'
	}
}


//==========================================================
// Public API - client view replica maps.
//

void
client_replica_maps_create(as_namespace* ns)
{
	uint32_t size = sizeof(client_replica_map) * ns->cfg_replication_factor;

	ns->replica_maps = cf_malloc(size);
	memset(ns->replica_maps, 0, size);

	for (uint32_t repl_ix = 0; repl_ix < ns->cfg_replication_factor;
			repl_ix++) {
		client_replica_map* repl_map = &ns->replica_maps[repl_ix];

		pthread_mutex_init(&repl_map->write_lock, NULL);

		cf_b64_encode((uint8_t*)repl_map->bitmap,
				(uint32_t)sizeof(repl_map->bitmap), (char*)repl_map->b64map);
	}
}


void
client_replica_maps_clear(as_namespace* ns)
{
	memset(ns->replica_maps, 0,
			sizeof(client_replica_map) * ns->cfg_replication_factor);

	for (uint32_t repl_ix = 0; repl_ix < ns->cfg_replication_factor;
			repl_ix++) {
		client_replica_map* repl_map = &ns->replica_maps[repl_ix];

		cf_b64_encode((uint8_t*)repl_map->bitmap,
				(uint32_t)sizeof(repl_map->bitmap), (char*)repl_map->b64map);
	}
}


bool
client_replica_maps_update(as_namespace* ns, uint32_t pid)
{
	uint32_t byte_i = pid >> 3;
	uint32_t byte_chunk = (byte_i / 3);
	uint32_t chunk_bitmap_offset = byte_chunk * 3;
	uint32_t chunk_b64map_offset = byte_chunk << 2;

	uint32_t bytes_from_end = CLIENT_BITMAP_BYTES - chunk_bitmap_offset;
	uint32_t input_size = bytes_from_end > 3 ? 3 : bytes_from_end;

	int replica = partition_get_replica_self_lockfree(ns, pid); // -1 if not
	uint8_t set_mask = 0x80 >> (pid & 0x7);
	bool changed = false;

	for (int repl_ix = 0; repl_ix < (int)ns->cfg_replication_factor;
			repl_ix++) {
		client_replica_map* repl_map = &ns->replica_maps[repl_ix];

		volatile uint8_t* mbyte = repl_map->bitmap + byte_i;
		bool owned = replica == repl_ix;
		bool is_set = (*mbyte & set_mask) != 0;
		bool needs_update = (owned && ! is_set) || (! owned && is_set);

		if (! needs_update) {
			continue;
		}

		volatile uint8_t* bitmap_chunk = repl_map->bitmap + chunk_bitmap_offset;
		volatile char* b64map_chunk = repl_map->b64map + chunk_b64map_offset;

		pthread_mutex_lock(&repl_map->write_lock);

		*mbyte ^= set_mask;
		cf_b64_encode((uint8_t*)bitmap_chunk, input_size, (char*)b64map_chunk);

		pthread_mutex_unlock(&repl_map->write_lock);

		changed = true;
	}

	return changed;
}


bool
client_replica_maps_is_partition_queryable(const as_namespace* ns, uint32_t pid)
{
	uint32_t byte_i = pid >> 3;

	const client_replica_map* repl_map = ns->replica_maps;
	const volatile uint8_t* mbyte = repl_map->bitmap + byte_i;

	uint8_t set_mask = 0x80 >> (pid & 0x7);

	return (*mbyte & set_mask) != 0;
}


//==========================================================
// Local helpers.
//

// Find best node to handle read/write. Called within partition lock.
cf_node
find_best_node(const as_partition* p, bool is_read)
{
	int self_n = find_self_in_replicas(p);
	bool is_final_master = self_n == 0;
	bool is_prole = self_n > 0; // self_n is -1 if not replica
	bool is_acting_master = p->target != 0;
	bool is_working_master = (is_final_master && p->origin == (cf_node)0) ||
			is_acting_master;

	if (is_working_master) {
		return g_config.self_node;
	}

	if (is_final_master) {
		return p->origin; // acting master elsewhere
	}

	if (is_read && is_prole && p->origin == (cf_node)0) {
		return g_config.self_node;
	}

	return p->replicas[0]; // final master as a last resort
}


void
accumulate_replica_stats(const as_partition* p, bool is_ldt_enabled,
		uint64_t* p_n_objects, uint64_t* p_n_sub_objects,
		uint64_t* p_n_tombstones)
{
	int64_t n_tombstones = (int64_t)p->n_tombstones;
	int64_t n_objects = (int64_t)as_index_tree_size(p->vp) - n_tombstones;

	*p_n_objects += n_objects > 0 ? (uint64_t)n_objects : 0;

	if (is_ldt_enabled) {
		*p_n_sub_objects += as_index_tree_size(p->sub_vp);
	}

	*p_n_tombstones += (uint64_t)n_tombstones;
}


int
partition_reserve_read_write(as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv, cf_node* node, bool is_read,
		uint64_t* cluster_key)
{
	as_partition* p = &ns->partitions[pid];

	pthread_mutex_lock(&p->lock);

	cf_node best_node = find_best_node(p, is_read);

	if (node) {
		*node = best_node;
	}

	if (cluster_key) {
		*cluster_key = p->cluster_key;
	}

	// If this node is not the appropriate one, return.
	if (best_node != g_config.self_node) {
		pthread_mutex_unlock(&p->lock);
		return -1;
	}

	partition_reserve_lockfree(p, ns, rsv);

	pthread_mutex_unlock(&p->lock);

	return 0;
}


void
partition_reserve_lockfree(as_partition* p, as_namespace* ns,
		as_partition_reservation* rsv)
{
	cf_rc_reserve(p->vp);

	if (ns->ldt_enabled) {
		cf_rc_reserve(p->sub_vp);
	}

	rsv->ns = ns;
	rsv->p = p;
	rsv->tree = p->vp;
	rsv->sub_tree = p->sub_vp;
	rsv->cluster_key = p->cluster_key;

	if (as_new_clustering()) {
		// FIXME - this is equivalent, but is it correct ???
		rsv->reject_repl_write = as_partition_version_is_null(&p->version);
	}
	else {
		rsv->reject_repl_write = p->state == AS_PARTITION_STATE_ABSENT;
	}

	rsv->n_dupl = p->n_dupl;

	if (rsv->n_dupl != 0) {
		memcpy(rsv->dupl_nodes, p->dupls, sizeof(cf_node) * rsv->n_dupl);
	}
}


// TODO - deprecate in "six months".
cf_node
partition_getreplica_prole(as_namespace* ns, uint32_t pid)
{
	as_partition* p = &ns->partitions[pid];

	pthread_mutex_lock(&p->lock);

	// Check is this is a master node.
	cf_node best_node = find_best_node(p, false);

	if (best_node == g_config.self_node) {
		// It's a master, return 0.
		best_node = (cf_node)0;
	}
	else {
		// Not a master, see if it's a prole.
		best_node = find_best_node(p, true);
	}

	pthread_mutex_unlock(&p->lock);

	return best_node;
}


// Definition for the partition-info data:
// name:part_id:STATE:replica_count(int):origin:target:migrate_tx:migrate_rx:sz
char
partition_getstate_str(const as_partition* p)
{
	if (as_new_clustering()) {
		int self_n = find_self_in_replicas(p); // -1 if not

		if (self_n >= 0) {
			return p->pending_immigrations == 0 ? 'S' : 'D';
		}

		return as_partition_version_is_null(&p->version) ? 'A' : 'Z';
	}
	else {
		switch (p->state) {
		case AS_PARTITION_STATE_UNDEF:
			return 'U';
		case AS_PARTITION_STATE_SYNC:
			return 'S';
		case AS_PARTITION_STATE_DESYNC:
			return 'D';
		case AS_PARTITION_STATE_ZOMBIE:
			return 'Z';
		case AS_PARTITION_STATE_ABSENT:
			return 'A';
		default:
			return '?';
		}
	}
}


int
partition_get_replica_self_lockfree(const as_namespace* ns, uint32_t pid)
{
	const as_partition* p = &ns->partitions[pid];

	int self_n = find_self_in_replicas(p); // -1 if not
	bool is_working_master = (self_n == 0 && p->origin == (cf_node)0) ||
			p->target != (cf_node)0;

	if (is_working_master) {
		return 0;
	}

	if (self_n > 0 && p->origin == (cf_node)0 &&
			// Check self_n < n_repl only because n_repl could be out-of-sync
			// with (less than) partition's replica list count.
			self_n < (int)ns->replication_factor) {
		return self_n;
	}

	return -1; // not a replica
}
