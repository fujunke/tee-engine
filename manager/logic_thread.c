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

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "com_protocol.h"
#include "extern_resources.h"
#include "h_table.h"
#include "io_thread.h"
#include "socket_help.h"
#include "ta_exit_states.h"
#include "ta_dir_watch.h"
#include "tee_list.h"
#include "tee_logging.h"
#include "logic_thread.h"

/* Used for hashtable init */
#define TA_SESS_COUNT_EST 50

static void release_ta_file_locks(proc_t ta)
{
	ta = ta;
	/* TODO: Implement */
}

static void add_and_notify_io_sock_to_close(int fd)
{
	struct sock_to_close *fd_to_close = NULL;
	const uint64_t event = 1;

	/* Notify logic thread */
	fd_to_close = calloc(1, sizeof(struct sock_to_close));
	if (!fd_to_close) {
		OT_LOG(LOG_ERR, "out-of-memory");
		return;
	}

	fd_to_close->sockfd = fd;

	/* Lock task queue from logic thread */
	if (pthread_mutex_lock(&socks_to_close_mutex)) {
		OT_LOG(LOG_ERR, "Failed to lock the mutex");
		free(fd_to_close);
		return;
	}

	list_add_after(&fd_to_close->list, &socks_to_close.list);

	if (pthread_mutex_unlock(&socks_to_close_mutex)) {
		/* For now, just log error */
		OT_LOG(LOG_ERR, "Failed to lock the mutex");
	}

	/* notify the I/O thread that there is something at output queue */
	if (write(event_close_sock, &event, sizeof(uint64_t)) == -1) {
		OT_LOG(LOG_ERR, "Failed to notify the io thread");
		/* TODO: See what is causing it! */
	}
}

static void free_sess(proc_t del_sess)
{
	if (!del_sess || del_sess->p_type != proc_t_session)
		return;

	h_table_remove(del_sess->content.sesLink.owner->content.process.links,
		       (unsigned char *)&del_sess->content.sesLink.session_id, sizeof(uint64_t));

	free(del_sess);
	del_sess = NULL;
}

static void free_proc(proc_t del_proc)
{
	proc_t proc_sess = NULL;

	if (!del_proc || del_proc->p_type == proc_t_session)
		return;

	/* Free process sessions
	 * Note: It is a programmer error, if he close session and there is session open */
	h_table_init_stepper(del_proc->content.process.links);
	while (1) {
		proc_sess = h_table_step(del_proc->content.process.links);
		if (proc_sess)
			free(h_table_remove(del_proc->content.process.links,
					    (unsigned char *)
					    (&proc_sess->content.sesLink.session_id),
					    sizeof(uint64_t)));
		else
			break;
	}

	/* Free session hashtable */
	h_table_free(del_proc->content.process.links);

	/* Client process spesific operations */
	if (del_proc->p_type == proc_t_CA) {

		if (pthread_mutex_lock(&CA_table_mutex) == -1) {
			OT_LOG(LOG_ERR, "Failed to lock mutex")
			goto skip;
		}

		h_table_remove(clientApps,
			       (unsigned char *)&del_proc->sockfd, sizeof(del_proc->sockfd));

		if (pthread_mutex_unlock(&CA_table_mutex))
			OT_LOG(LOG_ERR, "Failed to unlock the mutex");
	}
skip:
	/* Trusted process spesific operations */
	if (del_proc->p_type == proc_t_TA) {
		release_ta_file_locks(del_proc);
		h_table_remove(trustedApps,
			       (unsigned char *)&del_proc->content.process.pid, sizeof(pid_t));
	}

	add_and_notify_io_sock_to_close(del_proc->sockfd);

	free(del_proc);
	del_proc = NULL;
}

static void add_msg_out_queue_and_notify(struct manager_msg *man_msg)
{
	const uint64_t event = 1;

	/* Lock task queue from logic thread */
	if (pthread_mutex_lock(&done_queue_mutex)) {
		OT_LOG(LOG_ERR, "Failed to lock the mutex");
		free_manager_msg(man_msg);
		return;
	}

	/* enqueue the task manager queue */
	list_add_before(&man_msg->list, &done_queue.list);

	if (pthread_mutex_unlock(&done_queue_mutex)) {
		/* For now, just log error */
		OT_LOG(LOG_ERR, "Failed to lock the mutex");
	}

	/* notify the I/O thread that there is something at output queue */
	if (write(event_out_queue_fd, &event, sizeof(uint64_t)) == -1) {
		OT_LOG(LOG_ERR, "Failed to notify the io thread");
		/* TODO/PLACEHOLDER: notify IO thread */
	}
}

static void gen_err_msg_and_add_to_out(struct manager_msg *man_msg, uint32_t err_origin,
				       uint32_t err_name)
{
	free(man_msg->msg); /* replace old message with error */

	man_msg->msg = calloc(1, sizeof(struct com_msg_error));
	if (!man_msg->msg) {
		OT_LOG(LOG_ERR, "Out of memory");
		return;
	}

	man_msg->msg_len = sizeof(struct com_msg_error);

	/* Fill error message */
	((struct com_msg_error *)man_msg->msg)->msg_hdr.msg_name = COM_MSG_NAME_ERROR;
	((struct com_msg_error *)man_msg->msg)->ret_origin = err_origin;
	((struct com_msg_error *)man_msg->msg)->ret = err_name;

	add_msg_out_queue_and_notify(man_msg);
}

static void get_next_operation_id(uint64_t *next_op_id)
{
	static uint64_t next_operation_id;

	*next_op_id = ++next_operation_id;

	/* Note: Zero is reserved ID! */
	if (!*next_op_id)
		(*next_op_id)++;
}

static void ca_init_context(struct manager_msg *man_msg)
{
	struct com_msg_ca_init_tee_conn *init_msg = man_msg->msg;

	/* Valid init message */
	if (init_msg->msg_hdr.msg_name != COM_MSG_NAME_CA_INIT_CONTEXT ||
	    init_msg->msg_hdr.msg_type != COM_TYPE_QUERY) {
		OT_LOG(LOG_ERR, "Parsing wrong message, ignore msg");
		goto discard_msg;
	}

	/* Message can be received only from client */
	if (man_msg->proc->p_type != proc_t_CA ||
	    man_msg->proc->content.process.status != proc_uninitialized) {
		OT_LOG(LOG_ERR, "Message can be received only from clientApp");
		goto discard_msg;
	}

	/* Valid message. Updated CA proc status to initialized */
	man_msg->proc->content.process.status = proc_active;

	/* Response to CA */
	init_msg->msg_hdr.msg_type = COM_TYPE_RESPONSE;
	get_next_operation_id(&init_msg->operation_id);
	init_msg->ret = TEE_SUCCESS;

	add_msg_out_queue_and_notify(man_msg);

	return;

discard_msg:
	free_manager_msg(man_msg);
}

static bool active_sess_in_ta(proc_t ta_proc)
{
	proc_t sess;

	h_table_init_stepper(ta_proc->content.process.links);

	while (1) {
		sess = h_table_step(ta_proc->content.process.links);
		if (!sess)
			return false;

		/* Initialized means that there might be an open session message out */
		if (sess->content.sesLink.status == sess_active ||
		    sess->content.sesLink.status == sess_initialized)
			return true;
	}
}

/*!
 * \brief should_ta_destroy
 * \param ta_proc
 * \return If TA should destroy, return 1.
 * If TA should not destroy, return 0.
 * In case of error (e.g. TA not found in TA directory), return -1
 */
static int should_ta_destroy(proc_t ta_proc)
{
	struct trusted_app_propertie *ta_properties;
	int ret = 0; /* Default action is not to destroy */

	if (ta_dir_watch_lock_mutex())
		return -1; /* Err logged */

	ta_properties = ta_dir_watch_props(&ta_proc->content.process.ta_uuid);
	if (!ta_properties) {
		OT_LOG(LOG_ERR, "TA with requested UUID is not found");
		ret = -1;
		goto unlock;
	}

	/* Keep alive instance is never terminated */
	if (ta_properties->user_config.instanceKeepAlive) {
		ret = 0;
		goto unlock;
	}

	/* TA might be signleton or multi-instance TA,
	 * but if there is no session opens, terminate */
	if (!active_sess_in_ta(ta_proc))
		ret = 1;

unlock:
	ta_dir_watch_unlock_mutex();

	return ret;
}

static void send_close_sess_msg(proc_t ta_sess)
{
	struct manager_msg *man_msg = NULL;

	if (ta_sess->p_type != proc_t_session ||
	    ta_sess->content.sesLink.owner->p_type != proc_t_TA) {
		OT_LOG(LOG_ERR, "Not session or not session in TA process\n");
		return;
	}

	man_msg = calloc(1, sizeof(struct manager_msg));
	if (!man_msg) {
		OT_LOG(LOG_ERR, "Out of memory\n");
		return;
	}

	man_msg->msg = calloc(1, sizeof(struct com_msg_close_session));
	if (!man_msg->msg) {
		OT_LOG(LOG_ERR, "Out of memory\n");
		free(man_msg);
		return;
	}

	man_msg->msg_len = sizeof(struct com_msg_close_session);
	man_msg->proc = ta_sess->content.sesLink.owner;

	((struct com_msg_close_session *)man_msg->msg)->msg_hdr.msg_name =
			COM_MSG_NAME_CLOSE_SESSION;
	((struct com_msg_close_session *)man_msg->msg)->msg_hdr.msg_type = COM_TYPE_QUERY;
	((struct com_msg_close_session *)man_msg->msg)->sess_ctx =
			ta_sess->content.sesLink.sess_ctx;

	((struct com_msg_close_session *)man_msg->msg)->should_ta_destroy =
	    should_ta_destroy(ta_sess->content.sesLink.owner);
	if (((struct com_msg_close_session *)man_msg->msg)->should_ta_destroy == -1) {
		free_manager_msg(man_msg);
		return;
	}

	add_msg_out_queue_and_notify(man_msg);
}

static void remove_session_between(proc_t owner, proc_t to, uint64_t sess_id)
{
	free(h_table_remove(owner->content.process.links, (unsigned char *)(&sess_id),
			    sizeof(uint64_t)));

	free(h_table_remove(to->content.process.links, (unsigned char *)(&sess_id),
			    sizeof(uint64_t)));
}

static void open_session_response(struct manager_msg *man_msg)
{
	struct com_msg_open_session *open_resp_msg = man_msg->msg;
	proc_t ta_session;
	proc_t resp_msg_to_proc;

	/* Message can be received only from trusted App! */
	if (man_msg->proc->p_type != proc_t_TA) {
		OT_LOG(LOG_ERR, "Invalid sender");
		goto ignore_msg;

	}

	if (!(man_msg->proc->content.process.status == proc_active ||
	      man_msg->proc->content.process.status == proc_initialized)) {
		OT_LOG(LOG_ERR, "Invalid sender status");
		goto ignore_msg;
	}

	/* Sender is TA. Lets get TA session from TA proc session links */
	ta_session =
	    h_table_get(man_msg->proc->content.process.links,
			(unsigned char *)(&open_resp_msg->msg_hdr.sess_id), sizeof(uint64_t));
	if (!ta_session) {
		OT_LOG(LOG_ERR, "Invalid session ID");
		goto ignore_msg;
	}

	if (ta_session->content.sesLink.status != sess_initialized ||
	    ta_session->content.sesLink.to->content.sesLink.status != sess_initialized) {
		OT_LOG(LOG_ERR, "Invalid session status");
		goto ignore_msg;
	}

	/* Session might be removed, because return code is not TEE_SUCCESS. Therefore
	 * we need save "address" to where this message to send */
	resp_msg_to_proc = ta_session->content.sesLink.to->content.sesLink.owner;

	/* Check received message answer and proceed according to that */

	if (open_resp_msg->return_code_open_session != TEE_SUCCESS) {

		/* Although session will be removed, it is marked as closed. It is done,
		 * because then we can call send_close_sess_msg() and it can decide if TA needs
		 * to be terminated. */
		ta_session->content.sesLink.status = sess_closed;
		ta_session->content.sesLink.to->content.sesLink.status = sess_closed;

		send_close_sess_msg(ta_session);

		remove_session_between(ta_session->content.sesLink.owner,
				       ta_session->content.sesLink.to->content.sesLink.owner,
				       open_resp_msg->msg_hdr.sess_id);

	} else {

		/* Update session status to active */
		ta_session->content.sesLink.status = sess_active;
		ta_session->content.sesLink.to->content.sesLink.status = sess_active;

		/* Proc can be set active if TA create entry point func is executed */
		ta_session->content.sesLink.owner->content.process.status = proc_active;

		/* Take session ctx pointer */
		ta_session->content.sesLink.sess_ctx = open_resp_msg->sess_ctx;
	}

	/* Send message to its initial sender
	 * man_msg->proc will be used as message "address" */
	man_msg->proc = resp_msg_to_proc;
	add_msg_out_queue_and_notify(man_msg);

	return;

ignore_msg:
	free_manager_msg(man_msg);
}

static void get_next_sess_id(uint64_t *new_id)
{
	/* TODO: Rare, but after ~92 quadrillion session this will overflow */
	static uint64_t next_sess_id;

	*new_id = next_sess_id++;
}

static int create_uninitialized_ta_proc(proc_t *new_ta, TEE_UUID *ta_uuid)
{
	*new_ta = (proc_t)calloc(1, sizeof(struct __proc));
	if (!*new_ta) {
		OT_LOG(LOG_ERR, "Out of memory");
		return 1;
	}

	h_table_create(&(*new_ta)->content.process.links, TA_SESS_COUNT_EST);
	if (!(*new_ta)->content.process.links) {
		OT_LOG(LOG_ERR, "Out of memory");
		free(*new_ta);
		return 1;
	}

	(*new_ta)->p_type = proc_t_TA;
	(*new_ta)->content.process.status = proc_uninitialized;
	memcpy(&(*new_ta)->content.process.ta_uuid, ta_uuid, sizeof(TEE_UUID));

	return 0;
}

static proc_t get_ta_by_uuid(TEE_UUID *uuid)
{
	proc_t ta = NULL;

	h_table_init_stepper(trustedApps);

	while (1) {
		ta = (proc_t)h_table_step(trustedApps);
		if (!ta)
			break; /* TA is not running, return NULL */

		if (!bcmp(&ta->content.process.ta_uuid, uuid, sizeof(TEE_UUID)))
			break; /* TA running, return proc ptr */
	}

	return ta;
}

static int comm_launcher_to_launch_ta(struct manager_msg *man_msg, int *new_ta_fd,
				      pid_t *new_ta_pid)
{
	struct com_msg_ta_created *recv_created_msg = NULL;

	/* Initialize new ta pid -1 in purpose of error handling in this function */
	*new_ta_pid = -1;

	/* Communicating directly to launcher */
	if (com_send_msg(launcher_fd, man_msg->msg, man_msg->msg_len) != man_msg->msg_len) {
		/* TODO: Why socket failing */
		OT_LOG(LOG_ERR, "Communication proble to launcher");
		goto err;
	}

	/* Note: After this point we might receive signal SIGCHLD */

	/* Receive launcer msg. In case of error, abort TA initialization */
	if (com_recv_msg(launcher_fd, (void **)(&recv_created_msg), NULL)) {
		/* TODO: Why socket failing */
		OT_LOG(LOG_ERR, "failed to receive ta create message");
		goto err;
	}

	if (recv_created_msg->msg_hdr.msg_name != COM_MSG_NAME_CREATED_TA ||
	    recv_created_msg->msg_hdr.msg_type != COM_TYPE_RESPONSE) {
		OT_LOG(LOG_ERR, "Invalid message\n");
		goto err;
	}

	if (recv_created_msg->pid == -1) {
		/* If PID is -1, fork/clone failed! */
		OT_LOG(LOG_ERR, "Problem in TA launching");
		goto err;
	}

	*new_ta_pid = recv_created_msg->pid;

	/* launcher is forking to new proc and creates sockpair. Other end will be send here. */
	if (recv_fd(launcher_fd, new_ta_fd) == -1) {
		OT_LOG(LOG_ERR, "Error at recv TA fd");
		goto err;
	}

	free(recv_created_msg);
	return 0;

err:
	if (*new_ta_pid != -1)
		kill(*new_ta_pid, SIGKILL);

	free(recv_created_msg);
	gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_GENERIC);
	return 1;
}

static int alloc_and_init_sessLink(struct __proc **sesLink, proc_t owner, proc_t to,
				   uint64_t sess_id)
{
	*sesLink = (proc_t)calloc(1, sizeof(struct __proc));
	if (!*sesLink) {
		OT_LOG(LOG_ERR, "Out of memory");
		return 1;
	}

	(*sesLink)->p_type = proc_t_session;
	(*sesLink)->content.sesLink.status = sess_initialized;
	(*sesLink)->content.sesLink.owner = owner;
	(*sesLink)->content.sesLink.to = to;
	(*sesLink)->content.sesLink.session_id = sess_id;

	return 0;
}

static int add_new_session_to_proc(proc_t owner, proc_t to, uint64_t session_id,
				   proc_t *new_sesLink)
{
	if (alloc_and_init_sessLink(new_sesLink, owner, to, session_id))
		return 1;

	if (h_table_insert(owner->content.process.links, (unsigned char *)&session_id,
			   sizeof(uint64_t), *new_sesLink)) {
		OT_LOG(LOG_ERR, "Out of memory");
		return 1;
	}

	return 0;
}

static int create_sesLink(proc_t owner, proc_t to, uint64_t sess_id)
{
	proc_t new_owner_ses = NULL;
	proc_t new_to_ses = NULL;

	/* Following code will be generating two session link and cross linking sessions to gether
	 */

	if (add_new_session_to_proc(owner, NULL, sess_id, &new_owner_ses))
		return 1;

	if (add_new_session_to_proc(to, new_owner_ses, sess_id, &new_to_ses)) {
		free_sess(new_owner_ses);
		return 1;
	}

	/* It is initialized to waiting open session message, because immediately
	 * after this is open session message send out */
	new_owner_ses->content.sesLink.waiting_response_msg = WAIT_OPEN_SESSION_MSG;
	new_owner_ses->content.sesLink.to = new_to_ses;

	return 0;
}

/*!
 * \brief connect_to_ta
 * Function sole purpose is to decide if there is need to connect some existing TA or launch
 * new TA.
 * \param man_msg
 * \param conn_ta If function returns NULL -> Launch new TA or else connect to this proc
 * \param ta_uuid
 * \return true on success. False if can't connect ANY TA. See also explanation about conn_ta param
 */
static bool does_ta_exist_and_connectable(struct manager_msg *man_msg,
					  proc_t *conn_ta, TEE_UUID *ta_uuid)
{
	bool ret = true;
	struct trusted_app_propertie *ta_propertie;

	*conn_ta = get_ta_by_uuid(ta_uuid);
	if (*conn_ta && (*conn_ta)->p_type != proc_t_TA) {
		OT_LOG(LOG_ERR, "Something is wrong");
		gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_GENERIC);
		return false;
	}

	if (ta_dir_watch_lock_mutex()) {
		gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_GENERIC);
		return false; /* Err msg logged */
	}
	/* If trusted application exists at TA folder -> get it properties. */
	ta_propertie = ta_dir_watch_props(ta_uuid);
	if (!ta_propertie) {
		OT_LOG(LOG_ERR, "TA with requested UUID is not found");
		gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_BAD_PARAMETERS);
		ret = false;
		goto ret;
	}

	if (*conn_ta && ((*conn_ta)->content.process.status == proc_active ||
			 (*conn_ta)->content.process.status == proc_initialized ||
			 (*conn_ta)->content.process.status == proc_uninitialized) &&
	    ta_propertie->user_config.singletonInstance &&
	    !ta_propertie->user_config.multiSession) {
		/* Singleton and running and not supporting multi session! */
		gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_ACCESS_CONFLICT);
		ret = false;
		goto ret;
	}

	memcpy(&((struct com_msg_open_session *)man_msg->msg)->ta_so_name,
	       ta_propertie->ta_so_name, TA_MAX_FILE_NAME);

	if (*conn_ta && ta_propertie->user_config.singletonInstance) {

		if ((*conn_ta)->content.process.status == proc_active) {
			ret = true;
			goto ret; /* Singleton and TA running */
		}

		/* TA is being to initialized and we can not predict if initialization will
		 * success --> do not accept new open session msg */
		if ((*conn_ta)->content.process.status == proc_initialized ||
		    (*conn_ta)->content.process.status == proc_uninitialized) {
			gen_err_msg_and_add_to_out(man_msg,
						   TEE_ORIGIN_TEE, TEE_ERROR_ACCESS_CONFLICT);
			ret = false;
			goto ret;
		}

		/* End up here: TA is disconnected -> new TA will be launched */
	}

	if (*conn_ta && ta_propertie->user_config.instanceKeepAlive) {

		if ((*conn_ta)->content.process.status == proc_active) {
			ret = true;
			goto ret; /* Keep alive and TA running */
		}

		if ((*conn_ta)->content.process.status == proc_initialized ||
		    (*conn_ta)->content.process.status == proc_uninitialized) {
			gen_err_msg_and_add_to_out(man_msg,
						   TEE_ORIGIN_TEE, TEE_ERROR_ACCESS_CONFLICT);
			ret = false;
			goto ret;
		}

		/* End up here: TA is disconnected -> new TA will be launched */
	}

	/* If none of them were true, basic action is launch new TA */
	*conn_ta = NULL;

ret:
	ta_dir_watch_unlock_mutex();
	return ret;
}

static int launch_and_init_ta(struct manager_msg *man_msg, TEE_UUID *ta_uuid, proc_t *new_ta_proc)
{
	pid_t new_ta_pid = 0; /* Zero for compiler warning */

	/* Init return values */
	*new_ta_proc = NULL;

	/* Connection to new TA -> TA will be created */
	if (create_uninitialized_ta_proc(new_ta_proc, ta_uuid)) {
		/* Err logged and just send err to sender*/
		gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_GENERIC);
		return 1;
	}

	/* Launch new TA */
	if (comm_launcher_to_launch_ta(man_msg, &(((*new_ta_proc)->sockfd)),
				       &((*new_ta_proc)->content.process.pid))) {
		free(*new_ta_proc);
		return 1; /* Err logged and send */
	}

	/* Note: TA is launched and its init process is on going now on its own proc */

	if (epoll_reg_data((*new_ta_proc)->sockfd, EPOLLIN, *new_ta_proc)) {
		OT_LOG(LOG_ERR, "Epoll reg error");
		goto err_1;
	}

	if (h_table_insert(trustedApps, (unsigned char *)&((*new_ta_proc)->content.process.pid),
			   sizeof(pid_t), *new_ta_proc)) {
		OT_LOG(LOG_ERR, "out of memory");
		goto err_2;
	}

	/* TA initialization is going on */
	(*new_ta_proc)->content.process.status = proc_initialized;

	return 0;

err_2:
	epoll_unreg((*new_ta_proc)->sockfd);
err_1:
	kill(new_ta_pid, SIGKILL);
	free_proc(*new_ta_proc);
	*new_ta_proc = NULL;
	gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_GENERIC);

	return 1;
}

static void open_session_query(struct manager_msg *man_msg)
{
	proc_t new_ta = NULL;
	proc_t conn_ta = NULL;
	uint64_t new_session_id;
	struct com_msg_open_session *open_msg = man_msg->msg;

	/* Generate new session ID */
	get_next_sess_id(&new_session_id);

	/* SessID is needed when message is sent back from TA */
	open_msg->msg_hdr.sess_id = new_session_id;

	/* Note: Function will return TRUE/FALSE, but this boolean is not about should we
	 * connect to existing TA. If function return FALSE, TA might not be availible in TA
	 * folder or can not lock mutex. If true, TA is availible, but we must check also
	 * conn_ta -value:
	 * If conn_ta is NULL, launch new ta and connect to that
	 * If conn_ta NOT NULL, connect to that TA */
	if (!does_ta_exist_and_connectable(man_msg, &conn_ta, &open_msg->uuid))
		return; /* Err logged and send */

	if (conn_ta) {

		if (create_sesLink(man_msg->proc, conn_ta, new_session_id))
			goto err; /* Err msg logged */

		/* Pass on open session cmd to the TA
		 * Potential Issue: If this message send fails, CA will be waiting forever, because
		 * no error message is not send */
		man_msg->proc = conn_ta;
		add_msg_out_queue_and_notify(man_msg);

	} else {

		/* Launch new TA
		 * NOTE: ta_exist_and_connectable() fill open session ta_so_name-parameter!! */
		if (launch_and_init_ta(man_msg, &open_msg->uuid, &new_ta))
			return; /* Err msg logged and send to sender */

		if (create_sesLink(man_msg->proc, new_ta, new_session_id))
			goto err; /* Err msg logged */

		/* Open session command is already send */

		free_manager_msg(man_msg); /* TA will send response message */
	}

	return;

err:
	epoll_unreg(new_ta->sockfd);
	free_proc(new_ta);
	gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_GENERIC);
}

static void open_session_msg(struct manager_msg *man_msg)
{
	struct com_msg_open_session *open_msg = man_msg->msg;

	if (open_msg->msg_hdr.msg_name != COM_MSG_NAME_OPEN_SESSION) {
		OT_LOG(LOG_ERR, "Handling wrong message");
		goto discard_msg;
	}

	/* Function is only valid for proc FDs */
	if (man_msg->proc->p_type == proc_t_session) {
		OT_LOG(LOG_ERR, "Invalid sender");
		goto discard_msg;
	}

	/* Query and response will handle in their own functions */
	if (open_msg->msg_hdr.msg_type == COM_TYPE_QUERY) {
		open_session_query(man_msg);

	} else if (open_msg->msg_hdr.msg_type == COM_TYPE_RESPONSE) {
		open_session_response(man_msg);

	} else {
		OT_LOG(LOG_ERR, "Unkwon message type");
		goto discard_msg;
	}

	return;

discard_msg:
	free_manager_msg(man_msg);
}

static void invoke_cmd(struct manager_msg *man_msg)
{
	struct com_msg_invoke_cmd *invoke_msg = man_msg->msg;
	proc_t session = NULL;

	/* Valid open session message */
	if (invoke_msg->msg_hdr.msg_name != COM_MSG_NAME_INVOKE_CMD) {
		OT_LOG(LOG_ERR, "Invalid message");
		goto discard_msg;
	}

	/* Function is only valid for proc FDs */
	if (man_msg->proc->p_type == proc_t_session ||
	    man_msg->proc->content.process.status != proc_active) {
		OT_LOG(LOG_ERR, "Invalid sender or senders status");
		goto discard_msg;
	}

	/* REsponse to invoke to command can be received only from TA */
	if (invoke_msg->msg_hdr.msg_type == COM_TYPE_RESPONSE &&
	    man_msg->proc->p_type != proc_t_TA) {
		OT_LOG(LOG_ERR, "Invalid sender");
		goto discard_msg;
	}

	session = h_table_get(man_msg->proc->content.process.links,
			      (unsigned char *)(&invoke_msg->msg_hdr.sess_id), sizeof(uint64_t));
	if (!session || session->p_type != proc_t_session) {
		OT_LOG(LOG_ERR, "Session is not found");
		goto discard_msg;
	}

	if (session->content.sesLink.status == sess_panicked) {
		gen_err_msg_and_add_to_out(man_msg, TEE_ORIGIN_TEE, TEE_ERROR_TARGET_DEAD);
		free_sess(session);
		return;
	}

	if (invoke_msg->msg_hdr.msg_type == COM_TYPE_QUERY) {
		session->content.sesLink.waiting_response_msg = WAIT_INVOKE_MSG;
		invoke_msg->sess_ctx = session->content.sesLink.to->content.sesLink.sess_ctx;

	} else if (invoke_msg->msg_hdr.msg_type == COM_TYPE_RESPONSE) {
		session->content.sesLink.to->content.sesLink.waiting_response_msg = WAIT_NO_MSG_OUT;
	}

	man_msg->proc = session->content.sesLink.to->content.sesLink.owner;
	add_msg_out_queue_and_notify(man_msg);

	return;

discard_msg:
	free_manager_msg(man_msg);
}

static void ca_finalize_context(struct manager_msg *man_msg)
{
	struct com_msg_ca_finalize_constex *fin_con_msg = man_msg->msg;

	/* Valid init message */
	if (fin_con_msg->msg_hdr.msg_name != COM_MSG_NAME_CA_FINALIZ_CONTEXT ||
	    fin_con_msg->msg_hdr.msg_type != COM_TYPE_QUERY) {
		OT_LOG(LOG_ERR, "Parsing wrong message, ignore msg");
		goto ignore_msg;
	}

	/* Message can be received only from client */
	if (man_msg->proc->p_type != proc_t_CA ||
	    man_msg->proc->content.process.status != proc_active) {
		OT_LOG(LOG_ERR, "Invalid sender process or status");
		goto ignore_msg;
	}

	man_msg->proc->content.process.status = proc_disconnected;

	/* No more messages from this CA */
	if (epoll_unreg(man_msg->proc->sockfd))
		OT_LOG(LOG_ERR, "Failed to unreg socket");

	free_proc(man_msg->proc); /* Del client proc */

ignore_msg:
	free_manager_msg(man_msg);
}

static void send_close_resp_msg_to_sender(proc_t to)
{
	struct manager_msg *new_man_msg = NULL;

	new_man_msg = calloc(1, sizeof(struct manager_msg));
	if (!new_man_msg) {
		OT_LOG(LOG_ERR, "Out of memory\n");
		return;
	}

	new_man_msg->msg_len = sizeof(struct com_msg_close_session);
	new_man_msg->msg = calloc(1, new_man_msg->msg_len);
	if (!new_man_msg->msg) {
		OT_LOG(LOG_ERR, "Out of memory\n");
		free(new_man_msg);
		return;
	}

	((struct com_msg_close_session *)new_man_msg->msg)->msg_hdr.msg_name =
			COM_MSG_NAME_CLOSE_SESSION;
	((struct com_msg_close_session *)new_man_msg->msg)->msg_hdr.msg_type =
			COM_TYPE_RESPONSE;

	new_man_msg->proc = to;
	add_msg_out_queue_and_notify(new_man_msg);
}

static void close_session(struct manager_msg *man_msg)
{
	struct com_msg_close_session *close_msg = man_msg->msg;
	proc_t close_ta_proc;
	proc_t sender_proc = man_msg->proc;
	proc_t session;

	/* Valid open session message */
	if (close_msg->msg_hdr.msg_name != COM_MSG_NAME_CLOSE_SESSION ||
	    close_msg->msg_hdr.msg_type != COM_TYPE_QUERY) {
		OT_LOG(LOG_ERR, "Invalid message");
		goto ignore_msg;
	}

	/* Function is only valid for proc FDs */
	if (man_msg->proc->p_type == proc_t_session ||
	    man_msg->proc->content.process.status != proc_active) {
		OT_LOG(LOG_ERR, "Invalid sender or senders status");
		goto ignore_msg;
	}

	session = h_table_get(man_msg->proc->content.process.links,
			      (unsigned char *)(&close_msg->msg_hdr.sess_id), sizeof(uint64_t));
	if (!session || session->p_type != proc_t_session) {
		OT_LOG(LOG_ERR, "Session is not found");
		goto ignore_msg;
	}

	if (session->content.sesLink.status == sess_panicked) {
		free_sess(session);
		return;
	}

	/* Save session TO proc addr, because session might be removed */
	close_ta_proc = session->content.sesLink.to->content.sesLink.owner;

	/* Fill in session ctx before session gets removed */
	close_msg->sess_ctx = session->content.sesLink.to->content.sesLink.sess_ctx;

	/* Remove session */
	remove_session_between(session->content.sesLink.owner,
			       session->content.sesLink.to->content.sesLink.owner,
			       session->content.sesLink.session_id);

	/* Update close message */
	close_msg->should_ta_destroy = should_ta_destroy(close_ta_proc);
	if (close_msg->should_ta_destroy == -1) {
		goto ignore_msg;

	} else if (close_msg->should_ta_destroy == 1) {
		/* Mark TA as disconnected for signaling that this TA will be removed */
		close_ta_proc->content.process.status = proc_disconnected;

		/* No more messages from this TA */
		if (epoll_unreg(close_ta_proc->sockfd))
			OT_LOG(LOG_ERR, "Failed to unreg socket");
	}

	man_msg->proc = close_ta_proc;
	add_msg_out_queue_and_notify(man_msg);

	if (sender_proc->p_type == proc_t_TA)
		send_close_resp_msg_to_sender(sender_proc);

	return;

ignore_msg:
	free_manager_msg(man_msg);
}

static void set_all_ta_sess_status(proc_t ta, enum session_status new_status)
{
	proc_t proc_sess;

	h_table_init_stepper(ta->content.process.links);
	while (1) {
		proc_sess = h_table_step(ta->content.process.links);
		if (proc_sess) {
			proc_sess->content.sesLink.to->content.sesLink.status = new_status;
			proc_sess->content.sesLink.status = new_status;
		} else {
			break;
		}
	}
}

static void rm_all_ta_sessions(proc_t ta)
{
	proc_t proc_sess;

	h_table_init_stepper(ta->content.process.links);
	while (1) {
		proc_sess = h_table_step(ta->content.process.links);
		if (proc_sess) {

			/* Remove session from CA */
			free(h_table_remove(proc_sess->content.sesLink.to->content.process.links,
					    (unsigned char *)&proc_sess->content.sesLink.session_id,
					    sizeof(uint64_t)));

			/* Remove session from TA */
			free(h_table_remove(proc_sess->content.sesLink.owner->content.process.links,
					    (unsigned char *)&proc_sess->content.sesLink.session_id,
					    sizeof(uint64_t)));
		} else {
			break;
		}
	}
}


static TEE_Result unmap_create_entry_exit_value(uint8_t ret)
{
	switch (ret) {
	case 10:
		return TEE_ERROR_GENERIC;
	case 11:
		return TEE_ERROR_ACCESS_DENIED;
	case 12:
		return TEE_ERROR_CANCEL;
	case 13:
		return TEE_ERROR_ACCESS_CONFLICT;
	case 14:
		return TEE_ERROR_EXCESS_DATA;
	case 15:
		return TEE_ERROR_BAD_FORMAT;
	case 16:
		return TEE_ERROR_BAD_PARAMETERS;
	case 17:
		return TEE_ERROR_BAD_STATE;
	case 18:
		return TEE_ERROR_ITEM_NOT_FOUND;
	case 19:
		return TEE_ERROR_NOT_IMPLEMENTED;
	case 20:
		return TEE_ERROR_NOT_SUPPORTED;
	case 21:
		return TEE_ERROR_NO_DATA;
	case 22:
		return TEE_ERROR_OUT_OF_MEMORY;
	case 23:
		return TEE_ERROR_BUSY;
	case 24:
		return TEE_ERROR_COMMUNICATION;
	case 25:
		return TEE_ERROR_SECURITY;
	case 26:
		return TEE_ERROR_SHORT_BUFFER;
	case 27:
		return TEE_PENDING;
	case 28:
		return TEE_ERROR_TIMEOUT;
	case 29:
		return TEE_ERROR_OVERFLOW;
	case 30:
		return TEE_ERROR_TARGET_DEAD;
	case 31:
		return TEE_ERROR_STORAGE_NO_SPACE;
	case 32:
		return TEE_ERROR_MAC_INVALID;
	case 33:
		return TEE_ERROR_SIGNATURE_INVALID;
	case 34:
		return TEE_ERROR_TIME_NOT_SET;
	case 35:
		return TEE_ERROR_TIME_NEEDS_RESET;
	default:
		OT_LOG(LOG_ERR, "Unknow exit state")
		break;
	}

	return TEE_ERROR_GENERIC; /* Should not end up here */
}

static void gen_man_and_err_and_send(proc_t ta_sess, uint8_t exit_status, int waited_msg)
{
	struct manager_msg *man_msg = NULL;

	man_msg = calloc(1, sizeof(struct manager_msg));
	if (!man_msg) {
		OT_LOG(LOG_ERR, "Out of memory\n");
		return;
	}

	man_msg->msg = calloc(1, sizeof(struct com_msg_error));
	if (!man_msg->msg) {
		OT_LOG(LOG_ERR, "Out of memory\n");
		free(man_msg);
		return;
	}

	man_msg->msg_len = sizeof(struct com_msg_error);
	man_msg->proc = ta_sess->content.sesLink.to->content.sesLink.owner;

	((struct com_msg_error *)man_msg->msg)->msg_hdr.msg_name = COM_MSG_NAME_ERROR;

	if (exit_status) {
		((struct com_msg_error *)man_msg->msg)->ret =
				unmap_create_entry_exit_value(exit_status);
		((struct com_msg_error *)man_msg->msg)->ret_origin = TEE_ORIGIN_TRUSTED_APP;

	} else if (waited_msg) {

		if (waited_msg == WAIT_OPEN_SESSION_MSG)
			((struct com_msg_error *)man_msg->msg)->msg_hdr.msg_name =
				COM_MSG_NAME_OPEN_SESSION;

		else if (waited_msg == WAIT_INVOKE_MSG)
			((struct com_msg_error *)man_msg->msg)->msg_hdr.msg_name =
				COM_MSG_NAME_INVOKE_CMD;

		/* If TA exited during the out message, only possibility is panic! */
		((struct com_msg_error *)man_msg->msg)->ret = TEE_ERROR_TARGET_DEAD;
		((struct com_msg_error *)man_msg->msg)->ret_origin = TEE_ORIGIN_TRUSTED_APP;

	} else {
		((struct com_msg_error *)man_msg->msg)->ret = TEE_ERROR_GENERIC;
		((struct com_msg_error *)man_msg->msg)->ret_origin = TEE_ORIGIN_TEE;
	}

	add_msg_out_queue_and_notify(man_msg);
}

static void send_err_to_initialized_sess(proc_t ta, uint8_t exit_status)
{
	proc_t proc_sess;

	h_table_init_stepper(ta->content.process.links);
	while (1) {

		proc_sess = h_table_step(ta->content.process.links);
		if (!proc_sess)
			break;

		if (proc_sess->content.sesLink.status != sess_initialized)
			continue;

		gen_man_and_err_and_send(proc_sess, exit_status, 0);
	}
}

static void send_err_msg_to_waiting_sess(proc_t ta)
{
	proc_t proc_sess;

	h_table_init_stepper(ta->content.process.links);
	while (1) {

		proc_sess = h_table_step(ta->content.process.links);
		if (!proc_sess)
			break;

		if (proc_sess->content.sesLink.to->content.sesLink.waiting_response_msg ==
		    WAIT_NO_MSG_OUT)
			continue;

		proc_sess->content.sesLink.to->content.sesLink.waiting_response_msg =
				WAIT_NO_MSG_OUT;
		gen_man_and_err_and_send(proc_sess, 0,
					 proc_sess->content.sesLink.to->
					 content.sesLink.waiting_response_msg);
	}
}

static void send_err_generic_err_msg(proc_t ta)
{
	proc_t proc_sess;

	h_table_init_stepper(ta->content.process.links);
	while (1) {

		proc_sess = h_table_step(ta->content.process.links);
		if (!proc_sess)
			break;

		gen_man_and_err_and_send(proc_sess, 0, 0);
	}
}

static void ta_status_change(pid_t ta_pid, int status)
{
	proc_t ta = NULL;

	ta = h_table_get(trustedApps, (unsigned char *)&ta_pid, sizeof(pid_t));
	if (!ta || ta->p_type != proc_t_TA) {
		OT_LOG(LOG_ERR, "TA is not found or Something else has changed status");
		return;
	}

	/* Signal caused termination */
	if (WIFSIGNALED(status)) {
		set_all_ta_sess_status(ta, sess_panicked);
		send_err_msg_to_waiting_sess(ta);
		free_proc(ta);
	}

	if (WIFEXITED(status)) {

		if (WEXITSTATUS(status) >= 10 && WEXITSTATUS(status) <= 35) {
			send_err_to_initialized_sess(ta, WEXITSTATUS(status));
			rm_all_ta_sessions(ta);
			free_proc(ta);

		} else if (WEXITSTATUS(status) == TA_EXIT_DESTROY_ENTRY_EXEC) {
			free_proc(ta);

		} else if (WEXITSTATUS(status) == TA_EXIT_PANICKED) {
			set_all_ta_sess_status(ta, sess_panicked);
			send_err_msg_to_waiting_sess(ta);
			free_proc(ta);

		} else if (WEXITSTATUS(status) == TA_EXIT_LAUNCH_FAILED) {
			send_err_generic_err_msg(ta);
			rm_all_ta_sessions(ta);
			free_proc(ta);

		} else if (WEXITSTATUS(status) == TA_EXIT_FIRST_OPEN_SESS_FAILED) {
			send_err_generic_err_msg(ta);
			rm_all_ta_sessions(ta);
			free_proc(ta);

		} else {
			OT_LOG(LOG_ERR, "Unknow exit status, handeled as panic!");
			set_all_ta_sess_status(ta, sess_panicked);
			send_err_msg_to_waiting_sess(ta);
			free_proc(ta);
		}
	}
}

static void proc_changed_state(struct manager_msg *man_msg)
{
	pid_t changed_proc_pid;
	int status;

	free_manager_msg(man_msg); /* No information */

	/* wait for children, to reap the zombies */
	while (1) {

		changed_proc_pid = waitpid(-1, &status, WNOHANG);
		if (!changed_proc_pid)
			break;

		/*
		if (changed_proc_pid == launcher_pid) {
			launcher_status_change(status);
			continue;
		}
		*/

		if (WIFCONTINUED(status)) {
			OT_LOG(LOG_ERR, "Recv continue sig"); /* No action needed */
			continue;
		}

		/* Signal caused stop. Note. No action, just logging */
		if (WIFSTOPPED(status)) {
			OT_LOG(LOG_ERR, "recvstop signal");
			continue;
		}

		/* Note: If proc status is not continued or stopped, it is terminated. So no
		 * more socket communication to that process. Close sockets and change status */

		ta_status_change(changed_proc_pid, status);
	}
}

static void send_close_msg_to_all_sessions(proc_t ca_proc)
{
	proc_t ca_sess;

	h_table_init_stepper(ca_proc->content.process.links);
	while (1) {

		ca_sess = h_table_step(ca_proc->content.process.links);
		if (!ca_sess)
			break;

		send_close_sess_msg(ca_sess->content.sesLink.to);
	}
}

static void term_proc_by_fd_err(proc_t proc)
{
	if (proc->p_type == proc_t_CA) {
		send_close_msg_to_all_sessions(proc);
		free_proc(proc);

	} else if (proc->p_type == proc_t_TA) {

		/* TA socket dead. Kill TA and then it will generate SIGCHLD */
		if (kill(proc->content.process.pid, SIGKILL)) {

			if (errno != ESRCH)
				OT_LOG(LOG_ERR, "Failed to send signal")
		}
	}
}

static void fd_error(struct manager_msg *man_msg)
{
	struct com_msg_fd_err *fd_err_msg = man_msg->msg;

	/* TODO: We can send an error message in following error case: EDQUOT ENOSPC EFBIG EFAULT */

	switch (fd_err_msg->err_no) {
	case 0: /* If err_no is zero, EPOLLERR or EPOLLHUP generated this message! */
	case EINVAL:
	case EPIPE:
	case EBADF:
	case EIO:
	case EDQUOT:
	case ENOSPC:
	case EFBIG:
	case EFAULT:
		term_proc_by_fd_err(fd_err_msg->proc_ptr);
		break;

	case EAGAIN: /* EWOULDBLOCK */
	case EINTR:
	case EDESTADDRREQ:
	case EISDIR:
		OT_LOG(LOG_DEBUG, "Logging fd error. No action: %d", fd_err_msg->err_no)
		break;
	default:
		OT_LOG(LOG_DEBUG, "Logging fd error: Unknown errno")
		break;
	}

	free_manager_msg(man_msg); /* No information */
}

static void ta_rem_from_dir(struct manager_msg *man_msg)
{
	struct com_msg_ta_rem_from_dir *rem_ta_msg = man_msg->msg;
	proc_t ta;

	h_table_init_stepper(trustedApps);

	while (1) {
		ta = h_table_step(trustedApps);
		if (!ta)
			break;

		if (!bcmp(&rem_ta_msg->uuid, &ta->content.process.ta_uuid, sizeof(TEE_UUID))) {
			set_all_ta_sess_status(ta, sess_panicked);
			rm_all_ta_sessions(ta);
			free_proc(ta);
			break;
		}
	}

	free_manager_msg(man_msg);
}

static void request_cancel(struct manager_msg *man_msg)
{
	struct com_msg_request_cancellation *cancel_msg = man_msg->msg;
	struct manager_msg *new_man_msg = NULL;
	proc_t ca_sess;

	if (cancel_msg->msg_hdr.msg_name != COM_MSG_NAME_REQUEST_CANCEL ||
	    cancel_msg->msg_hdr.msg_type == COM_TYPE_QUERY) {
		OT_LOG(LOG_ERR, "Handling wrong message");
		goto discard_msg;
	}

	/* Function is only valid for proc CAs */
	if (man_msg->proc->p_type == proc_t_CA) {
		OT_LOG(LOG_ERR, "Invalid sender");
		goto discard_msg;
	}

	h_table_init_stepper(man_msg->proc->content.process.links);

	while (1) {
		ca_sess = h_table_step(man_msg->proc->content.process.links);
		if (!ca_sess)
			break;

		if (ca_sess->content.sesLink.status == sess_panicked ||
		    ca_sess->content.sesLink.waiting_response_msg == WAIT_NO_MSG_OUT)
			continue;

		new_man_msg = calloc(1, sizeof(struct manager_msg));
		if (!man_msg) {
			OT_LOG(LOG_ERR, "Out of memory\n");
			continue;
		}

		new_man_msg->msg = calloc(1, sizeof(struct com_msg_request_cancellation));
		if (!new_man_msg->msg) {
			OT_LOG(LOG_ERR, "Out of memory\n");
			free(new_man_msg);
			continue;
		}

		new_man_msg->msg_len = sizeof(struct com_msg_request_cancellation);
		new_man_msg->proc = ca_sess->content.sesLink.to->content.sesLink.owner;

		memcpy(new_man_msg->msg, man_msg->msg, sizeof(struct com_msg_request_cancellation));

		add_msg_out_queue_and_notify(new_man_msg);
	}

discard_msg:
	free_manager_msg(man_msg);
}


void *logic_thread_mainloop(void *arg)
{
	arg = arg; /* ignored */
	struct manager_msg *handled_msg;
	uint8_t com_msg_name;

	while (1) {

		if (pthread_mutex_lock(&todo_queue_mutex)) {
			OT_LOG(LOG_ERR, "Failed to lock the mutex");
			continue;
		}

		/* Wait for message */
		while (list_is_empty(&todo_queue.list)) {
			if (pthread_cond_wait(&todo_queue_cond, &todo_queue_mutex)) {
				OT_LOG(LOG_ERR, "Failed to wait for condition");
				continue;
			}
		}

		/* Queue is FIFO and therefore get just fist message */
		handled_msg = LIST_ENTRY(todo_queue.list.next, struct manager_msg, list);
		list_unlink(&handled_msg->list);

		if (pthread_mutex_unlock(&todo_queue_mutex)) {
			OT_LOG(LOG_ERR, "Failed to lock the mutex");
			continue;
		}

		/* Manager message queue is released */

		/* Exctract messagese part */
		if (com_get_msg_name(handled_msg->msg, &com_msg_name)) {
			OT_LOG(LOG_ERR, "Error with message, discarding");
			free_manager_msg(handled_msg);
			continue;
		}

		if (com_msg_name == COM_MSG_NAME_PROC_STATUS_CHANGE ||
		    com_msg_name == COM_MSG_NAME_FD_ERR ||
		    com_msg_name == COM_MSG_NAME_TA_REM_FROM_DIR) {

			/* Empty: No need sender details */

		} else {

		    if (!handled_msg->proc) {
			OT_LOG(LOG_ERR, "Error with sender details");
			free_manager_msg(handled_msg);
			continue;
		    }
		}

		switch (com_msg_name) {
		case COM_MSG_NAME_PROC_STATUS_CHANGE:
			proc_changed_state(handled_msg);
			break;

		case COM_MSG_NAME_FD_ERR:
			fd_error(handled_msg);
			break;

		case COM_MSG_NAME_CA_INIT_CONTEXT:
			ca_init_context(handled_msg);
			break;

		case COM_MSG_NAME_OPEN_SESSION:
			open_session_msg(handled_msg);
			break;

		case COM_MSG_NAME_INVOKE_CMD:
			invoke_cmd(handled_msg);
			break;

		case COM_MSG_NAME_CLOSE_SESSION:
			close_session(handled_msg);
			break;

		case COM_MSG_NAME_CA_FINALIZ_CONTEXT:
			ca_finalize_context(handled_msg);
			break;

		case COM_MSG_NAME_TA_REM_FROM_DIR:
			ta_rem_from_dir(handled_msg);
			break;

		case COM_MSG_NAME_REQUEST_CANCEL:
			request_cancel(handled_msg);
			break;

		default:
			/* Just logging an error and message will be ignored */
			OT_LOG(LOG_ERR, "Unknow message, ignore");
			free_manager_msg(handled_msg);
		}
	}

	/* should never reach here */
	OT_LOG(LOG_ERR, "Logic thread is about to exit");
	exit(EXIT_FAILURE); /* TODO: Replace this function with kill tee gracefully */
	return NULL;
}
