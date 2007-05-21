/*
 *  OpenChange MAPI implementation.
 *
 *  Copyright (C) Julien Kerihuel 2007.
 *  Copyright (C) Fabien Le Mentec 2007.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#ifndef	__MAPI_OBJECT_H
#define	__MAPI_OBJECT_H


#include <gen_ndr/exchange.h>


/* forward declarations
 */
struct mapi_session;
struct mapi_handles;


/* generic mapi object definition
 */

typedef uint64_t mapi_id_t;
typedef uint32_t mapi_handle_t;

typedef struct mapi_object {
	uint64_t		id;
	mapi_handle_t		handle;
	struct mapi_session	*session;
	struct mapi_handles	*handles;
	void			*private_data;
} mapi_object_t;


/*
 * Interface objects
 */


/**
 * IMsgStore object 
 */
typedef struct mapi_obj_store
{
	uint64_t	fid_non_ipm_subtree;
	uint64_t	fid_deferred_actions;
	uint64_t	fid_spooler_queue;
	uint64_t	fid_top_information_store;
	uint64_t	fid_inbox;
	uint64_t	fid_outbox;
	uint64_t	fid_sent_items;
	uint64_t	fid_deleted_items;
	uint64_t	fid_common_views;
	uint64_t	fid_schedule;
	uint64_t	fid_finder;
	uint64_t	fid_views;
	uint64_t	fid_shortcuts;
} mapi_object_store_t;


/**
 * IMAPITable object 
 */
typedef struct mapi_obj_table
{
	struct SPropTagArray proptags;
} mapi_object_table_t;

#endif /*!__MAPI_OBJECT_H */
