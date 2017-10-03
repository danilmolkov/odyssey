
/*
 * Odissey.
 *
 * Advanced PostgreSQL connection pooler.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

#include <machinarium.h>
#include <shapito.h>

#include "sources/macro.h"
#include "sources/version.h"
#include "sources/error.h"
#include "sources/atomic.h"
#include "sources/list.h"
#include "sources/pid.h"
#include "sources/id.h"
#include "sources/logger.h"
#include "sources/daemon.h"
#include "sources/scheme.h"
#include "sources/scheme_mgr.h"
#include "sources/config.h"
#include "sources/msg.h"
#include "sources/system.h"
#include "sources/server.h"
#include "sources/server_pool.h"
#include "sources/client.h"
#include "sources/client_pool.h"
#include "sources/route_id.h"
#include "sources/route.h"
#include "sources/route_pool.h"
#include "sources/io.h"
#include "sources/instance.h"
#include "sources/router.h"
#include "sources/pooler.h"
#include "sources/relay.h"
#include "sources/frontend.h"
#include "sources/backend.h"
#include "sources/reset.h"
#include "sources/auth.h"
#include "sources/auth_query.h"

static inline int
od_auth_query_do(od_server_t *server, char *query, int len,
                 shapito_password_t *result)
{
	od_instance_t *instance = server->system->instance;
	int rc;
	shapito_stream_t *stream = &server->stream;
	shapito_stream_reset(stream);
	rc = shapito_fe_write_query(stream, query, len);
	if (rc == -1)
		return -1;
	rc = od_write(server->io, stream);
	if (rc == -1) {
		od_error(&instance->logger, "auth_query", server->client, server,
		         "write error: %s",
		         machine_error(server->io));
		return -1;
	}

	/* update server sync state and stats */
	od_server_sync_request(server);
	od_server_stat_request(server);

	/* wait for response */
	int has_result = 0;
	while (1) {
		shapito_stream_reset(stream);
		int rc;
		rc = od_read(server->io, stream, UINT32_MAX);
		if (rc == -1) {
			if (! machine_timedout()) {
				od_error(&instance->logger, "auth_query", server->client, server,
				         "read error: %s",
				         machine_error(server->io));
			}
			return -1;
		}
		int offset = rc;
		char type = stream->start[offset];
		od_debug(&instance->logger, "auth_query", server->client, server,
		         "%c", type);

		switch (type) {
		/* ErrorResponse */
		case 'E':
			od_backend_error(server, "auth_query", stream->start,
			                 shapito_stream_used(stream));
			return -1;
		/* RowDescription */
		case 'T':
			break;
		/* DataRow */
		case 'D':
		{
			if (has_result) {
				return -1;
			}
			char *pos = stream->start;
			uint32_t pos_size = shapito_stream_used(stream);

			/* count */
			uint32_t count;
			rc = shapito_stream_read32(&count, &pos, &pos_size);
			if (shapito_unlikely(rc == -1)) {
				return -1;
			}
			if (count != 2) {
				return -1;
			}

			/* user */
			uint32_t user_len;
			rc = shapito_stream_read32(&user_len, &pos, &pos_size);
			if (shapito_unlikely(rc == -1)) {
				return -1;
			}
			char *user = pos;
			rc = shapito_stream_read(user_len, &pos, &pos_size);
			if (shapito_unlikely(rc == -1)) {
				return -1;
			}
			(void)user;
			(void)user_len;

			/* password */
			uint32_t password_len;
			rc = shapito_stream_read32(&password_len, &pos, &pos_size);
			if (shapito_unlikely(rc == -1)) {
				return -1;
			}
			char *password = pos;
			rc = shapito_stream_read(password_len, &pos, &pos_size);
			if (shapito_unlikely(rc == -1)) {
				return -1;
			}

			result->password_len = password_len;
			result->password = malloc(password_len);
			if (result->password == NULL)
				return -1;
			memcpy(result->password, password, password_len);
			has_result = 1;
			break;
		}
		/* ReadyForQuery */
		case 'Z':
			od_backend_ready(server, "auth_query",
			                 stream->start + offset,
			                 shapito_stream_used(stream) - offset);
			return 0;
		}
	}

	return 0;
}

int od_auth_query(od_system_t *system, od_schemeroute_t *scheme,
                  shapito_password_t *password)
{
	od_instance_t *instance = system->instance;

	/* create internal auth client */
	od_client_t *auth_client;
	auth_client = od_client_allocate();
	if (auth_client == NULL)
		return -1;
	auth_client->system = system;

	od_idmgr_generate(&instance->id_mgr, &auth_client->id, "a");

	/* set auth query route db and user */
	shapito_parameters_add(&auth_client->startup.params, "database", 9,
	                       scheme->auth_query_db,
	                       strlen(scheme->auth_query_db) + 1);

	shapito_parameters_add(&auth_client->startup.params, "user", 5,
	                       scheme->auth_query_user,
	                       strlen(scheme->auth_query_user) + 1);

	shapito_parameter_t *param;
	param = (shapito_parameter_t*)auth_client->startup.params.buf.start;
	auth_client->startup.database = param;

	param = shapito_parameter_next(param);
	auth_client->startup.user = param;

	/* route */
	od_routerstatus_t status;
	status = od_route(auth_client);
	if (status != OD_ROK) {
		od_client_free(auth_client);
		return -1;
	}

	/* attach */
	status = od_router_attach(auth_client);
	if (status != OD_ROK) {
		od_unroute(auth_client);
		od_client_free(auth_client);
		return -1;
	}
	od_server_t *server;
	server = auth_client->server;

	od_debug(&instance->logger, "auth_query", NULL, server,
	         "attached to %s%.*s",
	         server->id.id_prefix, sizeof(server->id.id),
	         server->id.id);

	/* connect to server, if necessary */
	int rc;
	if (server->io == NULL) {
		rc = od_backend_connect(server, "auth_query");
		if (rc == -1) {
			od_router_close_and_unroute(auth_client);
			od_client_free(auth_client);
			return -1;
		}
	}

	/* discard last server configuration */
	od_route_t *route;
	route = server->route;
	if (route->scheme->pool_discard) {
		rc = od_reset_discard(server, NULL);
		if (rc == -1) {
			od_router_close_and_unroute(auth_client);
			od_client_free(auth_client);
			return -1;
		}
	}

	/* execute query */
	rc = od_auth_query_do(server,
	                      scheme->auth_query,
	                      strlen(scheme->auth_query) + 1,
	                      password);
	if (rc == -1) {
		od_router_close_and_unroute(auth_client);
		od_client_free(auth_client);
		return -1;
	}

	/* detach and unroute */
	od_router_detach_and_unroute(auth_client);
	od_client_free(auth_client);
	return 0;
}
