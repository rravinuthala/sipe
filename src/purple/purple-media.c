/**
 * @file purple-media.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2010 SIPE Project <http://sipe.sourceforge.net/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "glib.h"
#include <string.h>

#include "sipe-common.h"

#include "mediamanager.h"
#include "request.h"
#include "agent.h"

#include "sipe-backend.h"
#include "sipe-core.h"
#include "sipe-media.h"

#include "purple-private.h"

typedef struct _sipe_purple_media {
	PurpleMedia *m;
	// Prevent infinite recursion in on_stream_info_cb
	gboolean	in_recursion;
} sipe_purple_media;

static PurpleMediaSessionType sipe_media_to_purple(SipeMediaType type);
static PurpleMediaCandidateType sipe_candidate_type_to_purple(SipeCandidateType type);
static PurpleMediaNetworkProtocol sipe_network_protocol_to_purple(SipeNetworkProtocol proto);
static SipeNetworkProtocol purple_network_protocol_to_sipe(PurpleMediaNetworkProtocol proto);

static void
on_candidates_prepared_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
						  SIPE_UNUSED_PARAMETER gchar *sessionid,
						  SIPE_UNUSED_PARAMETER gchar *participant,
						  sipe_media_call *call)
{
	if (call->candidates_prepared_cb)
		call->candidates_prepared_cb(call);
}

static void
on_state_changed_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
					PurpleMediaState state,
					gchar *sessionid,
					gchar *participant,
					sipe_media_call *call)
{
	printf("sipe_media_state_changed_cb: %d %s %s\n", state, sessionid, participant);
	if (state == PURPLE_MEDIA_STATE_CONNECTED && call->media_connected_cb)
		call->media_connected_cb(call);
}

static void
on_stream_info_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media,
				  PurpleMediaInfoType type,
				  SIPE_UNUSED_PARAMETER gchar *sessionid,
				  SIPE_UNUSED_PARAMETER gchar *participant,
				  gboolean local, sipe_media_call *call)
{
	sipe_purple_media *m = call->media;
	if (m->in_recursion) {
		m->in_recursion = FALSE;
		return;
	}

	if (type == PURPLE_MEDIA_INFO_ACCEPT && call->call_accept_cb)
		call->call_accept_cb(call, local);
	else if (type == PURPLE_MEDIA_INFO_REJECT && call->call_reject_cb)
		call->call_reject_cb(call, local);
	else if (type == PURPLE_MEDIA_INFO_HOLD && call->call_hold_cb) {
		call->call_hold_cb(call, local, TRUE);
		if (!local) {
			m->in_recursion = TRUE;
			purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_HOLD, NULL, NULL, TRUE);
		}
	} else if (type == PURPLE_MEDIA_INFO_UNHOLD && call->call_hold_cb) {
		call->call_hold_cb(call, local, FALSE);
		m->in_recursion = TRUE;
		if (!call->local_on_hold && !call->remote_on_hold) {
			purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_UNHOLD, NULL, NULL, TRUE);
		} else {
			/* Remote side is still on hold, keep local also held to prevent sending 
			 * unnecessary media over network */
			purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_HOLD, NULL, NULL, TRUE);
		}
	} else if (type == PURPLE_MEDIA_INFO_HANGUP && call->call_hangup_cb)
		call->call_hangup_cb(call, local);
}

struct sipe_media *
sipe_backend_media_new(struct sipe_core_public *sipe_public,
		       struct sipe_media_call *call,
		       const gchar *participant,
		       gboolean initiator)
{
	static int i = 1;
	sipe_purple_media *m = g_new0(sipe_purple_media, 1);
	struct sipe_backend_private *purple_private = sipe_public->backend_private;
	PurpleMediaManager *manager = purple_media_manager_get();

	m->m = purple_media_manager_create_media(manager,
						 purple_private->account,
						 "fsrtpconference",
						 participant, initiator);

	if((i%2) == 0)
	{
	printf("\n\nCONNECTING SIGNALS i:%d\n\n",i);
	g_signal_connect(G_OBJECT(m->m), "candidates-prepared",
						G_CALLBACK(on_candidates_prepared_cb), call);
	g_signal_connect(G_OBJECT(m->m), "stream-info",
						G_CALLBACK(on_stream_info_cb), call);
	g_signal_connect(G_OBJECT(m->m), "state-changed",
						G_CALLBACK(on_state_changed_cb), call);
	}
	i++;

	return (struct sipe_media *)m;
}

void
sipe_backend_media_free(sipe_media *media)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	purple_media_manager_remove_media(purple_media_manager_get(), m->m);
	g_free(m);
}

gboolean
sipe_backend_media_add_stream(sipe_media *media, const gchar* participant, const gchar* sess_id,
							  SipeMediaType type, gboolean use_nice,
							  gboolean initiator)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	PurpleMediaSessionType prpl_type = sipe_media_to_purple(type);
	GParameter *params = NULL;
	guint params_cnt = 0;
	gchar *transmitter;

	if (use_nice) {
		transmitter = "nice";
		params_cnt = 2;

		params = g_new0(GParameter, params_cnt);
		params[0].name = "controlling-mode";
		g_value_init(&params[0].value, G_TYPE_BOOLEAN);
		g_value_set_boolean(&params[0].value, initiator);
		params[1].name = "compatibility-mode";
		g_value_init(&params[1].value, G_TYPE_UINT);
		
		/**
		 * !!!TEMPORARY HACK!!!
		 *
		 * This enables successful compilation with libnice < 0.0.12
		 *
		 * This DOES NOT mean that the resulting code will work!!!
		 */
#ifdef NICE_HAS_COMPATIBILITY
		g_value_set_uint(&params[1].value, NICE_COMPATIBILITY_OC2007R2);
#endif
	} else {
		transmitter = "rawudp";
	}

	printf("\nADDING STREAM FOR:%s\n",sess_id);
	return purple_media_add_stream(m->m, sess_id, participant, prpl_type,
								   initiator, transmitter, params_cnt, params);
}

void
sipe_backend_media_add_remote_candidates(sipe_media *media, gchar* participant, GList *candidates)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	purple_media_add_remote_candidates(m->m, "sipe-voice", participant, candidates);
}

void
sipe_backend_media_add_video_remote_candidates(sipe_media *media, gchar* participant, GList *candidates)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	purple_media_add_remote_candidates(m->m, "sipe-video", participant, candidates);
}

gboolean sipe_backend_media_is_initiator(sipe_media *media, gchar *participant)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	return purple_media_is_initiator(m->m, "sipe-voice", participant);
}

sipe_codec *
sipe_backend_codec_new(int id, const char *name, guint clock_rate, SipeMediaType type)
{
	printf("\nMedia type:%d\n",sipe_media_to_purple(type));
	return (sipe_codec *)purple_media_codec_new(id, name, sipe_media_to_purple(type), clock_rate);
}

void
sipe_backend_codec_free(sipe_codec *codec)
{
	if (codec)
		g_object_unref(codec);
}

int
sipe_backend_codec_get_id(sipe_codec *codec)
{
	return purple_media_codec_get_id((PurpleMediaCodec *)codec);
}

gchar *
sipe_backend_codec_get_name(sipe_codec *codec)
{
	return purple_media_codec_get_encoding_name((PurpleMediaCodec *)codec);
}

guint
sipe_backend_codec_get_clock_rate(sipe_codec *codec)
{
	return purple_media_codec_get_clock_rate((PurpleMediaCodec *)codec);
}

void
sipe_backend_codec_add_optional_parameter(sipe_codec *codec, const gchar *name, const gchar *value)
{
	purple_media_codec_add_optional_parameter((PurpleMediaCodec *)codec, name, value);
}

GList *
sipe_backend_codec_get_optional_parameters(sipe_codec *codec)
{
	return purple_media_codec_get_optional_parameters((PurpleMediaCodec *)codec);
}

gboolean
sipe_backend_set_remote_codecs(sipe_media_call* call, gchar* participant)
{
	sipe_purple_media    *m = (sipe_purple_media *)call->media;
	GList    *codecs	= call->remote_codecs;
		
	printf("\n\nSETTING VOICE REMOTE CODECS\n\n");
	return purple_media_set_remote_codecs(m->m, "sipe-voice", participant, codecs);
}

gboolean
sipe_backend_set_video_remote_codecs(sipe_media_call* call, gchar* participant)
{
	sipe_purple_media   *vm = (sipe_purple_media *)call->video_media;
	GList *video_codecs	= call->remote_video_codecs;
		
	printf("\n\nSETTING VIDEO REMOTE CODECS\n\n");
	return purple_media_set_remote_codecs(vm->m, "sipe-video", participant, video_codecs);
}

gboolean
sipe_backend_set_send_codec(sipe_media_call* call)
{
	sipe_purple_media    *m = (sipe_purple_media *)call->media;
	GList *codecs           = sipe_backend_get_local_codecs(call);
	PurpleMediaCodec *set_codec   = NULL;
	int a;

	printf("\n\nSETTING SET_CODEC\n\n");
        while (codecs) {
                sipe_codec *c = codecs->data;
                gchar *name = sipe_backend_codec_get_name(c);
		a = strcmp(name,"PCMU");
		if(a == 0)
		{
			printf("\nCODEC NAME:%s\n",name);
			set_codec = (PurpleMediaCodec *)c;
			break;
		}

                g_free(name);
                codecs = codecs->next;
        }
	if(set_codec == NULL)
	{
		printf("\nSET CODEC IS NULL\n");
		return 0;
	}
	else
		printf("\nSET CODEC NOT NULL\n");

	return purple_media_set_send_codec(m->m, "sipe-voice", set_codec);
}
gboolean
sipe_backend_set_video_send_codec(sipe_media_call* call)
{
	sipe_purple_media    *m = (sipe_purple_media *)call->video_media;
	GList *codecs           = sipe_backend_get_video_codecs(call);
	PurpleMediaCodec *set_codec   = NULL;
	int a;

	printf("\n\nSETTING SET_CODEC\n\n");
        while (codecs) {
                sipe_codec *c = codecs->data;
                gchar *name = sipe_backend_codec_get_name(c);
		a = strcmp(name,"H263");
		if(a == 0)
		{
			printf("\nCODEC NAME:%s\n",name);
			set_codec = (PurpleMediaCodec *)c;
			break;
		}

                g_free(name);
                codecs = codecs->next;
        }
	if(set_codec == NULL)
	{
		printf("\nSET CODEC IS NULL\n");
		return 0;
	}
	else
		printf("\nSET CODEC NOT NULL\n");

	return purple_media_set_send_codec(m->m, "sipe-video", set_codec);
}

GList*
sipe_backend_get_local_codecs(sipe_media_call* call)
{
	sipe_purple_media	*m = (sipe_purple_media *)call->media;
	return purple_media_get_codecs(m->m, "sipe-voice");
}

GList*
sipe_backend_get_video_codecs(sipe_media_call* call)
{
	sipe_purple_media	*m = (sipe_purple_media *)call->video_media;
	return purple_media_get_codecs(m->m, "sipe-video");
}

sipe_candidate *
sipe_backend_candidate_new(const gchar *foundation, SipeComponentType component,
						   SipeCandidateType type, SipeNetworkProtocol proto,
						   const gchar *ip, guint port)
{
	return (sipe_candidate *)purple_media_candidate_new(
								foundation,
								component,
								sipe_candidate_type_to_purple(type),
								sipe_network_protocol_to_purple(proto),
								ip,
								port);
}

void
sipe_backend_candidate_free(sipe_candidate *candidate)
{
	if (candidate)
		g_object_unref(candidate);
}

gchar *
sipe_backend_candidate_get_username(sipe_candidate *candidate)
{
	return purple_media_candidate_get_username((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_password(sipe_candidate *candidate)
{
	return purple_media_candidate_get_password((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_foundation(sipe_candidate *candidate)
{
	return purple_media_candidate_get_foundation((PurpleMediaCandidate*)candidate);
}

gchar *
sipe_backend_candidate_get_ip(sipe_candidate *candidate)
{
	return purple_media_candidate_get_ip((PurpleMediaCandidate*)candidate);
}

guint
sipe_backend_candidate_get_port(sipe_candidate *candidate)
{
	return purple_media_candidate_get_port((PurpleMediaCandidate*)candidate);
}

guint32
sipe_backend_candidate_get_priority(sipe_candidate *candidate)
{
	return purple_media_candidate_get_priority((PurpleMediaCandidate*)candidate);
}

void
sipe_backend_candidate_set_priority(sipe_candidate *candidate, guint32 priority)
{
	g_object_set(candidate, "priority", priority, NULL);
}

SipeComponentType
sipe_backend_candidate_get_component_type(sipe_candidate *candidate)
{
	return purple_media_candidate_get_component_id((PurpleMediaCandidate*)candidate);
}

SipeCandidateType
sipe_backend_candidate_get_type(sipe_candidate *candidate)
{
	return purple_media_candidate_get_candidate_type((PurpleMediaCandidate*)candidate);
}

SipeNetworkProtocol
sipe_backend_candidate_get_protocol(sipe_candidate *candidate)
{
	PurpleMediaNetworkProtocol proto =
		purple_media_candidate_get_protocol((PurpleMediaCandidate*)candidate);
	return purple_network_protocol_to_sipe(proto);
}

void
sipe_backend_candidate_set_username_and_pwd(sipe_candidate *candidate,
											const gchar *username,
											const gchar *password)
{
	g_object_set(candidate, "username", username, "password", password, NULL);
}

GList*
sipe_backend_get_local_candidates(sipe_media_call* call, gchar* participant)
{
	sipe_purple_media *m = (sipe_purple_media *)call->media;
	return purple_media_get_local_candidates(m->m, "sipe-voice", participant);
}

GList*
sipe_backend_get_video_candidates(sipe_media_call* call, gchar* participant)
{
	sipe_purple_media *m = (sipe_purple_media *)call->video_media;
	return purple_media_get_local_candidates(m->m, "sipe-video", participant);
}

void
sipe_backend_media_hold(sipe_media* media, gboolean local)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_HOLD, NULL, NULL, local);
}

void
sipe_backend_media_unhold(sipe_media* media, gboolean local)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_UNHOLD, NULL, NULL, local);
}

void
sipe_backend_media_hangup(sipe_media* media, gboolean local)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_HANGUP, NULL, NULL, local);
}

void
sipe_backend_media_reject(sipe_media* media, gboolean local)
{
	sipe_purple_media *m = (sipe_purple_media *)media;
	purple_media_stream_info(m->m, PURPLE_MEDIA_INFO_REJECT, NULL, NULL, local);
}

PurpleMediaSessionType sipe_media_to_purple(SipeMediaType type)
{
	switch (type) {
		case SIPE_MEDIA_AUDIO: return PURPLE_MEDIA_AUDIO;
		case SIPE_MEDIA_VIDEO: return PURPLE_MEDIA_VIDEO;
		default:               return PURPLE_MEDIA_NONE;
	}
}

/*SipeMediaType purple_media_to_sipe(PurpleMediaSessionType type)
{
	switch (type) {
		case PURPLE_MEDIA_AUDIO: return SIPE_MEDIA_AUDIO;
		case PURPLE_MEDIA_VIDEO: return SIPE_MEDIA_VIDEO;
		default:				 return SIPE_MEDIA_AUDIO;
	}
}*/

static PurpleMediaCandidateType
sipe_candidate_type_to_purple(SipeCandidateType type)
{
	switch (type) {
		case SIPE_CANDIDATE_TYPE_HOST:	return PURPLE_MEDIA_CANDIDATE_TYPE_HOST;
		case SIPE_CANDIDATE_TYPE_RELAY:	return PURPLE_MEDIA_CANDIDATE_TYPE_RELAY;
		case SIPE_CANDIDATE_TYPE_SRFLX:	return PURPLE_MEDIA_CANDIDATE_TYPE_SRFLX;
		default:						return PURPLE_MEDIA_CANDIDATE_TYPE_HOST;
	}
}

static PurpleMediaNetworkProtocol
sipe_network_protocol_to_purple(SipeNetworkProtocol proto)
{
	switch (proto) {
		case SIPE_NETWORK_PROTOCOL_TCP:	return PURPLE_MEDIA_NETWORK_PROTOCOL_TCP;
		case SIPE_NETWORK_PROTOCOL_UDP:	return PURPLE_MEDIA_NETWORK_PROTOCOL_UDP;
		default:						return PURPLE_MEDIA_NETWORK_PROTOCOL_TCP;
	}
}

static SipeNetworkProtocol
purple_network_protocol_to_sipe(PurpleMediaNetworkProtocol proto)
{
	switch (proto) {
		case PURPLE_MEDIA_NETWORK_PROTOCOL_TCP: return SIPE_NETWORK_PROTOCOL_TCP;
		case PURPLE_MEDIA_NETWORK_PROTOCOL_UDP: return SIPE_NETWORK_PROTOCOL_UDP;
		default:								return SIPE_NETWORK_PROTOCOL_UDP;
	}
}

/*
void sipe_media_error_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media, gchar* error, SIPE_UNUSED_PARAMETER struct sipe_account_data *sip)
{
	printf("sipe_media_error_cb: %s\n", error);
}

void sipe_media_codecs_changed_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media, gchar* codec, SIPE_UNUSED_PARAMETER struct sipe_account_data *sip)
{
	printf("sipe_media_codecs_changed_cb: %s\n", codec);
}

void sipe_media_level_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media, gchar* sessionid, gchar* participant, gdouble percent, SIPE_UNUSED_PARAMETER struct sipe_account_data *sip)
{
	printf("sipe_media_level_cb: %s %s %f\n", sessionid, participant, percent);
}

void sipe_media_new_candidate_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media, gchar* sessionid, gchar* cname, PurpleMediaCandidate *candidate, SIPE_UNUSED_PARAMETER struct sipe_account_data *sip)
{
	printf("sipe_media_new_candidate_cb: %s cname: %s %s %d\n", sessionid, cname,
			purple_media_candidate_get_ip(candidate),
			purple_media_candidate_get_port(candidate));
}

void sipe_media_state_changed_cb(SIPE_UNUSED_PARAMETER PurpleMedia *media, PurpleMediaState state, gchar* sessionid, gchar* participant, SIPE_UNUSED_PARAMETER struct sipe_account_data *sip)
{
	printf("sipe_media_state_changed_cb: %d %s %s\n", state, sessionid, participant);
}

g_signal_connect(G_OBJECT(media), "error", G_CALLBACK(sipe_media_error_cb), call);
g_signal_connect(G_OBJECT(media), "codecs-changed", G_CALLBACK(sipe_media_codecs_changed_cb), call);
g_signal_connect(G_OBJECT(media), "level", G_CALLBACK(sipe_media_level_cb), call);
g_signal_connect(G_OBJECT(media), "new-candidate", G_CALLBACK(sipe_media_new_candidate_cb), call);
g_signal_connect(G_OBJECT(media), "state-changed", G_CALLBACK(sipe_media_state_changed_cb), call);

*/

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/
