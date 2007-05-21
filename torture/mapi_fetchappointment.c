/* 
   OpenChange MAPI implementation testsuite

   Fetch appointment items from an Exchange server

   Copyright (C) Julien Kerihuel 2007
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/


#include <libmapi/libmapi.h>
#include <gen_ndr/ndr_exchange.h>
#include <param.h>
#include <credentials.h>
#include <torture/torture.h>
#include <torture/torture_proto.h>
#include <samba/popt.h>

BOOL torture_rpc_mapi_fetchappointment(struct torture_context *torture)
{
	NTSTATUS		nt_status;
	enum MAPISTATUS		retval;
	struct dcerpc_pipe	*p;
	TALLOC_CTX		*mem_ctx;
	BOOL			ret = True;
	struct mapi_session	*session;
	uint64_t		id_calendar;
	mapi_object_t		obj_store;
	mapi_object_t		obj_calendar;
	mapi_object_t		obj_cal_table;
	struct SRowSet		SRowSet;
	struct SPropTagArray	*SPropTagArray;

	/* init torture */
	mem_ctx = talloc_init("torture_rpc_mapi_fetchappointment");
	nt_status = torture_rpc_connection(mem_ctx, &p, &dcerpc_table_exchange_emsmdb);
	if (!NT_STATUS_IS_OK(nt_status)) {
		talloc_free(mem_ctx);
		return False;
	}

	/* init mapi */
	if ((session = torture_init_mapi(mem_ctx)) == NULL) return False;
	
	/* init objects */
	mapi_object_init(&obj_store);
	mapi_object_init(&obj_calendar);
	mapi_object_init(&obj_cal_table);

	/* session::OpenMsgStore */
	retval = OpenMsgStore(&obj_store);
	mapi_errstr("OpenMsgStore", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* Retrieve the default calendar folder id */
	retval = GetDefaultFolder(&obj_store, &id_calendar, olFolderCalendar);
	mapi_errstr("GetDefaultFolder", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* We now open the calendar folder */
	retval = OpenFolder(&obj_store, id_calendar, &obj_calendar);
	mapi_errstr("OpenFolder", GetLastError());
	if (retval != MAPI_E_SUCCESS) return False;

	/* Operations on the calendar folder */
	retval = GetContentsTable(&obj_calendar, &obj_cal_table);
	if (retval != MAPI_E_SUCCESS) return False;

	SPropTagArray = set_SPropTagArray(mem_ctx, 0x8,
					  PR_FID,
					  PR_MID,
					  PR_INST_ID,
					  PR_INSTANCE_NUM,
					  PR_SUBJECT,
					  PR_MESSAGE_CLASS,
					  PR_RULE_MSG_PROVIDER,
					  PR_RULE_MSG_NAME);
	retval = SetColumns(&obj_cal_table, SPropTagArray);
	MAPIFreeBuffer(SPropTagArray);
	if (retval != MAPI_E_SUCCESS) return False;

	retval = QueryRows(&obj_cal_table, 0x32, TBL_ADVANCE, &SRowSet);
	if (retval != MAPI_E_SUCCESS) return False;

	{
		int i;
		mapi_object_t obj_message;
		struct mapi_SPropValue_array properties_array;

		printf("We have %d appointments in the table\n", SRowSet.cRows);
		for (i = 0; i < SRowSet.cRows; i++) {
			mapi_object_init(&obj_message);
			retval = OpenMessage(&obj_calendar, 
					     SRowSet.aRow[i].lpProps[0].value.d,
					     SRowSet.aRow[i].lpProps[1].value.d,
					     &obj_message);
			if (retval != MAPI_E_NOT_FOUND) {
				retval = GetPropsAll(&obj_message, &properties_array);
				if (retval == MAPI_E_SUCCESS) {
					mapidump_appointment(&properties_array);
					mapi_object_release(&obj_message);
				}
			}
		}
	}

	mapi_object_release(&obj_cal_table);
	mapi_object_release(&obj_calendar);
	mapi_object_release(&obj_store);

	/* uninitialize mapi
	 */
	MAPIUninitialize();
	talloc_free(mem_ctx);
	
	return (ret);
}
