/*****************************************************************************
** Copyright (C) 2014 Intel Corporation.                                    **
**                                                                          **
** Licensed under the Apache License, Version 2.0 (the "License");          **
** you may not use this file except in compliance with the License.         **
** You may obtain a copy of the License at                                  **
**                                                                          **
**      http://www.apache.org/licenses/LICENSE-2.0                          **
**                                                                          **
** Unless required by applicable law or agreed to in writing, software      **
** distributed under the License is distributed on an "AS IS" BASIS,        **
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. **
** See the License for the specific language governing permissions and      **
** limitations under the License.                                           **
*****************************************************************************/

#include <stdint.h>

#include "callbacks.h"
#include "tee_internal_client_api.h"

TEE_Result TEE_OpenTASession(TEE_UUID *destination, uint32_t cancellationRequestTimeout,
			     uint32_t paramTypes, TEE_Param params[4],
			     TEE_TASessionHandle *session, uint32_t *returnOrigin)
{
	TEE_Result (*open_ta_session)(TEE_UUID *destination, uint32_t cancellationRequestTimeout,
				      uint32_t paramTypes, TEE_Param params[4],
				      TEE_TASessionHandle *session,
				      uint32_t *returnOrigin) = fn_ptr_open_ta_session();

	return open_ta_session(destination, cancellationRequestTimeout, paramTypes,
			       params, session, returnOrigin);
}

void TEE_CloseTASession(TEE_TASessionHandle session)
{
	void (*closeTaSession)(TEE_TASessionHandle session) = fn_ptr_close_ta_session();

	closeTaSession(session);
}

TEE_Result TEE_InvokeTACommand(TEE_TASessionHandle session, uint32_t cancellationRequestTimeout,
			       uint32_t commandID, uint32_t paramTypes, TEE_Param params[4],
			       uint32_t *returnOrigin)
{
	TEE_Result (*invoke_ta_command)(TEE_TASessionHandle session,
					uint32_t cancellationRequestTimeout,
					uint32_t commandID, uint32_t paramTypes,
					TEE_Param params[4],
					uint32_t *returnOrigin) = fn_ptr_invoke_ta_command();

	return invoke_ta_command(session, cancellationRequestTimeout, commandID,
				 paramTypes, params, returnOrigin);
}
