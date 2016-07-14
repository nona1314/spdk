/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <rte_config.h>
#include <rte_lcore.h>

#include "conf.h"
#include "controller.h"
#include "host.h"
#include "nvmf_internal.h"
#include "port.h"
#include "subsystem.h"
#include "transport.h"
#include "spdk/conf.h"
#include "spdk/log.h"

#define PORTNUMSTRLEN 32

static int
spdk_nvmf_parse_nvmf_tgt(void)
{
	struct spdk_conf_section *sp;
	int max_queue_depth;
	int max_conn_per_sess;
	int rc;

	sp = spdk_conf_find_section(NULL, "Nvmf");
	if (sp == NULL) {
		SPDK_ERRLOG("No Nvmf section in configuration file.\n");
		return -1;
	}

	max_queue_depth = spdk_conf_section_get_intval(sp, "MaxQueueDepth");
	if (max_queue_depth < 0) {
		max_queue_depth = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	}

	max_conn_per_sess = spdk_conf_section_get_intval(sp, "MaxConnectionsPerSession");
	if (max_conn_per_sess < 0) {
		max_conn_per_sess = SPDK_NVMF_DEFAULT_MAX_CONNECTIONS_PER_SESSION;
	}

	rc = nvmf_tgt_init(max_queue_depth, max_conn_per_sess);
	return rc;
}

static int
spdk_nvmf_parse_addr(char *listen_addr, char **host, char **port)
{
	int n, len;
	const char *p, *q;

	if (listen_addr == NULL) {
		SPDK_ERRLOG("Invalid listen addr for Fabric Interface (NULL)\n");
		return -1;
	}

	*host = NULL;
	*port = NULL;

	if (listen_addr[0] == '[') {
		/* IPv6 */
		p = strchr(listen_addr + 1, ']');
		if (p == NULL) {
			return -1;
		}
		p++;
		n = p - listen_addr;
		*host = calloc(1, n + 1);
		if (!*host) {
			return -1;
		}
		memcpy(*host, listen_addr, n);
		(*host)[n] = '\0';
		if (p[0] == '\0') {
			*port = calloc(1, PORTNUMSTRLEN);
			if (!*port) {
				free(*host);
				return -1;
			}
			snprintf(*port, PORTNUMSTRLEN, "%d", SPDK_NVMF_DEFAULT_SIN_PORT);
		} else {
			if (p[0] != ':') {
				free(*host);
				return -1;
			}
			q = strchr(listen_addr, '@');
			if (q == NULL) {
				q = listen_addr + strlen(listen_addr);
			}
			len = q - p - 1;

			*port = calloc(1, len + 1);
			if (!*port) {
				free(*host);
				return -1;
			}
			memcpy(*port, p + 1, len);
		}
	} else {
		/* IPv4 */
		p = strchr(listen_addr, ':');
		if (p == NULL) {
			p = listen_addr + strlen(listen_addr);
		}
		n = p - listen_addr;
		*host = calloc(1, n + 1);
		if (!*host) {
			return -1;
		}
		memcpy(*host, listen_addr, n);
		(*host)[n] = '\0';
		if (p[0] == '\0') {
			*port = calloc(1, PORTNUMSTRLEN);
			if (!*port) {
				free(*host);
				return -1;
			}
			snprintf(*port, PORTNUMSTRLEN, "%d", SPDK_NVMF_DEFAULT_SIN_PORT);
		} else {
			if (p[0] != ':') {
				free(*host);
				return -1;
			}
			q = strchr(listen_addr, '@');
			if (q == NULL) {
				q = listen_addr + strlen(listen_addr);
			}

			if (q == p) {
				free(*host);
				return -1;
			}

			len = q - p - 1;
			*port = calloc(1, len + 1);
			if (!*port) {
				free(*host);
				return -1;
			}
			memcpy(*port, p + 1, len);

		}
	}

	return 0;
}

static int
spdk_nvmf_parse_port(struct spdk_conf_section *sp)
{
	struct spdk_nvmf_port		*port;
	struct spdk_nvmf_fabric_intf	*fabric_intf;
	char *transport_name, *listen_addr, *host, *listen_port;
	int i = 0, rc = 0;

	/* Create the Subsystem Port */
	port = spdk_nvmf_port_create(sp->num);
	if (!port) {
		SPDK_ERRLOG("Port create failed\n");
		return -1;
	}

	/* Loop over the listen addresses and add them to the port */
	for (i = 0; ; i++) {
		const struct spdk_nvmf_transport *transport;

		transport_name = spdk_conf_section_get_nmval(sp, "Listen", i, 0);
		if (transport_name == NULL) {
			break;
		}

		transport = spdk_nvmf_transport_get(transport_name);
		if (transport == NULL) {
			SPDK_ERRLOG("Unknown transport type '%s'\n", transport_name);
			return -1;
		}

		listen_addr = spdk_conf_section_get_nmval(sp, "Listen", i, 1);
		if (listen_addr == NULL) {
			SPDK_ERRLOG("Missing address for Listen in Port%d\n", sp->num);
			break;
		}
		rc = spdk_nvmf_parse_addr(listen_addr, &host, &listen_port);
		if (rc < 0) {
			continue;
		}
		fabric_intf = spdk_nvmf_fabric_intf_create(transport, host, listen_port);
		if (!fabric_intf) {
			continue;
		}

		spdk_nvmf_port_add_fabric_intf(port, fabric_intf);
	}

	if (TAILQ_EMPTY(&port->head)) {
		SPDK_ERRLOG("No fabric interface found\n");
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_parse_ports(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Port")) {
			rc = spdk_nvmf_parse_port(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

static int
spdk_nvmf_parse_host(struct spdk_conf_section *sp)
{
	int i;
	const char *mask;
	char **netmasks;
	int num_netmasks;
	struct spdk_nvmf_host *host;


	for (num_netmasks = 0; ; num_netmasks++) {
		mask = spdk_conf_section_get_nval(sp, "Netmask", num_netmasks);
		if (mask == NULL) {
			break;
		}
	}

	if (num_netmasks == 0) {
		return -1;
	}


	netmasks = calloc(num_netmasks, sizeof(char *));
	if (!netmasks) {
		return -1;
	}

	for (i = 0; i < num_netmasks; i++) {
		mask = spdk_conf_section_get_nval(sp, "Netmask", i);
		netmasks[i] = strdup(mask);
		if (!netmasks[i]) {
			free(netmasks);
			return -1;
		}
	}

	host = spdk_nvmf_host_create(sp->num, num_netmasks, netmasks);

	if (!host) {
		free(netmasks);
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_parse_hosts(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Host")) {
			rc = spdk_nvmf_parse_host(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

static int
spdk_nvmf_parse_nvme(void)
{
	struct spdk_conf_section *sp;
	struct nvme_bdf_whitelist *whitelist = NULL;
	const char *val;
	bool claim_all = false;
	bool unbind_from_kernel = false;
	int i = 0;
	int rc;

	sp = spdk_conf_find_section(NULL, "Nvme");
	if (sp == NULL) {
		SPDK_ERRLOG("NVMe device section in config file not found!\n");
		return -1;
	}

	val = spdk_conf_section_get_val(sp, "ClaimAllDevices");
	if (val != NULL) {
		if (!strcmp(val, "Yes")) {
			claim_all = true;
		}
	}

	val = spdk_conf_section_get_val(sp, "UnbindFromKernel");
	if (val != NULL) {
		if (!strcmp(val, "Yes")) {
			unbind_from_kernel = true;
		}
	}

	if (!claim_all) {
		for (i = 0; ; i++) {
			unsigned int domain, bus, dev, func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 0);
			if (val == NULL) {
				break;
			}

			whitelist = realloc(whitelist, sizeof(*whitelist) * (i + 1));

			rc = sscanf(val, "%x:%x:%x.%x", &domain, &bus, &dev, &func);
			if (rc != 4) {
				SPDK_ERRLOG("Invalid format for BDF: %s\n", val);
				free(whitelist);
				return -1;
			}

			whitelist[i].domain = domain;
			whitelist[i].bus = bus;
			whitelist[i].dev = dev;
			whitelist[i].func = func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 1);
			if (val == NULL) {
				SPDK_ERRLOG("BDF section with no device name\n");
				free(whitelist);
				return -1;
			}

			snprintf(whitelist[i].name, MAX_NVME_NAME_LENGTH, "%s", val);
		}

		if (i == 0) {
			SPDK_ERRLOG("No BDF section\n");
			return -1;
		}
	}

	rc = spdk_nvmf_init_nvme(whitelist, i,
				 claim_all, unbind_from_kernel);

	free(whitelist);

	return rc;
}

static int
spdk_nvmf_validate_nqn(const char *nqn)
{
	size_t len;

	len = strlen(nqn);
	if (len > SPDK_NVMF_NQN_MAX_LEN) {
		SPDK_ERRLOG("Invalid NQN \"%s\": length %zu > max %d\n", nqn, len, SPDK_NVMF_NQN_MAX_LEN);
		return -1;
	}

	if (strncasecmp(nqn, "nqn.", 4) != 0) {
		SPDK_ERRLOG("Invalid NQN \"%s\": NQN must begin with \"nqn.\".\n", nqn);
		return -1;
	}

	/* yyyy-mm. */
	if (!(isdigit(nqn[4]) && isdigit(nqn[5]) && isdigit(nqn[6]) && isdigit(nqn[7]) &&
	      nqn[8] == '-' && isdigit(nqn[9]) && isdigit(nqn[10]) && nqn[11] == '.')) {
		SPDK_ERRLOG("Invalid date code in NQN \"%s\"\n", nqn);
		return -1;
	}

	return 0;
}

static int
spdk_nvmf_parse_subsystem(struct spdk_conf_section *sp)
{
	const char *val, *nqn;
	struct spdk_nvmf_subsystem *subsystem;

	const char *port_name, *host_name;
	int port_id, host_id;

	struct spdk_nvmf_ctrlr *nvmf_ctrlr;
	int i, ret;

	nqn = spdk_conf_section_get_val(sp, "NQN");
	if (nqn == NULL) {
		SPDK_ERRLOG("No NQN specified for Subsystem %d\n", sp->num);
		return -1;
	}

	if (spdk_nvmf_validate_nqn(nqn) != 0) {
		return -1;
	}

	subsystem = nvmf_create_subsystem(sp->num, nqn, SPDK_NVMF_SUB_NVME, rte_get_master_lcore());
	if (subsystem == NULL) {
		return -1;
	}

	val = spdk_conf_section_get_val(sp, "Mapping");
	if (val == NULL) {
		SPDK_ERRLOG("No Mapping entry in Subsystem %d\n", sp->num);
		nvmf_delete_subsystem(subsystem);
		return -1;
	}

	for (i = 0; i < MAX_PER_SUBSYSTEM_ACCESS_MAP; i++) {
		val = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		if (val == NULL) {
			break;
		}

		port_name = spdk_conf_section_get_nmval(sp, "Mapping", i, 0);
		host_name = spdk_conf_section_get_nmval(sp, "Mapping", i, 1);
		if (port_name == NULL || host_name == NULL) {
			nvmf_delete_subsystem(subsystem);
			return -1;
		}
		if (strncasecmp(port_name, "Port",
				strlen("Port")) != 0
		    || sscanf(port_name, "%*[^0-9]%d", &port_id) != 1) {
			SPDK_ERRLOG("Invalid mapping for Subsystem %d\n", sp->num);
			nvmf_delete_subsystem(subsystem);
			return -1;
		}
		if (strncasecmp(host_name, "Host",
				strlen("Host")) != 0
		    || sscanf(host_name, "%*[^0-9]%d", &host_id) != 1) {
			SPDK_ERRLOG("Invalid mapping for Subsystem %d\n", sp->num);
			nvmf_delete_subsystem(subsystem);
			return -1;
		}
		if (port_id < 1 || host_id < 1) {
			SPDK_ERRLOG("Invalid mapping for Subsystem %d\n", sp->num);
			nvmf_delete_subsystem(subsystem);
			return -1;
		}

		ret = spdk_nvmf_subsystem_add_map(subsystem, port_id, host_id);
		if (ret < 0) {
			nvmf_delete_subsystem(subsystem);
			return -1;
		}
	}

	val = spdk_conf_section_get_val(sp, "Controller");
	if (val == NULL) {
		SPDK_ERRLOG("Subsystem %d: missing Controller\n", sp->num);
		nvmf_delete_subsystem(subsystem);
		return -1;
	}

	/* claim this controller from the available controller list */
	nvmf_ctrlr = spdk_nvmf_ctrlr_claim(val);
	if (nvmf_ctrlr == NULL) {
		SPDK_ERRLOG("Subsystem %d: NVMe controller %s not found\n", sp->num, val);
		nvmf_delete_subsystem(subsystem);
		return -1;
	}

	ret = nvmf_subsystem_add_ctrlr(subsystem, nvmf_ctrlr->ctrlr);
	if (ret < 0) {
		SPDK_ERRLOG("Subsystem %d: adding controller %s failed\n", sp->num, val);
		nvmf_delete_subsystem(subsystem);
		return -1;
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "    NVMf Subsystem: Nvme Controller: %s , %p\n",
		      nvmf_ctrlr->name, nvmf_ctrlr->ctrlr);

	return 0;
}

static int
spdk_nvmf_parse_subsystems(void)
{
	int rc = 0;
	struct spdk_conf_section *sp;

	sp = spdk_conf_first_section(NULL);
	while (sp != NULL) {
		if (spdk_conf_section_match_prefix(sp, "Subsystem")) {
			rc = spdk_nvmf_parse_subsystem(sp);
			if (rc < 0) {
				return -1;
			}
		}
		sp = spdk_conf_next_section(sp);
	}
	return 0;
}

int
spdk_nvmf_parse_conf(void)
{
	int rc;

	/* NVMf section */
	rc = spdk_nvmf_parse_nvmf_tgt();
	if (rc < 0) {
		return rc;
	}

	/* Port sections */
	rc = spdk_nvmf_parse_ports();
	if (rc < 0) {
		return rc;
	}

	/* Host sections */
	rc = spdk_nvmf_parse_hosts();
	if (rc < 0) {
		return rc;
	}

	/* NVMe sections */
	rc = spdk_nvmf_parse_nvme();
	if (rc < 0) {
		return rc;
	}

	/* Subsystem sections */
	rc = spdk_nvmf_parse_subsystems();
	if (rc < 0) {
		return rc;
	}

	return 0;
}
