/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>

#include <sys/param.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>

#include <crm/crm.h>

#include <clplumbing/ipc.h>
#include <clplumbing/Gmain_timeout.h>
#include <clplumbing/cl_log.h>
#include <clplumbing/cl_signal.h>
#include <clplumbing/lsb_exitcodes.h>
#include <clplumbing/uids.h>
#include <clplumbing/realtime.h>
#include <clplumbing/GSource.h>
#include <clplumbing/cl_poll.h>

#include <ha_msg.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/cluster.h>

/* frees msg */
gboolean 
send_ipc_message(IPC_Channel *ipc_client, HA_Message *msg)
{
	gboolean all_is_good = TRUE;
	int fail_level = LOG_WARNING;

	if(ipc_client != NULL && ipc_client->conntype == IPC_CLIENT) {
		fail_level = LOG_ERR;
	}

	if (msg == NULL) {
		crm_err("cant send NULL message");
		all_is_good = FALSE;

	} else if (ipc_client == NULL) {
		crm_err("cant send message without an IPC Channel");
		all_is_good = FALSE;

	} else if(ipc_client->ops->get_chan_status(ipc_client) != IPC_CONNECT) {
		do_crm_log(fail_level, "IPC Channel to %d is not connected",
			      (int)ipc_client->farside_pid);
		all_is_good = FALSE;
	}

	if(all_is_good && msg2ipcchan(msg, ipc_client) != HA_OK) {
		do_crm_log(fail_level, "Could not send IPC message to %d",
			(int)ipc_client->farside_pid);
		all_is_good = FALSE;

		if(ipc_client->ops->get_chan_status(ipc_client) != IPC_CONNECT) {
			do_crm_log(fail_level,
				      "IPC Channel to %d is no longer connected",
				      (int)ipc_client->farside_pid);

		} else if(ipc_client->conntype == IPC_CLIENT) {
			if(ipc_client->send_queue->current_qlen >= ipc_client->send_queue->max_qlen) {
				crm_err("Send queue to %d (size=%d) full.",
					ipc_client->farside_pid,
					(int)ipc_client->send_queue->max_qlen);
			}
		}
	}
/* 	ipc_client->ops->resume_io(ipc_client); */
	
	crm_log_message_adv(all_is_good?LOG_MSG:LOG_WARNING,"IPC[outbound]",msg);
	
	return all_is_good;
}

void
default_ipc_connection_destroy(gpointer user_data)
{
	return;
}

int
init_server_ipc_comms(
	char *channel_name,
	gboolean (*channel_client_connect)(IPC_Channel *newclient,gpointer user_data),
	void (*channel_connection_destroy)(gpointer user_data))
{
	/* the clients wait channel is the other source of events.
	 * This source delivers the clients connection events.
	 * listen to this source at a relatively lower priority.
	 */
    
	char    commpath[SOCKET_LEN];
	IPC_WaitConnection *wait_ch;
	
	sprintf(commpath, CRM_SOCK_DIR "/%s", channel_name);

	wait_ch = wait_channel_init(commpath);

	if (wait_ch == NULL) {
		return 1;
	}
	
	G_main_add_IPC_WaitConnection(
		G_PRIORITY_LOW, wait_ch, NULL, FALSE,
		channel_client_connect, channel_name,
		channel_connection_destroy);

	crm_debug_3("Listening on: %s", commpath);

	return 0;
}

GCHSource*
init_client_ipc_comms(const char *channel_name,
		      gboolean (*dispatch)(
			      IPC_Channel* source_data, gpointer user_data),
		      void *client_data, IPC_Channel **ch)
{
	IPC_Channel *a_ch = NULL;
	GCHSource *the_source = NULL;
	void *callback_data = client_data;

	a_ch = init_client_ipc_comms_nodispatch(channel_name);
	if(ch != NULL) {
		*ch = a_ch;
		if(callback_data == NULL) {
			callback_data = a_ch;
		}
	}

	if(a_ch == NULL) {
		crm_warn("Setup of client connection failed,"
			 " not adding channel to mainloop");
		
		return NULL;
	}

	if(dispatch == NULL) {
		crm_warn("No dispatch method specified..."
			 "maybe you meant init_client_ipc_comms_nodispatch()?");
	} else {
		crm_debug_3("Adding dispatch method to channel");

		the_source = G_main_add_IPC_Channel(
			G_PRIORITY_HIGH, a_ch, FALSE, dispatch, callback_data, 
			default_ipc_connection_destroy);
	}
	
	return the_source;
}

IPC_Channel *
init_client_ipc_comms_nodispatch(const char *channel_name)
{
	IPC_Channel *ch;
	GHashTable  *attrs;
	static char  path[] = IPC_PATH_ATTR;

	char *commpath = NULL;
	int local_socket_len = 2; /* 2 = '/' + '\0' */

	local_socket_len += strlen(channel_name);
	local_socket_len += strlen(CRM_SOCK_DIR);

	crm_malloc0(commpath, local_socket_len);

	sprintf(commpath, CRM_SOCK_DIR "/%s", channel_name);
	commpath[local_socket_len - 1] = '\0';
	crm_debug("Attempting to talk on: %s", commpath);
	
	attrs = g_hash_table_new(g_str_hash,g_str_equal);
	g_hash_table_insert(attrs, path, commpath);

	ch = ipc_channel_constructor(IPC_ANYTYPE, attrs);
	g_hash_table_destroy(attrs);

	if (ch == NULL) {
		crm_err("Could not access channel on: %s", commpath);
		crm_free(commpath);
		return NULL;
		
	} else if (ch->ops->initiate_connection(ch) != IPC_OK) {
		crm_debug("Could not init comms on: %s", commpath);
		ch->ops->destroy(ch);
		crm_free(commpath);
		return NULL;
	}

	ch->ops->set_recv_qlen(ch, 512);
	ch->ops->set_send_qlen(ch, 512);
 	ch->should_send_block = TRUE;

	crm_debug_3("Processing of %s complete", commpath);

	crm_free(commpath);
	return ch;
}

IPC_WaitConnection *
wait_channel_init(char daemonsocket[])
{
	IPC_WaitConnection *wait_ch;
	mode_t mask;
	char path[] = IPC_PATH_ATTR;
	GHashTable * attrs;

	
	attrs = g_hash_table_new(g_str_hash,g_str_equal);
	g_hash_table_insert(attrs, path, daemonsocket);
    
	mask = umask(0);
	wait_ch = ipc_wait_conn_constructor(IPC_ANYTYPE, attrs);
	if (wait_ch == NULL) {
		cl_perror("Can't create wait channel of type %s",
			  IPC_ANYTYPE);
		exit(1);
	}
	mask = umask(mask);
    
	g_hash_table_destroy(attrs);
    
	return wait_ch;
}

longclock_t ipc_call_start = 0;
longclock_t ipc_call_stop = 0;
longclock_t ipc_call_diff = 0;

gboolean
subsystem_msg_dispatch(IPC_Channel *sender, void *user_data)
{
	int lpc = 0;
	HA_Message *msg = NULL;
	ha_msg_input_t *new_input = NULL;
	gboolean all_is_well = TRUE;
	const char *sys_to;
	const char *task;

	while(IPC_ISRCONN(sender)) {
		gboolean process = FALSE;
		if(sender->ops->is_message_pending(sender) == 0) {
			break;
		}

		msg = msgfromIPC_noauth(sender);
		if (msg == NULL) {
			crm_err("No message from %d this time",
				sender->farside_pid);
			continue;
		}

		lpc++;
		new_input = new_ha_msg_input(msg);
		crm_msg_del(msg);
		msg = NULL;
		
		crm_log_message(LOG_MSG, new_input->msg);

		sys_to = cl_get_string(new_input->msg, F_CRM_SYS_TO);
		task   = cl_get_string(new_input->msg, F_CRM_TASK);

		if(safe_str_eq(task, CRM_OP_HELLO)) {
			process = TRUE;

		} else if(sys_to == NULL) {
			crm_err("Value of %s was NULL!!", F_CRM_SYS_TO);
			
		} else if(task == NULL) {
			crm_err("Value of %s was NULL!!", F_CRM_TASK);
			
		} else {
			process = TRUE;
		}

		if(process){
			gboolean (*process_function)
				(HA_Message *msg, crm_data_t *data, IPC_Channel *sender) = NULL;
			process_function = user_data;
#ifdef MSG_LOG
			crm_log_message_adv(
				LOG_MSG, __FUNCTION__, new_input->msg);
#endif
			if(ipc_call_diff_max_ms > 0) {
				ipc_call_start = time_longclock();
			}
			if(FALSE == process_function(
				   new_input->msg, new_input->xml, sender)) {
				crm_warn("Received a message destined for %s"
					 " by mistake", sys_to);
			}
			if(ipc_call_diff_max_ms > 0) {
				unsigned int ipc_call_diff_ms = 0;
				ipc_call_stop = time_longclock();
				ipc_call_diff = sub_longclock(
					ipc_call_stop, ipc_call_start);
				ipc_call_diff_ms = longclockto_ms(
					ipc_call_diff);
				if(ipc_call_diff_ms > ipc_call_diff_max_ms) {
					crm_err("%s took %dms to complete",
						sys_to, ipc_call_diff_ms);
				}
			}
		} else {
#ifdef MSG_LOG
			crm_log_message_adv(
				LOG_ERR, NULL, new_input->msg);
#endif
		}
		
		delete_ha_msg_input(new_input);
		new_input = NULL;

		if(sender->ch_status == IPC_CONNECT) {
			break;
		}
	}

	crm_debug_2("Processed %d messages", lpc);
	if (sender->ch_status != IPC_CONNECT) {
		crm_err("The server %d has left us: Shutting down...NOW",
			sender->farside_pid);

		exit(1); /* shutdown properly later */
		
		return !all_is_well;
	}
	return all_is_well;
}

gboolean
is_ipc_empty(IPC_Channel *ch)
{
	if(ch == NULL) {
		return TRUE;

	} else if(ch->send_queue->current_qlen == 0
		  && ch->recv_queue->current_qlen == 0) {
		return TRUE;
	}	
	return FALSE;
}

void
send_hello_message(IPC_Channel *ipc_client,
		   const char *uuid,
		   const char *client_name,
		   const char *major_version,
		   const char *minor_version)
{
	crm_data_t *hello_node = NULL;
	HA_Message *hello = NULL;
	if (uuid == NULL || strlen(uuid) == 0
	    || client_name == NULL || strlen(client_name) == 0
	    || major_version == NULL || strlen(major_version) == 0
	    || minor_version == NULL || strlen(minor_version) == 0) {
		crm_err("Missing fields, Hello message will not be valid.");
		return;
	}

	hello_node = create_xml_node(NULL, XML_TAG_OPTIONS);
	crm_xml_add(hello_node, "major_version", major_version);
	crm_xml_add(hello_node, "minor_version", minor_version);
	crm_xml_add(hello_node, "client_name",   client_name);
	crm_xml_add(hello_node, "client_uuid",   uuid);

	crm_debug_4("creating hello message");
	hello = create_request(
		CRM_OP_HELLO, hello_node, NULL, NULL, client_name, uuid);

	send_ipc_message(ipc_client, hello);
	crm_debug_4("hello message sent");
	
	free_xml(hello_node);
	crm_msg_del(hello);
}


gboolean
process_hello_message(crm_data_t *hello,
		      char **uuid,
		      char **client_name,
		      char **major_version,
		      char **minor_version)
{
	const char *local_uuid;
	const char *local_client_name;
	const char *local_major_version;
	const char *local_minor_version;

	*uuid = NULL;
	*client_name = NULL;
	*major_version = NULL;
	*minor_version = NULL;

	if(hello == NULL) {
		return FALSE;
	}
	
	local_uuid = crm_element_value(hello, "client_uuid");
	local_client_name = crm_element_value(hello, "client_name");
	local_major_version = crm_element_value(hello, "major_version");
	local_minor_version = crm_element_value(hello, "minor_version");

	if (local_uuid == NULL || strlen(local_uuid) == 0) {
		crm_err("Hello message was not valid (field %s not found)",
		       "uuid");
		return FALSE;

	} else if (local_client_name==NULL || strlen(local_client_name)==0){
		crm_err("Hello message was not valid (field %s not found)",
			"client name");
		return FALSE;

	} else if(local_major_version == NULL
		  || strlen(local_major_version) == 0){
		crm_err("Hello message was not valid (field %s not found)",
			"major version");
		return FALSE;

	} else if (local_minor_version == NULL
		   || strlen(local_minor_version) == 0){
		crm_err("Hello message was not valid (field %s not found)",
			"minor version");
		return FALSE;
	}
    
	*uuid          = crm_strdup(local_uuid);
	*client_name   = crm_strdup(local_client_name);
	*major_version = crm_strdup(local_major_version);
	*minor_version = crm_strdup(local_minor_version);

	crm_debug_3("Hello message ok");
	return TRUE;
}

HA_Message *
create_request_adv(const char *task, crm_data_t *msg_data,
		   const char *host_to,  const char *sys_to,
		   const char *sys_from, const char *uuid_from,
		   const char *origin)
{
	char *true_from = NULL;
	HA_Message *request = NULL;
	char *reference = generateReference(task, sys_from);

	if (uuid_from != NULL) {
		true_from = generate_hash_key(sys_from, uuid_from);
	} else if(sys_from != NULL) {
		true_from = crm_strdup(sys_from);
	} else {
		crm_err("No sys from specified");
	}
	
	/* host_from will get set for us if necessary by CRMd when routed */
	request = ha_msg_new(11);

	ha_msg_add(request, F_CRM_ORIGIN,	origin);
	ha_msg_add(request, F_TYPE,		T_CRM);
	ha_msg_add(request, F_CRM_VERSION,	CRM_FEATURE_SET);
	ha_msg_add(request, F_CRM_MSG_TYPE,     XML_ATTR_REQUEST);
	ha_msg_add(request, XML_ATTR_REFERENCE, reference);
	ha_msg_add(request, F_CRM_TASK,		task);
	ha_msg_add(request, F_CRM_SYS_TO,       sys_to);
	ha_msg_add(request, F_CRM_SYS_FROM,     true_from);

	/* HOSTTO will be ignored if it is to the DC anyway. */
	if(host_to != NULL && strlen(host_to) > 0) {
		ha_msg_add(request, F_CRM_HOST_TO,  host_to);
	}

	if (msg_data != NULL) {
		add_message_xml(request, F_CRM_DATA, msg_data);
	}
	crm_free(reference);
	crm_free(true_from);
	
	return request;
}

ha_msg_input_t *
new_ha_msg_input(const HA_Message *orig) 
{
	ha_msg_input_t *input_copy = NULL;
	crm_malloc0(input_copy, sizeof(ha_msg_input_t));

	input_copy->msg = ha_msg_copy(orig);
	input_copy->xml = get_message_xml(input_copy->msg, F_CRM_DATA);
	return input_copy;
}

ha_msg_input_t *
new_ipc_msg_input(IPC_Message *orig) 
{
	ha_msg_input_t *input_copy = NULL;
	
	crm_malloc0(input_copy, sizeof(ha_msg_input_t));
	input_copy->msg = ipcmsg2hamsg(orig);
	input_copy->xml = get_message_xml(input_copy->msg, F_CRM_DATA);
	return input_copy;
}

void
delete_ha_msg_input(ha_msg_input_t *orig) 
{
	if(orig == NULL) {
		return;
	}
 	crm_msg_del(orig->msg);
	free_xml(orig->xml);
	crm_free(orig);
}

HA_Message *
validate_crm_message(
	HA_Message *msg, const char *sys, const char *uuid, const char *msg_type)
{
	const char *from = NULL;
	const char *to = NULL;
	const char *type = NULL;
	const char *crm_msg_reference = NULL;
	HA_Message *action = NULL;
	const char *true_sys;
	char *local_sys = NULL;
	
	
	if (msg == NULL) {
		return NULL;
	}

	from = cl_get_string(msg, F_CRM_SYS_FROM);
	to   = cl_get_string(msg, F_CRM_SYS_TO);
	type = cl_get_string(msg, F_CRM_MSG_TYPE);
	
	crm_msg_reference = cl_get_string(msg, XML_ATTR_REFERENCE);
	action = msg;
	true_sys = sys;

	if (uuid != NULL) {
		local_sys = generate_hash_key(sys, uuid);
		true_sys = local_sys;
	}

	if (to == NULL) {
		crm_info("No sub-system defined.");
		action = NULL;
	} else if (true_sys != NULL && strcasecmp(to, true_sys) != 0) {
		crm_debug_3("The message is not for this sub-system (%s != %s).",
			  to, true_sys);
		action = NULL;
	}

	crm_free(local_sys);
	
	if (type == NULL) {
		crm_info("No message type defined.");
		return NULL;
		
	} else if (msg_type != NULL && strcasecmp(msg_type, type) != 0) {
		crm_info("Expecting a (%s) message but received a (%s).",
		       msg_type, type);
		action = NULL;
	}

	if (crm_msg_reference == NULL) {
		crm_info("No message crm_msg_reference defined.");
		action = NULL;
	}
/*
 	if(action != NULL) 
		crm_debug_3(
		       "XML is valid and node with message type (%s) found.",
		       type);
	crm_debug_3("Returning node (%s)", crm_element_name(action));
*/
	
	return action;
}

/*
 * This method adds a copy of xml_response_data
 */
HA_Message *
create_reply_adv(HA_Message *original_request,
		 crm_data_t *xml_response_data, const char *origin)
{
	HA_Message *reply = NULL;

	const char *host_from= cl_get_string(original_request, F_CRM_HOST_FROM);
	const char *sys_from = cl_get_string(original_request, F_CRM_SYS_FROM);
	const char *sys_to   = cl_get_string(original_request, F_CRM_SYS_TO);
	const char *type     = cl_get_string(original_request, F_CRM_MSG_TYPE);
	const char *operation= cl_get_string(original_request, F_CRM_TASK);
	const char *crm_msg_reference = cl_get_string(
		original_request, XML_ATTR_REFERENCE);
	
	if (type == NULL) {
		crm_err("Cannot create new_message,"
			" no message type in original message");
		CRM_ASSERT(type != NULL);
		return NULL;
#if 0
	} else if (strcasecmp(XML_ATTR_REQUEST, type) != 0) {
		crm_err("Cannot create new_message,"
			" original message was not a request");
		return NULL;
#endif
	}
	reply = ha_msg_new(10);

	ha_msg_add(reply, F_CRM_ORIGIN,		origin);
	ha_msg_add(reply, F_TYPE,		T_CRM);
	ha_msg_add(reply, F_CRM_VERSION,	CRM_FEATURE_SET);
	ha_msg_add(reply, F_CRM_MSG_TYPE,	XML_ATTR_RESPONSE);
	ha_msg_add(reply, XML_ATTR_REFERENCE,	crm_msg_reference);
	ha_msg_add(reply, F_CRM_TASK,		operation);

	/* since this is a reply, we reverse the from and to */
	ha_msg_add(reply, F_CRM_SYS_TO,		sys_from);
	ha_msg_add(reply, F_CRM_SYS_FROM,	sys_to);
	
	/* HOSTTO will be ignored if it is to the DC anyway. */
	if(host_from != NULL && strlen(host_from) > 0) {
		ha_msg_add(reply, F_CRM_HOST_TO, host_from);
	}

	if (xml_response_data != NULL) {
		add_message_xml(reply, F_CRM_DATA, xml_response_data);
	}

	return reply;
}
