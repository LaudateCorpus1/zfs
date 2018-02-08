/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016 by Delphix. All rights reserved.
 */

#include <sys/lua/lua.h>
#include <sys/lua/lauxlib.h>

#include <sys/zcp.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_synctask.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_bookmark.h>
#include <sys/dsl_destroy.h>
#include <sys/dmu_objset.h>
#include <sys/zfs_znode.h>
#include <sys/zfeature.h>
#include <sys/metaslab.h>

#define	DST_AVG_BLKSHIFT 14

typedef int (zcp_synctask_func_t)(lua_State *, boolean_t, nvlist_t *);
typedef struct zcp_synctask_info {
	const char *name;
	zcp_synctask_func_t *func;
	zfs_space_check_t space_check;
	int blocks_modified;
	const zcp_arg_t pargs[4];
	const zcp_arg_t kwargs[2];
} zcp_synctask_info_t;

/*
 * Generic synctask interface for channel program syncfuncs.
 *
 * To perform some action in syncing context, we'd generally call
 * dsl_sync_task(), but since the Lua script is already running inside a
 * synctask we need to leave out some actions (such as acquiring the config
 * rwlock and performing space checks).
 *
 * If 'sync' is false, executes a dry run and returns the error code.
 *
 * This function also handles common fatal error cases for channel program
 * library functions. If a fatal error occurs, err_dsname will be the dataset
 * name reported in error messages, if supplied.
 */
static int
zcp_sync_task(lua_State *state, dsl_checkfunc_t *checkfunc,
    dsl_syncfunc_t *syncfunc, void *arg, boolean_t sync, const char *err_dsname)
{
	int err;
	zcp_run_info_t *ri = zcp_run_info(state);

	err = checkfunc(arg, ri->zri_tx);
	if (!sync)
		return (err);

	if (err == 0) {
		syncfunc(arg, ri->zri_tx);
	} else if (err == EIO) {
		if (err_dsname != NULL) {
			return (luaL_error(state,
			    "I/O error while accessing dataset '%s'",
			    err_dsname));
		} else {
			return (luaL_error(state,
			    "I/O error while accessing dataset."));
		}
	}

	return (err);
}


static int zcp_synctask_destroy(lua_State *, boolean_t, nvlist_t *);
static zcp_synctask_info_t zcp_synctask_destroy_info = {
	.name = "destroy",
	.func = zcp_synctask_destroy,
	.space_check = ZFS_SPACE_CHECK_NONE,
	.blocks_modified = 0,
	.pargs = {
	    {.za_name = "filesystem | snapshot", .za_lua_type = LUA_TSTRING},
	    {NULL, 0}
	},
	.kwargs = {
	    {.za_name = "defer", .za_lua_type = LUA_TBOOLEAN},
	    {NULL, 0}
	}
};

/* ARGSUSED */
static int
zcp_synctask_destroy(lua_State *state, boolean_t sync, nvlist_t *err_details)
{
	int err;
	const char *dsname = lua_tostring(state, 1);

	boolean_t issnap = (strchr(dsname, '@') != NULL);

	if (!issnap && !lua_isnil(state, 2)) {
		return (luaL_error(state,
		    "'deferred' kwarg only supported for snapshots: %s",
		    dsname));
	}

	if (issnap) {
		dsl_destroy_snapshot_arg_t ddsa = { 0 };
		ddsa.ddsa_name = dsname;
		if (!lua_isnil(state, 2)) {
			ddsa.ddsa_defer = lua_toboolean(state, 2);
		} else {
			ddsa.ddsa_defer = B_FALSE;
		}

		err = zcp_sync_task(state, dsl_destroy_snapshot_check,
		    dsl_destroy_snapshot_sync, &ddsa, sync, dsname);
	} else {
		dsl_destroy_head_arg_t ddha = { 0 };
		ddha.ddha_name = dsname;

		err = zcp_sync_task(state, dsl_destroy_head_check,
		    dsl_destroy_head_sync, &ddha, sync, dsname);
	}

	return (err);
}

static int zcp_synctask_promote(lua_State *, boolean_t, nvlist_t *err_details);
static zcp_synctask_info_t zcp_synctask_promote_info = {
	.name = "promote",
	.func = zcp_synctask_promote,
	.space_check = ZFS_SPACE_CHECK_RESERVED,
	.blocks_modified = 3,
	.pargs = {
	    {.za_name = "clone", .za_lua_type = LUA_TSTRING},
	    {NULL, 0}
	},
	.kwargs = {
	    {NULL, 0}
	}
};

static int
zcp_synctask_promote(lua_State *state, boolean_t sync, nvlist_t *err_details)
{
	int err;
	dsl_dataset_promote_arg_t ddpa = { 0 };
	const char *dsname = lua_tostring(state, 1);
	zcp_run_info_t *ri = zcp_run_info(state);

	ddpa.ddpa_clonename = dsname;
	ddpa.err_ds = err_details;
	ddpa.cr = ri->zri_cred;

	/*
	 * If there was a snapshot name conflict, then err_ds will be filled
	 * with a list of conflicting snapshot names.
	 */
	err = zcp_sync_task(state, dsl_dataset_promote_check,
	    dsl_dataset_promote_sync, &ddpa, sync, dsname);

	return (err);
}

void
zcp_synctask_wrapper_cleanup(void *arg)
{
	fnvlist_free(arg);
}

static int
zcp_synctask_wrapper(lua_State *state)
{
	int err;
	int num_ret = 1;
	nvlist_t *err_details = fnvlist_alloc();

	/*
	 * Make sure err_details is properly freed, even if a fatal error is
	 * thrown during the synctask.
	 */
	zcp_register_cleanup(state, &zcp_synctask_wrapper_cleanup, err_details);

	zcp_synctask_info_t *info = lua_touserdata(state, lua_upvalueindex(1));
	boolean_t sync = lua_toboolean(state, lua_upvalueindex(2));

	zcp_run_info_t *ri = zcp_run_info(state);
	dsl_pool_t *dp = ri->zri_pool;

	/* MOS space is triple-dittoed, so we multiply by 3. */
	uint64_t funcspace = (info->blocks_modified << DST_AVG_BLKSHIFT) * 3;

	zcp_parse_args(state, info->name, info->pargs, info->kwargs);

	err = 0;
	if (info->space_check != ZFS_SPACE_CHECK_NONE && funcspace > 0) {
		uint64_t quota = dsl_pool_adjustedsize(dp,
		    info->space_check == ZFS_SPACE_CHECK_RESERVED) -
		    metaslab_class_get_deferred(spa_normal_class(dp->dp_spa));
		uint64_t used = dsl_dir_phys(dp->dp_root_dir)->dd_used_bytes +
		    ri->zri_space_used;

		if (used + funcspace > quota) {
			err = SET_ERROR(ENOSPC);
		}
	}

	if (err == 0) {
		err = info->func(state, sync, err_details);
	}

	if (err == 0) {
		ri->zri_space_used += funcspace;
	}

	lua_pushnumber(state, (lua_Number)err);
	if (fnvlist_num_pairs(err_details) > 0) {
		(void) zcp_nvlist_to_lua(state, err_details, NULL, 0);
		num_ret++;
	}

	zcp_clear_cleanup(state);
	fnvlist_free(err_details);

	return (num_ret);
}

int
zcp_load_synctask_lib(lua_State *state, boolean_t sync)
{
	int i;
	zcp_synctask_info_t *zcp_synctask_funcs[] = {
		&zcp_synctask_destroy_info,
		&zcp_synctask_promote_info,
		NULL
	};

	lua_newtable(state);

	for (i = 0; zcp_synctask_funcs[i] != NULL; i++) {
		zcp_synctask_info_t *info = zcp_synctask_funcs[i];
		lua_pushlightuserdata(state, info);
		lua_pushboolean(state, sync);
		lua_pushcclosure(state, &zcp_synctask_wrapper, 2);
		lua_setfield(state, -2, info->name);
		info++;
	}

	return (1);
}