/*-
 * SSLsplit - transparent SSL/TLS interception
 * https://www.roe.ch/SSLsplit
 *
 * Copyright (c) 2009-2018, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "opts.h"

#include "sys.h"
#include "log.h"
#include "defaults.h"

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>

#ifndef OPENSSL_NO_DH
#include <openssl/dh.h>
#endif /* !OPENSSL_NO_DH */
#include <openssl/x509.h>

/*
 * Handle out of memory conditions in early stages of main().
 * Print error message and exit with failure status code.
 * Does not return.
 */
void NORET
oom_die(const char *argv0)
{
	fprintf(stderr, "%s: out of memory\n", argv0);
	exit(EXIT_FAILURE);
}

opts_t *
opts_new(void)
{
	opts_t *opts;

	opts = malloc(sizeof(opts_t));
	memset(opts, 0, sizeof(opts_t));

	opts->sslcomp = 1;
	opts->chain = sk_X509_new_null();
	opts->sslmethod = SSLv23_method;
	opts->leafkey_rsabits = DFLT_LEAFKEY_RSABITS;
	opts->conn_idle_timeout = 120;
	opts->expired_conn_check_period = 10;
	opts->ssl_shutdown_retry_delay = 100;
	opts->stats_period = 1;
	opts->remove_http_accept_encoding = 1;
	opts->remove_http_referer = 1;
	opts->verify_peer = 1;
	opts->user_timeout = 300;
	opts->max_http_header_size = 8192;
	return opts;
}

void
opts_free(opts_t *opts)
{
	sk_X509_pop_free(opts->chain, X509_free);
	if (opts->clientcrt) {
		X509_free(opts->clientcrt);
	}
	if (opts->clientkey) {
		EVP_PKEY_free(opts->clientkey);
	}
	if (opts->cacrt) {
		X509_free(opts->cacrt);
	}
	if (opts->cakey) {
		EVP_PKEY_free(opts->cakey);
	}
	if (opts->key) {
		EVP_PKEY_free(opts->key);
	}
#ifndef OPENSSL_NO_DH
	if (opts->dh) {
		DH_free(opts->dh);
	}
#endif /* !OPENSSL_NO_DH */
#ifndef OPENSSL_NO_ECDH
	if (opts->ecdhcurve) {
		free(opts->ecdhcurve);
	}
#endif /* !OPENSSL_NO_ECDH */
	if (opts->spec) {
		proxyspec_free(opts->spec);
	}
	if (opts->ciphers) {
		free(opts->ciphers);
	}
#ifndef OPENSSL_NO_ENGINE
	if (opts->openssl_engine) {
		free(opts->openssl_engine);
	}
#endif /* !OPENSSL_NO_ENGINE */
	if (opts->tgcrtdir) {
		free(opts->tgcrtdir);
	}
	if (opts->crlurl) {
		free(opts->crlurl);
	}
	if (opts->dropuser) {
		free(opts->dropuser);
	}
	if (opts->dropgroup) {
		free(opts->dropgroup);
	}
	if (opts->jaildir) {
		free(opts->jaildir);
	}
	if (opts->pidfile) {
		free(opts->pidfile);
	}
	if (opts->connectlog) {
		free(opts->connectlog);
	}
	if (opts->contentlog) {
		free(opts->contentlog);
	}
	if (opts->certgendir) {
		free(opts->certgendir);
	}
	if (opts->contentlog_basedir) {
		free(opts->contentlog_basedir);
	}
	if (opts->masterkeylog) {
		free(opts->masterkeylog);
	}
	if (opts->pcaplog) {
		free(opts->pcaplog);
	}
	if (opts->pcaplog_basedir) {
		free(opts->pcaplog_basedir);
	}
#ifndef WITHOUT_MIRROR
	if (opts->mirrorif) {
		free(opts->mirrorif);
	}
	if (opts->mirrortarget) {
		free(opts->mirrortarget);
	}
#endif /* !WITHOUT_MIRROR */
	if (opts->userdb_path) {
		free(opts->userdb_path);
	}
	if (opts->user_auth_url) {
		free(opts->user_auth_url);
	}
	if (opts->user_auth) {
		sqlite3_finalize(opts->update_user_atime);
		sqlite3_close(opts->userdb);
	}
	passsite_t *passsite = opts->passsites;
	while (passsite) {
		passsite_t *next = passsite->next;
		free(passsite->site);
		if (passsite->ip)
			free(passsite->ip);
		if (passsite->user)
			free(passsite->user);
		if (passsite->keyword)
			free(passsite->keyword);
		free(passsite);
		passsite = next;
	}
	memset(opts, 0, sizeof(opts_t));
	free(opts);
}

/*
 * Return 1 if opts_t contains a proxyspec that (eventually) uses SSL/TLS,
 * 0 otherwise.  When 0, it is safe to assume that no SSL/TLS operations
 * will take place with this configuration.
 */
int
opts_has_ssl_spec(opts_t *opts)
{
	proxyspec_t *p = opts->spec;

	while (p) {
		if (p->ssl || p->upgrade)
			return 1;
		p = p->next;
	}

	return 0;
}

/*
 * Return 1 if opts_t contains a proxyspec with dns, 0 otherwise.
 */
int
opts_has_dns_spec(opts_t *opts)
{
	proxyspec_t *p = opts->spec;

	while (p) {
		if (p->dns)
			return 1;
		p = p->next;
	}

	return 0;
}

/*
 * Dump the SSL/TLS protocol related configuration to the debug log.
 */
void
opts_proto_dbg_dump(opts_t *opts)
{
	log_dbg_printf("SSL/TLS protocol: %s%s%s%s%s%s\n",
#if (OPENSSL_VERSION_NUMBER < 0x10100000L) || defined(LIBRESSL_VERSION_NUMBER)
#ifdef HAVE_SSLV2
	               (opts->sslmethod == SSLv2_method) ? "ssl2" :
#endif /* HAVE_SSLV2 */
#ifdef HAVE_SSLV3
	               (opts->sslmethod == SSLv3_method) ? "ssl3" :
#endif /* HAVE_SSLV3 */
#ifdef HAVE_TLSV10
	               (opts->sslmethod == TLSv1_method) ? "tls10" :
#endif /* HAVE_TLSV10 */
#ifdef HAVE_TLSV11
	               (opts->sslmethod == TLSv1_1_method) ? "tls11" :
#endif /* HAVE_TLSV11 */
#ifdef HAVE_TLSV12
	               (opts->sslmethod == TLSv1_2_method) ? "tls12" :
#endif /* HAVE_TLSV12 */
#else /* OPENSSL_VERSION_NUMBER >= 0x10100000L */
#ifdef HAVE_SSLV3
	               (opts->sslversion == SSL3_VERSION) ? "ssl3" :
#endif /* HAVE_SSLV3 */
#ifdef HAVE_TLSV10
	               (opts->sslversion == TLS1_VERSION) ? "tls10" :
#endif /* HAVE_TLSV10 */
#ifdef HAVE_TLSV11
	               (opts->sslversion == TLS1_1_VERSION) ? "tls11" :
#endif /* HAVE_TLSV11 */
#ifdef HAVE_TLSV12
	               (opts->sslversion == TLS1_2_VERSION) ? "tls12" :
#endif /* HAVE_TLSV12 */
#endif /* OPENSSL_VERSION_NUMBER >= 0x10100000L */
	               "negotiate",
#ifdef HAVE_SSLV2
	               opts->no_ssl2 ? " -ssl2" :
#endif /* HAVE_SSLV2 */
	               "",
#ifdef HAVE_SSLV3
	               opts->no_ssl3 ? " -ssl3" :
#endif /* HAVE_SSLV3 */
	               "",
#ifdef HAVE_TLSV10
	               opts->no_tls10 ? " -tls10" :
#endif /* HAVE_TLSV10 */
	               "",
#ifdef HAVE_TLSV11
	               opts->no_tls11 ? " -tls11" :
#endif /* HAVE_TLSV11 */
	               "",
#ifdef HAVE_TLSV12
	               opts->no_tls12 ? " -tls12" :
#endif /* HAVE_TLSV12 */
	               "");
}

/*
 * Parse proxyspecs using a simple state machine.
 */
void
proxyspec_parse(int *argc, char **argv[], const char *natengine,
                proxyspec_t **opts_spec)
{
	proxyspec_t *spec = NULL;
	char *addr = NULL;
	int af = AF_UNSPEC;
	int state = 0;

	while ((*argc)--) {
		switch (state) {
			default:
			case 0:
				/* tcp | ssl | http | https | autossl | pop3 | pop3s | smtp | smtps */
				spec = malloc(sizeof(proxyspec_t));
				memset(spec, 0, sizeof(proxyspec_t));
				spec->next = *opts_spec;
				*opts_spec = spec;

				/* Defaults */
				spec->ssl = 0;
				spec->http = 0;
				spec->upgrade = 0;
				spec->pop3 = 0;
				spec->smtp = 0;
				if (!strcmp(**argv, "tcp")) {
					/* use defaults */
				} else
				if (!strcmp(**argv, "ssl")) {
					spec->ssl = 1;
				} else
				if (!strcmp(**argv, "http")) {
					spec->http = 1;
				} else
				if (!strcmp(**argv, "https")) {
					spec->ssl = 1;
					spec->http = 1;
				} else
				if (!strcmp(**argv, "autossl")) {
					spec->upgrade = 1;
				} else
				if (!strcmp(**argv, "pop3")) {
					spec->pop3 = 1;
				} else
				if (!strcmp(**argv, "pop3s")) {
					spec->ssl = 1;
					spec->pop3 = 1;
				} else
				if (!strcmp(**argv, "smtp")) {
					spec->smtp = 1;
				} else
				if (!strcmp(**argv, "smtps")) {
					spec->ssl = 1;
					spec->smtp = 1;
				} else {
					fprintf(stderr, "Unknown connection "
					                "type '%s'\n", **argv);
					exit(EXIT_FAILURE);
				}
				state++;
				break;
			case 1:
				/* listenaddr */
				addr = **argv;
				state++;
				break;
			case 2:
				/* listenport */
				af = sys_sockaddr_parse(&spec->listen_addr,
				                        &spec->listen_addrlen,
				                        addr, **argv,
				                        sys_get_af(addr),
				                        EVUTIL_AI_PASSIVE);
				if (af == -1) {
					exit(EXIT_FAILURE);
				}
				if (natengine) {
					spec->natengine = strdup(natengine);
					if (!spec->natengine) {
						fprintf(stderr,
						        "Out of memory"
						        "\n");
						exit(EXIT_FAILURE);
					}
				} else {
					spec->natengine = NULL;
				}
				state++;
				break;
			case 3:
				// UTM port is mandatory
				// The UTM port is set/used in pf and UTM service config.
				// @todo IPv6?
				if (strstr(**argv, "up:")) {
					char *up = **argv + 3;
					char *ua = "127.0.0.1";
					char *ra = "127.0.0.1";

					// ua and ra are optional, if both specified, ua should come before ra
					// UTM address
					if (*argc && strstr(*((*argv) + 1), "ua:")) {
						(*argv)++; (*argc)--;
						ua = **argv + 3;
					}
					// UTM return address
					if (*argc && strstr(*((*argv) + 1), "ra:")) {
						(*argv)++; (*argc)--;
						ra = **argv + 3;
					}

					int utm_af = sys_sockaddr_parse(&spec->conn_dst_addr,
										&spec->conn_dst_addrlen,
										ua, up, AF_INET, EVUTIL_AI_PASSIVE);
					if (utm_af == -1) {
						exit(EXIT_FAILURE);
					}
					utm_af = sys_sockaddr_parse(&spec->child_src_addr,
										&spec->child_src_addrlen,
										ra, "0", AF_INET, EVUTIL_AI_PASSIVE);
					if (utm_af == -1) {
						exit(EXIT_FAILURE);
					}
					state++;
				}
				break;
			case 4:
				/* [ natengine | dstaddr ] */
				if (!strcmp(**argv, "tcp") ||
				    !strcmp(**argv, "ssl") ||
				    !strcmp(**argv, "http") ||
				    !strcmp(**argv, "https") ||
				    !strcmp(**argv, "autossl") ||
				    !strcmp(**argv, "pop3") ||
				    !strcmp(**argv, "pop3s") ||
				    !strcmp(**argv, "smtp") ||
				    !strcmp(**argv, "smtps")) {
					/* implicit default natengine */
					(*argv)--; (*argc)++; /* rewind */
					state = 0;
				} else
				if (!strcmp(**argv, "sni")) {
					free(spec->natengine);
					spec->natengine = NULL;
					if (!spec->ssl) {
						fprintf(stderr,
						        "SNI hostname lookup "
						        "only works for ssl "
						        "and https proxyspecs"
						        "\n");
						exit(EXIT_FAILURE);
					}
					state = 6;
				} else
				if (nat_exist(**argv)) {
					/* natengine */
					free(spec->natengine);
					spec->natengine = strdup(**argv);
					if (!spec->natengine) {
						fprintf(stderr,
						        "Out of memory"
						        "\n");
						exit(EXIT_FAILURE);
					}
					state = 0;
				} else {
					/* explicit target address */
					free(spec->natengine);
					spec->natengine = NULL;
					addr = **argv;
					state++;
				}
				break;
			case 5:
				/* dstport */
				af = sys_sockaddr_parse(&spec->connect_addr,
				                        &spec->connect_addrlen,
				                        addr, **argv, af, 0);
				if (af == -1) {
					exit(EXIT_FAILURE);
				}
				state = 0;
				break;
			case 6:
				/* SNI dstport */
				spec->sni_port = atoi(**argv);
				if (!spec->sni_port) {
					fprintf(stderr, "Invalid port '%s'\n",
					                **argv);
					exit(EXIT_FAILURE);
				}
				spec->dns = 1;
				state = 0;
				break;
		}
		(*argv)++;
	}
	if (state != 0 && state != 4) {
		fprintf(stderr, "Incomplete proxyspec!\n");
		exit(EXIT_FAILURE);
	}
}

/*
 * Clear and free a proxy spec.
 */
void
proxyspec_free(proxyspec_t *spec)
{
	do {
		proxyspec_t *next = spec->next;
		if (spec->natengine)
			free(spec->natengine);
		memset(spec, 0, sizeof(proxyspec_t));
		free(spec);
		spec = next;
	} while (spec);
}

/*
 * Return text representation of proxy spec for display to the user.
 * Returned string must be freed by caller.
 */
char *
proxyspec_str(proxyspec_t *spec)
{
	char *s;
	char *lhbuf, *lpbuf;
	char *cbuf = NULL;
	char *pdstbuf = NULL;
	char *csrcbuf = NULL;
	if (sys_sockaddr_str((struct sockaddr *)&spec->listen_addr,
	                     spec->listen_addrlen, &lhbuf, &lpbuf) != 0) {
		return NULL;
	}
	if (spec->connect_addrlen) {
		char *chbuf, *cpbuf;
		if (sys_sockaddr_str((struct sockaddr *)&spec->connect_addr,
		                     spec->connect_addrlen,
		                     &chbuf, &cpbuf) != 0) {
			return NULL;
		}
		if (asprintf(&cbuf, "\nconnect= [%s]:%s", chbuf, cpbuf) < 0) {
			return NULL;
		}
		free(chbuf);
		free(cpbuf);
	}
	if (spec->conn_dst_addrlen) {
		char *chbuf, *cpbuf;
		if (sys_sockaddr_str((struct sockaddr *)&spec->conn_dst_addr,
		                     spec->conn_dst_addrlen,
		                     &chbuf, &cpbuf) != 0) {
			return NULL;
		}
		if (asprintf(&pdstbuf, "\nparent dst addr= [%s]:%s", chbuf, cpbuf) < 0) {
			return NULL;
		}
		free(chbuf);
		free(cpbuf);
	}
	if (spec->child_src_addrlen) {
		char *chbuf, *cpbuf;
		if (sys_sockaddr_str((struct sockaddr *)&spec->child_src_addr,
		                     spec->child_src_addrlen,
		                     &chbuf, &cpbuf) != 0) {
			return NULL;
		}
		if (asprintf(&csrcbuf, "\nchild src addr= [%s]:%s", chbuf, cpbuf) < 0) {
			return NULL;
		}
		free(chbuf);
		free(cpbuf);
	}
	if (spec->sni_port) {
		if (asprintf(&cbuf, "\nsni %i", spec->sni_port) < 0) {
			return NULL;
		}
	}
	if (asprintf(&s, "listen=[%s]:%s %s%s%s%s%s %s%s%s", lhbuf, lpbuf,
	             (spec->ssl ? "ssl" : "tcp"),
	             (spec->upgrade ? "|autossl" : ""),
	             (spec->http ? "|http" : ""),
	             (spec->pop3 ? "|pop3" : ""),
	             (spec->smtp ? "|smtp" : ""),
	             (spec->natengine ? spec->natengine : cbuf),
	             (pdstbuf),
	             (csrcbuf)) < 0) {
		s = NULL;
	}
	free(lhbuf);
	free(lpbuf);
	if (cbuf)
		free(cbuf);
	if (pdstbuf)
		free(pdstbuf);
	if (csrcbuf)
		free(csrcbuf);
	return s;
}

void
opts_set_cacrt(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->cacrt)
		X509_free(opts->cacrt);
	opts->cacrt = ssl_x509_load(optarg);
	if (!opts->cacrt) {
		fprintf(stderr, "%s: error loading CA cert from '%s':\n",
		        argv0, optarg);
		if (errno) {
			fprintf(stderr, "%s\n", strerror(errno));
		} else {
			ERR_print_errors_fp(stderr);
		}
		exit(EXIT_FAILURE);
	}
	ssl_x509_refcount_inc(opts->cacrt);
	sk_X509_insert(opts->chain, opts->cacrt, 0);
	if (!opts->cakey) {
		opts->cakey = ssl_key_load(optarg);
	}
#ifndef OPENSSL_NO_DH
	if (!opts->dh) {
		opts->dh = ssl_dh_load(optarg);
	}
#endif /* !OPENSSL_NO_DH */
#ifdef DEBUG_OPTS
	log_dbg_printf("CACert: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

void
opts_set_cakey(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->cakey)
		EVP_PKEY_free(opts->cakey);
	opts->cakey = ssl_key_load(optarg);
	if (!opts->cakey) {
		fprintf(stderr, "%s: error loading CA key from '%s':\n",
		        argv0, optarg);
		if (errno) {
			fprintf(stderr, "%s\n", strerror(errno));
		} else {
			ERR_print_errors_fp(stderr);
		}
		exit(EXIT_FAILURE);
	}
	if (!opts->cacrt) {
		opts->cacrt = ssl_x509_load(optarg);
		if (opts->cacrt) {
			ssl_x509_refcount_inc(opts->cacrt);
			sk_X509_insert(opts->chain, opts->cacrt, 0);
		}
	}
#ifndef OPENSSL_NO_DH
	if (!opts->dh) {
		opts->dh = ssl_dh_load(optarg);
	}
#endif /* !OPENSSL_NO_DH */
#ifdef DEBUG_OPTS
	log_dbg_printf("CAKey: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

void
opts_set_chain(opts_t *opts, const char *argv0, const char *optarg)
{
	if (ssl_x509chain_load(NULL, &opts->chain, optarg) == -1) {
		fprintf(stderr, "%s: error loading chain from '%s':\n",
		        argv0, optarg);
		if (errno) {
			fprintf(stderr, "%s\n", strerror(errno));
		} else {
			ERR_print_errors_fp(stderr);
		}
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("CAChain: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

void
opts_set_key(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->key)
		EVP_PKEY_free(opts->key);
	opts->key = ssl_key_load(optarg);
	if (!opts->key) {
		fprintf(stderr, "%s: error loading leaf key from '%s':\n",
		        argv0, optarg);
		if (errno) {
			fprintf(stderr, "%s\n", strerror(errno));
		} else {
			ERR_print_errors_fp(stderr);
		}
		exit(EXIT_FAILURE);
	}
#ifndef OPENSSL_NO_DH
	if (!opts->dh) {
		opts->dh = ssl_dh_load(optarg);
	}
#endif /* !OPENSSL_NO_DH */
#ifdef DEBUG_OPTS
	log_dbg_printf("LeafCerts: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

void
opts_set_crl(opts_t *opts, const char *optarg)
{
	if (opts->crlurl)
		free(opts->crlurl);
	opts->crlurl = strdup(optarg);
#ifdef DEBUG_OPTS
	log_dbg_printf("CRL: %s\n", opts->crlurl);
#endif /* DEBUG_OPTS */
}

void
opts_set_tgcrtdir(opts_t *opts, const char *argv0, const char *optarg)
{
	if (!sys_isdir(optarg)) {
		fprintf(stderr, "%s: '%s' is not a directory\n",
		        argv0, optarg);
		exit(EXIT_FAILURE);
	}
	if (opts->tgcrtdir)
		free(opts->tgcrtdir);
	opts->tgcrtdir = strdup(optarg);
	if (!opts->tgcrtdir)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("TargetCertDir: %s\n", opts->tgcrtdir);
#endif /* DEBUG_OPTS */
}

static void
set_certgendir(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->certgendir)
		free(opts->certgendir);
	opts->certgendir = strdup(optarg);
	if (!opts->certgendir)
		oom_die(argv0);
}

void
opts_set_certgendir_writegencerts(opts_t *opts, const char *argv0,
                                  const char *optarg)
{
	opts->certgen_writeall = 0;
	set_certgendir(opts, argv0, optarg);
#ifdef DEBUG_OPTS
	log_dbg_printf("WriteGenCertsDir: certgendir=%s, writeall=%u\n",
	               opts->certgendir, opts->certgen_writeall);
#endif /* DEBUG_OPTS */
}

void
opts_set_certgendir_writeall(opts_t *opts, const char *argv0,
                             const char *optarg)
{
	opts->certgen_writeall = 1;
	set_certgendir(opts, argv0, optarg);
#ifdef DEBUG_OPTS
	log_dbg_printf("WriteAllCertsDir: certgendir=%s, writeall=%u\n",
	               opts->certgendir, opts->certgen_writeall);
#endif /* DEBUG_OPTS */
}

void
opts_set_deny_ocsp(opts_t *opts)
{
	opts->deny_ocsp = 1;
}

void
opts_unset_deny_ocsp(opts_t *opts)
{
	opts->deny_ocsp = 0;
}

void
opts_set_passthrough(opts_t *opts)
{
	opts->passthrough = 1;
}

void
opts_unset_passthrough(opts_t *opts)
{
	opts->passthrough = 0;
}

void
opts_set_clientcrt(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->clientcrt)
		X509_free(opts->clientcrt);
	opts->clientcrt = ssl_x509_load(optarg);
	if (!opts->clientcrt) {
		fprintf(stderr, "%s: error loading client cert from '%s':\n",
		        argv0, optarg);
		if (errno) {
			fprintf(stderr, "%s\n", strerror(errno));
		} else {
			ERR_print_errors_fp(stderr);
		}
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("ClientCert: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

void
opts_set_clientkey(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->clientkey)
		EVP_PKEY_free(opts->clientkey);
	opts->clientkey = ssl_key_load(optarg);
	if (!opts->clientkey) {
		fprintf(stderr, "%s: error loading client key from '%s':\n",
		        argv0, optarg);
		if (errno) {
			fprintf(stderr, "%s\n", strerror(errno));
		} else {
			ERR_print_errors_fp(stderr);
		}
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("ClientKey: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

#ifndef OPENSSL_NO_DH
void
opts_set_dh(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->dh)
		DH_free(opts->dh);
	opts->dh = ssl_dh_load(optarg);
	if (!opts->dh) {
		fprintf(stderr, "%s: error loading DH params from '%s':\n",
		        argv0, optarg);
		if (errno) {
			fprintf(stderr, "%s\n", strerror(errno));
		} else {
			ERR_print_errors_fp(stderr);
		}
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("DHGroupParams: %s\n", optarg);
#endif /* DEBUG_OPTS */
}
#endif /* !OPENSSL_NO_DH */

#ifndef OPENSSL_NO_ECDH
void
opts_set_ecdhcurve(opts_t *opts, const char *argv0, const char *optarg)
{
	EC_KEY *ec;
	if (opts->ecdhcurve)
		free(opts->ecdhcurve);
	if (!(ec = ssl_ec_by_name(optarg))) {
		fprintf(stderr, "%s: unknown curve '%s'\n", argv0, optarg);
		exit(EXIT_FAILURE);
	}
	EC_KEY_free(ec);
	opts->ecdhcurve = strdup(optarg);
	if (!opts->ecdhcurve)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("ECDHCurve: %s\n", opts->ecdhcurve);
#endif /* DEBUG_OPTS */
}
#endif /* !OPENSSL_NO_ECDH */

void
opts_set_sslcomp(opts_t *opts)
{
	opts->sslcomp = 1;
}

void
opts_unset_sslcomp(opts_t *opts)
{
	opts->sslcomp = 0;
}

void
opts_set_ciphers(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->ciphers)
		free(opts->ciphers);
	opts->ciphers = strdup(optarg);
	if (!opts->ciphers)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("Ciphers: %s\n", opts->ciphers);
#endif /* DEBUG_OPTS */
}

#ifndef OPENSSL_NO_ENGINE
void
opts_set_openssl_engine(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->openssl_engine)
		free(opts->openssl_engine);
	opts->openssl_engine = strdup(optarg);
	if (!opts->openssl_engine)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("OpenSSLEngine: %s\n", opts->openssl_engine);
#endif /* DEBUG_OPTS */
}
#endif /* !OPENSSL_NO_ENGINE */

/*
 * Parse SSL proto string in optarg and look up the corresponding SSL method.
 * Calls exit() on failure.
 */
void
opts_force_proto(opts_t *opts, const char *argv0, const char *optarg)
{
#if (OPENSSL_VERSION_NUMBER < 0x10100000L) || defined(LIBRESSL_VERSION_NUMBER)
	if (opts->sslmethod != SSLv23_method) {
#else /* OPENSSL_VERSION_NUMBER >= 0x10100000L */
	if (opts->sslversion) {
#endif /* OPENSSL_VERSION_NUMBER >= 0x10100000L */
		fprintf(stderr, "%s: cannot use -r multiple times\n", argv0);
		exit(EXIT_FAILURE);
	}

#if (OPENSSL_VERSION_NUMBER < 0x10100000L) || defined(LIBRESSL_VERSION_NUMBER)
#ifdef HAVE_SSLV2
	if (!strcmp(optarg, "ssl2")) {
		opts->sslmethod = SSLv2_method;
	} else
#endif /* HAVE_SSLV2 */
#ifdef HAVE_SSLV3
	if (!strcmp(optarg, "ssl3")) {
		opts->sslmethod = SSLv3_method;
	} else
#endif /* HAVE_SSLV3 */
#ifdef HAVE_TLSV10
	if (!strcmp(optarg, "tls10") || !strcmp(optarg, "tls1")) {
		opts->sslmethod = TLSv1_method;
	} else
#endif /* HAVE_TLSV10 */
#ifdef HAVE_TLSV11
	if (!strcmp(optarg, "tls11")) {
		opts->sslmethod = TLSv1_1_method;
	} else
#endif /* HAVE_TLSV11 */
#ifdef HAVE_TLSV12
	if (!strcmp(optarg, "tls12")) {
		opts->sslmethod = TLSv1_2_method;
	} else
#endif /* HAVE_TLSV12 */
#else /* OPENSSL_VERSION_NUMBER >= 0x10100000L */
/*
 * Support for SSLv2 and the corresponding SSLv2_method(),
 * SSLv2_server_method() and SSLv2_client_method() functions were
 * removed in OpenSSL 1.1.0.
 */
#ifdef HAVE_SSLV3
	if (!strcmp(optarg, "ssl3")) {
		opts->sslversion = SSL3_VERSION;
	} else
#endif /* HAVE_SSLV3 */
#ifdef HAVE_TLSV10
	if (!strcmp(optarg, "tls10") || !strcmp(optarg, "tls1")) {
		opts->sslversion = TLS1_VERSION;
	} else
#endif /* HAVE_TLSV10 */
#ifdef HAVE_TLSV11
	if (!strcmp(optarg, "tls11")) {
		opts->sslversion = TLS1_1_VERSION;
	} else
#endif /* HAVE_TLSV11 */
#ifdef HAVE_TLSV12
	if (!strcmp(optarg, "tls12")) {
		opts->sslversion = TLS1_2_VERSION;
	} else
#endif /* HAVE_TLSV12 */
#endif /* OPENSSL_VERSION_NUMBER >= 0x10100000L */
	{
		fprintf(stderr, "%s: Unsupported SSL/TLS protocol '%s'\n",
		                argv0, optarg);
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("ForceSSLProto: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

/*
 * Parse SSL proto string in optarg and set the corresponding no_foo bit.
 * Calls exit() on failure.
 */
void
opts_disable_proto(opts_t *opts, const char *argv0, const char *optarg)
{
#ifdef HAVE_SSLV2
	if (!strcmp(optarg, "ssl2")) {
		opts->no_ssl2 = 1;
	} else
#endif /* HAVE_SSLV2 */
#ifdef HAVE_SSLV3
	if (!strcmp(optarg, "ssl3")) {
		opts->no_ssl3 = 1;
	} else
#endif /* HAVE_SSLV3 */
#ifdef HAVE_TLSV10
	if (!strcmp(optarg, "tls10") || !strcmp(optarg, "tls1")) {
		opts->no_tls10 = 1;
	} else
#endif /* HAVE_TLSV10 */
#ifdef HAVE_TLSV11
	if (!strcmp(optarg, "tls11")) {
		opts->no_tls11 = 1;
	} else
#endif /* HAVE_TLSV11 */
#ifdef HAVE_TLSV12
	if (!strcmp(optarg, "tls12")) {
		opts->no_tls12 = 1;
	} else
#endif /* HAVE_TLSV12 */
	{
		fprintf(stderr, "%s: Unsupported SSL/TLS protocol '%s'\n",
		                argv0, optarg);
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("DisableSSLProto: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

void
opts_set_user(opts_t *opts, const char *argv0, const char *optarg)
{
	if (!sys_isuser(optarg)) {
		fprintf(stderr, "%s: '%s' is not an existing user\n",
		        argv0, optarg);
		exit(EXIT_FAILURE);
	}
	if (opts->dropuser)
		free(opts->dropuser);
	opts->dropuser = strdup(optarg);
	if (!opts->dropuser)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("User: %s\n", opts->dropuser);
#endif /* DEBUG_OPTS */
}

void
opts_set_group(opts_t *opts, const char *argv0, const char *optarg)
{

	if (!sys_isgroup(optarg)) {
		fprintf(stderr, "%s: '%s' is not an existing group\n",
		        argv0, optarg);
		exit(EXIT_FAILURE);
	}
	if (opts->dropgroup)
		free(opts->dropgroup);
	opts->dropgroup = strdup(optarg);
	if (!opts->dropgroup)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("Group: %s\n", opts->dropgroup);
#endif /* DEBUG_OPTS */
}

void
opts_set_jaildir(opts_t *opts, const char *argv0, const char *optarg)
{
	if (!sys_isdir(optarg)) {
		fprintf(stderr, "%s: '%s' is not a directory\n", argv0, optarg);
		exit(EXIT_FAILURE);
	}
	if (opts->jaildir)
		free(opts->jaildir);
	opts->jaildir = realpath(optarg, NULL);
	if (!opts->jaildir) {
		fprintf(stderr, "%s: Failed to realpath '%s': %s (%i)\n",
		        argv0, optarg, strerror(errno), errno);
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("Chroot: %s\n", opts->jaildir);
#endif /* DEBUG_OPTS */
}

void
opts_set_pidfile(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->pidfile)
		free(opts->pidfile);
	opts->pidfile = strdup(optarg);
	if (!opts->pidfile)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("PidFile: %s\n", opts->pidfile);
#endif /* DEBUG_OPTS */
}

void
opts_set_connectlog(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->connectlog)
		free(opts->connectlog);
	if (!(opts->connectlog = sys_realdir(optarg))) {
		if (errno == ENOENT) {
			fprintf(stderr, "Directory part of '%s' does not "
			                "exist\n", optarg);
			exit(EXIT_FAILURE);
		} else {
			fprintf(stderr, "Failed to realpath '%s': %s (%i)\n",
			              optarg, strerror(errno), errno);
			oom_die(argv0);
		}
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("ConnectLog: %s\n", opts->connectlog);
#endif /* DEBUG_OPTS */
}

void
opts_set_contentlog(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->contentlog)
		free(opts->contentlog);
	if (!(opts->contentlog = sys_realdir(optarg))) {
		if (errno == ENOENT) {
			fprintf(stderr, "Directory part of '%s' does not "
			                "exist\n", optarg);
			exit(EXIT_FAILURE);
		} else {
			fprintf(stderr, "Failed to realpath '%s': %s (%i)\n",
			              optarg, strerror(errno), errno);
			oom_die(argv0);
		}
	}
	opts->contentlog_isdir = 0;
	opts->contentlog_isspec = 0;
#ifdef DEBUG_OPTS
	log_dbg_printf("ContentLog: %s\n", opts->contentlog);
#endif /* DEBUG_OPTS */
}

void
opts_set_contentlogdir(opts_t *opts, const char *argv0, const char *optarg)
{
	if (!sys_isdir(optarg)) {
		fprintf(stderr, "%s: '%s' is not a directory\n", argv0, optarg);
		exit(EXIT_FAILURE);
	}
	if (opts->contentlog)
		free(opts->contentlog);
	opts->contentlog = realpath(optarg, NULL);
	if (!opts->contentlog) {
		fprintf(stderr, "%s: Failed to realpath '%s': %s (%i)\n",
		        argv0, optarg, strerror(errno), errno);
		exit(EXIT_FAILURE);
	}
	opts->contentlog_isdir = 1;
	opts->contentlog_isspec = 0;
#ifdef DEBUG_OPTS
	log_dbg_printf("ContentLogDir: %s\n", opts->contentlog);
#endif /* DEBUG_OPTS */
}

static void
opts_set_logbasedir(const char *argv0, const char *optarg,
                    char **basedir, char **log)
{
	char *lhs, *rhs, *p, *q;
	size_t n;
	if (*basedir)
		free(*basedir);
	if (*log)
		free(*log);
	if (log_content_split_pathspec(optarg, &lhs, &rhs) == -1) {
		fprintf(stderr, "%s: Failed to split '%s' in lhs/rhs:"
		                " %s (%i)\n", argv0, optarg,
		                strerror(errno), errno);
		exit(EXIT_FAILURE);
	}
	/* eliminate %% from lhs */
	for (p = q = lhs; *p; p++, q++) {
		if (q < p)
			*q = *p;
		if (*p == '%' && *(p+1) == '%')
			p++;
	}
	*q = '\0';
	/* all %% in lhs resolved to % */
	if (sys_mkpath(lhs, 0777) == -1) {
		fprintf(stderr, "%s: Failed to create '%s': %s (%i)\n",
		        argv0, lhs, strerror(errno), errno);
		exit(EXIT_FAILURE);
	}
	*basedir = realpath(lhs, NULL);
	if (!*basedir) {
		fprintf(stderr, "%s: Failed to realpath '%s': %s (%i)\n",
		        argv0, lhs, strerror(errno), errno);
		exit(EXIT_FAILURE);
	}
	/* count '%' in basedir */
	for (n = 0, p = *basedir;
		 *p;
		 p++) {
		if (*p == '%')
			n++;
	}
	free(lhs);
	n += strlen(*basedir);
	if (!(lhs = malloc(n + 1)))
		oom_die(argv0);
	/* re-encoding % to %%, copying basedir to lhs */
	for (p = *basedir, q = lhs;
		 *p;
		 p++, q++) {
		*q = *p;
		if (*q == '%')
			*(++q) = '%';
	}
	*q = '\0';
	/* lhs contains encoded realpathed basedir */
	if (asprintf(log, "%s/%s", lhs, rhs) < 0)
		oom_die(argv0);
	free(lhs);
	free(rhs);
}

void
opts_set_contentlogpathspec(opts_t *opts, const char *argv0, const char *optarg)
{
	opts_set_logbasedir(argv0, optarg, &opts->contentlog_basedir,
	                    &opts->contentlog);
	opts->contentlog_isdir = 0;
	opts->contentlog_isspec = 1;
#ifdef DEBUG_OPTS
	log_dbg_printf("ContentLogPathSpec: basedir=%s, %s\n",
	               opts->contentlog_basedir, opts->contentlog);
#endif /* DEBUG_OPTS */
}

#ifdef HAVE_LOCAL_PROCINFO
void
opts_set_lprocinfo(opts_t *opts)
{
	opts->lprocinfo = 1;
}

void
opts_unset_lprocinfo(opts_t *opts)
{
	opts->lprocinfo = 0;
}
#endif /* HAVE_LOCAL_PROCINFO */

void
opts_set_masterkeylog(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->masterkeylog)
		free(opts->masterkeylog);
	if (!(opts->masterkeylog = sys_realdir(optarg))) {
		if (errno == ENOENT) {
			fprintf(stderr, "Directory part of '%s' does not "
			                "exist\n", optarg);
			exit(EXIT_FAILURE);
		} else {
			fprintf(stderr, "Failed to realpath '%s': %s (%i)\n",
			              optarg, strerror(errno), errno);
			oom_die(argv0);
		}
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("MasterKeyLog: %s\n", opts->masterkeylog);
#endif /* DEBUG_OPTS */
}

void
opts_set_pcaplog(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->pcaplog)
		free(opts->pcaplog);
	if (!(opts->pcaplog = sys_realdir(optarg))) {
		if (errno == ENOENT) {
			fprintf(stderr, "Directory part of '%s' does not "
			                "exist\n", optarg);
			exit(EXIT_FAILURE);
		} else {
			fprintf(stderr, "Failed to realpath '%s': %s (%i)\n",
			              optarg, strerror(errno), errno);
			oom_die(argv0);
		}
	}
	opts->pcaplog_isdir = 0;
	opts->pcaplog_isspec = 0;
#ifdef DEBUG_OPTS
	log_dbg_printf("PcapLog: %s\n", opts->pcaplog);
#endif /* DEBUG_OPTS */
}

void
opts_set_pcaplogdir(opts_t *opts, const char *argv0, const char *optarg)
{
	if (!sys_isdir(optarg)) {
		fprintf(stderr, "%s: '%s' is not a directory\n", argv0, optarg);
		exit(EXIT_FAILURE);
	}
	if (opts->pcaplog)
		free(opts->pcaplog);
	opts->pcaplog = realpath(optarg, NULL);
	if (!opts->pcaplog) {
		fprintf(stderr, "%s: Failed to realpath '%s': %s (%i)\n",
		        argv0, optarg, strerror(errno), errno);
		exit(EXIT_FAILURE);
	}
	opts->pcaplog_isdir = 1;
	opts->pcaplog_isspec = 0;
#ifdef DEBUG_OPTS
	log_dbg_printf("PcapLogDir: %s\n", opts->pcaplog);
#endif /* DEBUG_OPTS */
}

void
opts_set_pcaplogpathspec(opts_t *opts, const char *argv0, const char *optarg)
{
	opts_set_logbasedir(argv0, optarg, &opts->pcaplog_basedir,
	                    &opts->pcaplog);
	opts->pcaplog_isdir = 0;
	opts->pcaplog_isspec = 1;
#ifdef DEBUG_OPTS
	log_dbg_printf("PcapLogPathSpec: basedir=%s, %s\n",
	               opts->pcaplog_basedir, opts->pcaplog);
#endif /* DEBUG_OPTS */
}

#ifndef WITHOUT_MIRROR
void
opts_set_mirrorif(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->mirrorif)
		free(opts->mirrorif);
	opts->mirrorif = strdup(optarg);
	if (!opts->mirrorif)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("MirrorIf: %s\n", opts->mirrorif);
#endif /* DEBUG_OPTS */
}

void
opts_set_mirrortarget(opts_t *opts, const char *argv0, const char *optarg)
{
	if (opts->mirrortarget)
		free(opts->mirrortarget);
	opts->mirrortarget = strdup(optarg);
	if (!opts->mirrortarget)
		oom_die(argv0);
#ifdef DEBUG_OPTS
	log_dbg_printf("MirrorTarget: %s\n", opts->mirrortarget);
#endif /* DEBUG_OPTS */
}
#endif /* !WITHOUT_MIRROR */

void
opts_set_daemon(opts_t *opts)
{
	opts->detach = 1;
}

void
opts_unset_daemon(opts_t *opts)
{
	opts->detach = 0;
}

void
opts_set_debug(opts_t *opts)
{
	log_dbg_mode(LOG_DBG_MODE_ERRLOG);
	opts->debug = 1;
}

void
opts_unset_debug(opts_t *opts)
{
	log_dbg_mode(LOG_DBG_MODE_NONE);
	opts->debug = 0;
}

void
opts_set_debug_level(const char *optarg)
{
	// Compare strlen(s2)+1 chars to match exactly
	if (strncmp(optarg, "2", 2) == 0) {
		log_dbg_mode(LOG_DBG_MODE_FINE);
	} else if (strncmp(optarg, "3", 2) == 0) {
		log_dbg_mode(LOG_DBG_MODE_FINER);
	} else if (strncmp(optarg, "4", 2) == 0) {
		log_dbg_mode(LOG_DBG_MODE_FINEST);
	} else {
		fprintf(stderr, "Invalid DebugLevel '%s', use 2-4\n", optarg);
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("DebugLevel: %s\n", optarg);
#endif /* DEBUG_OPTS */
}

void
opts_set_statslog(opts_t *opts)
{
	opts->statslog = 1;
}

void
opts_unset_statslog(opts_t *opts)
{
	opts->statslog = 0;
}

static void
opts_set_remove_http_accept_encoding(opts_t *opts)
{
	opts->remove_http_accept_encoding = 1;
}

static void
opts_unset_remove_http_accept_encoding(opts_t *opts)
{
	opts->remove_http_accept_encoding = 0;
}

static void
opts_set_remove_http_referer(opts_t *opts)
{
	opts->remove_http_referer = 1;
}

static void
opts_unset_remove_http_referer(opts_t *opts)
{
	opts->remove_http_referer = 0;
}

static void
opts_set_verify_peer(opts_t *opts)
{
	opts->verify_peer = 1;
}

static void
opts_unset_verify_peer(opts_t *opts)
{
	opts->verify_peer = 0;
}

static void
opts_set_allow_wrong_host(opts_t *opts)
{
	opts->allow_wrong_host = 1;
}

static void
opts_unset_allow_wrong_host(opts_t *opts)
{
	opts->allow_wrong_host = 0;
}

static void
opts_set_user_auth(UNUSED opts_t *opts)
{
#if defined(__OpenBSD__) || defined(__linux__)
	// Enable user auth on OpenBSD and Linux only
	opts->user_auth = 1;
#endif /* __OpenBSD__ || __linux__ */
}

static void
opts_unset_user_auth(opts_t *opts)
{
	opts->user_auth = 0;
}

static void
opts_set_userdb_path(opts_t *opts, const char *optarg)
{
	if (opts->userdb_path)
		free(opts->userdb_path);
	opts->userdb_path = strdup(optarg);
#ifdef DEBUG_OPTS
	log_dbg_printf("UserDBPath: %s\n", opts->userdb_path);
#endif /* DEBUG_OPTS */
}

static void
opts_set_user_auth_url(opts_t *opts, const char *optarg)
{
	if (opts->user_auth_url)
		free(opts->user_auth_url);
	opts->user_auth_url = strdup(optarg);
#ifdef DEBUG_OPTS
	log_dbg_printf("UserAuthURL: %s\n", opts->user_auth_url);
#endif /* DEBUG_OPTS */
}

static void
opts_set_validate_proto(opts_t *opts)
{
	opts->validate_proto = 1;
}

static void
opts_unset_validate_proto(opts_t *opts)
{
	opts->validate_proto = 0;
}

static void
opts_set_open_files_limit(const char *value, int line_num)
{
	unsigned int i = atoi(value);
	if (i >= 50 && i <= 10000) {
		struct rlimit rl;
		rl.rlim_cur = i;
		rl.rlim_max = i;
		if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
			fprintf(stderr, "Failed setting OpenFilesLimit\n");
			if (errno) {
				fprintf(stderr, "%s\n", strerror(errno));
			} else {
				ERR_print_errors_fp(stderr);
			}
			exit(EXIT_FAILURE);
		}
	} else {
		fprintf(stderr, "Invalid OpenFilesLimit %s at line %d, use 50-10000\n", value, line_num);
		exit(EXIT_FAILURE);
	}
#ifdef DEBUG_OPTS
	log_dbg_printf("OpenFilesLimit: %u\n", i);
#endif /* DEBUG_OPTS */
}

static void
opts_set_pass_site(opts_t *opts, char *value, int line_num)
{
	// site [(clientaddr|(user|*) [description keyword])]
	char *argv[sizeof(char *) * 3];
	int argc = 0;
	char *p, *last = NULL;

	for ((p = strtok_r(value, " ", &last));
		 p;
		 (p = strtok_r(NULL, " ", &last))) {
		if (argc < 3) {
			argv[argc++] = p;
		} else {
			break;
		}
	}

	if (!argc) {
		fprintf(stderr, "PassSite requires at least one parameter at line %d\n", line_num);
		exit(EXIT_FAILURE);
	}

	passsite_t *ps = malloc(sizeof(passsite_t));
	memset(ps, 0, sizeof(passsite_t));

	size_t len = strlen(argv[0]);
	// Common names are separated by slashes
	char s[len + 3];
	strncpy(s + 1, argv[0], len);
	s[0] = '/';
	s[len + 1] = '/';
	s[len + 2] = '\0';
	ps->site = strdup(s);

	if (argc > 1) {
		if (!strcmp(argv[1], "*")) {
			ps->all = 1;
		} else if (sys_isuser(argv[1])) {
			if (!opts->user_auth) {
				fprintf(stderr, "PassSite user filter requires user auth at line %d\n", line_num);
				exit(EXIT_FAILURE);
			}
			ps->user = strdup(argv[1]);
		} else {
			ps->ip = strdup(argv[1]);
		}
	}

	if (argc > 2) {
		if (ps->ip) {
			fprintf(stderr, "PassSite client ip cannot define keyword filter at line %d\n", line_num);
			exit(EXIT_FAILURE);
		}
		ps->keyword = strdup(argv[2]);
	}

	ps->next = opts->passsites;
	opts->passsites = ps;
#ifdef DEBUG_OPTS
	log_dbg_printf("PassSite: %s, %s, %s, %s\n", ps->site, STRORDASH(ps->ip), ps->all ? "*" : STRORDASH(ps->user), STRORDASH(ps->keyword));
#endif /* DEBUG_OPTS */
}

static int
check_value_yesno(const char *value, const char *name, int line_num)
{
	/* Compare strlen(s2)+1 chars to match exactly */
	if (!strncmp(value, "yes", 4)) {
		return 1;
	} else if (!strncmp(value, "no", 3)) {
		return 0;
	}
	fprintf(stderr, "Error in conf: Invalid '%s' value '%s' at line %d, use yes|no\n", name, value, line_num);
	return -1;
}

#define MAX_TOKEN 10

static int
set_option(opts_t *opts, const char *argv0,
           const char *name, char *value, char **natengine, int line_num)
{
	int yes;
	int retval = -1;

	/* Compare strlen(s2)+1 chars to match exactly */
	if (!strncmp(name, "CACert", 7)) {
		opts_set_cacrt(opts, argv0, value);
	} else if (!strncmp(name, "CAKey", 6)) {
		opts_set_cakey(opts, argv0, value);
	} else if (!strncmp(name, "ClientCert", 11)) {
		opts_set_clientcrt(opts, argv0, value);
	} else if (!strncmp(name, "ClientKey", 10)) {
		opts_set_clientkey(opts, argv0, value);
	} else if (!strncmp(name, "CAChain", 8)) {
		opts_set_chain(opts, argv0, value);
	} else if (!strncmp(name, "LeafCerts", 10)) {
		opts_set_key(opts, argv0, value);
	} else if (!strncmp(name, "CRL", 4)) {
		opts_set_crl(opts, value);
	} else if (!strncmp(name, "TargetCertDir", 14)) {
		opts_set_tgcrtdir(opts, argv0, value);
	} else if (!strncmp(name, "WriteGenCertsDir", 17)) {
		opts_set_certgendir_writegencerts(opts, argv0, value);
	} else if (!strncmp(name, "WriteAllCertsDir", 17)) {
		opts_set_certgendir_writeall(opts, argv0, value);
	} else if (!strncmp(name, "DenyOCSP", 9)) {
		yes = check_value_yesno(value, "DenyOCSP", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_deny_ocsp(opts) : opts_unset_deny_ocsp(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("DenyOCSP: %u\n", opts->deny_ocsp);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "Passthrough", 12)) {
		yes = check_value_yesno(value, "Passthrough", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_passthrough(opts) : opts_unset_passthrough(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("Passthrough: %u\n", opts->passthrough);
#endif /* DEBUG_OPTS */
#ifndef OPENSSL_NO_DH
	} else if (!strncmp(name, "DHGroupParams", 14)) {
		opts_set_dh(opts, argv0, value);
#endif /* !OPENSSL_NO_DH */
#ifndef OPENSSL_NO_ECDH
	} else if (!strncmp(name, "ECDHCurve", 10)) {
		opts_set_ecdhcurve(opts, argv0, value);
#endif /* !OPENSSL_NO_ECDH */
#ifdef SSL_OP_NO_COMPRESSION
	} else if (!strncmp(name, "SSLCompression", 15)) {
		yes = check_value_yesno(value, "SSLCompression", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_sslcomp(opts) : opts_unset_sslcomp(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("SSLCompression: %u\n", opts->sslcomp);
#endif /* DEBUG_OPTS */
#endif /* SSL_OP_NO_COMPRESSION */
	} else if (!strncasecmp(name, "LeafKeyRSABits", 15)) {
		unsigned int i = atoi(value);
		if (i == 1024 || i == 2048 || i == 3072 || i == 4096) {
			opts->leafkey_rsabits = i;
		} else {
			fprintf(stderr, "Invalid LeafKeyRSABits %s at line %d, use 1024|2048|3072|4096\n", value, line_num);
			goto leave;
		}
#ifdef DEBUG_OPTS
		log_dbg_printf("LeafKeyRSABits: %u\n", opts->leafkey_rsabits);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "ForceSSLProto", 14)) {
		opts_force_proto(opts, argv0, value);
	} else if (!strncmp(name, "DisableSSLProto", 16)) {
		opts_disable_proto(opts, argv0, value);
	} else if (!strncmp(name, "Ciphers", 8)) {
		opts_set_ciphers(opts, argv0, value);
#ifndef OPENSSL_NO_ENGINE
	} else if (!strncmp(name, "OpenSSLEngine", 14)) {
		opts_set_openssl_engine(opts, argv0, value);
#endif /* !OPENSSL_NO_ENGINE */
	} else if (!strncmp(name, "NATEngine", 10)) {
		if (*natengine)
			free(*natengine);
		*natengine = strdup(value);
		if (!*natengine)
			goto leave;
#ifdef DEBUG_OPTS
		log_dbg_printf("NATEngine: %s\n", *natengine);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "User", 5)) {
		opts_set_user(opts, argv0, value);
	} else if (!strncmp(name, "Group", 6)) {
		opts_set_group(opts, argv0, value);
	} else if (!strncmp(name, "Chroot", 7)) {
		opts_set_jaildir(opts, argv0, value);
	} else if (!strncmp(name, "PidFile", 8)) {
		opts_set_pidfile(opts, argv0, value);
	} else if (!strncmp(name, "ConnectLog", 11)) {
		opts_set_connectlog(opts, argv0, value);
	} else if (!strncmp(name, "ContentLog", 11)) {
		opts_set_contentlog(opts, argv0, value);
	} else if (!strncmp(name, "ContentLogDir", 14)) {
		opts_set_contentlogdir(opts, argv0, value);
	} else if (!strncmp(name, "ContentLogPathSpec", 19)) {
		opts_set_contentlogpathspec(opts, argv0, value);
#ifdef HAVE_LOCAL_PROCINFO
	} else if (!strncmp(name, "LogProcInfo", 11)) {
		yes = check_value_yesno(value, "LogProcInfo", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_lprocinfo(opts) : opts_unset_lprocinfo(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("LogProcInfo: %u\n", opts->lprocinfo);
#endif /* DEBUG_OPTS */
#endif /* HAVE_LOCAL_PROCINFO */
	} else if (!strncmp(name, "MasterKeyLog", 13)) {
		opts_set_masterkeylog(opts, argv0, value);
	} else if (!strncmp(name, "PcapLog", 8)) {
		opts_set_pcaplog(opts, argv0, value);
	} else if (!strncmp(name, "PcapLogDir", 11)) {
		opts_set_pcaplogdir(opts, argv0, value);
	} else if (!strncmp(name, "PcapLogPathSpec", 16)) {
		opts_set_pcaplogpathspec(opts, argv0, value);
#ifndef WITHOUT_MIRROR
	} else if (!strncmp(name, "MirrorIf", 9)) {
		opts_set_mirrorif(opts, argv0, value);
	} else if (!strncmp(name, "MirrorTarget", 13)) {
		opts_set_mirrortarget(opts, argv0, value);
#endif /* !WITHOUT_MIRROR */
	} else if (!strncmp(name, "Daemon", 7)) {
		yes = check_value_yesno(value, "Daemon", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_daemon(opts) : opts_unset_daemon(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("Daemon: %u\n", opts->detach);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "Debug", 6)) {
		yes = check_value_yesno(value, "Debug", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_debug(opts) : opts_unset_debug(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("Debug: %u\n", opts->debug);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "DebugLevel", 11)) {
		opts_set_debug_level(value);
	} else if (!strncmp(name, "UserAuth", 9)) {
		yes = check_value_yesno(value, "UserAuth", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_user_auth(opts) : opts_unset_user_auth(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("UserAuth: %u\n", opts->user_auth);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "UserDBPath", 11)) {
		opts_set_userdb_path(opts, value);
	} else if (!strncmp(name, "UserAuthURL", 12)) {
		opts_set_user_auth_url(opts, value);
	} else if (!strncasecmp(name, "UserTimeout", 12)) {
		unsigned int i = atoi(value);
		if (i <= 86400) {
			opts->user_timeout = i;
		} else {
			fprintf(stderr, "Invalid UserTimeout %s at line %d, use 0-86400\n", value, line_num);
			goto leave;
		}
#ifdef DEBUG_OPTS
		log_dbg_printf("UserTimeout: %u\n", opts->user_timeout);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "ValidateProto", 14)) {
		yes = check_value_yesno(value, "ValidateProto", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_validate_proto(opts) : opts_unset_validate_proto(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("ValidateProto: %u\n", opts->validate_proto);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "MaxHTTPHeaderSize", 18)) {
		unsigned int i = atoi(value);
		if (i >= 1024 && i <= 65536) {
			opts->max_http_header_size = i;
		} else {
			fprintf(stderr, "Invalid MaxHTTPHeaderSize %s at line %d, use 1024-65536\n", value, line_num);
			goto leave;
		}
#ifdef DEBUG_OPTS
		log_dbg_printf("MaxHTTPHeaderSize: %u\n", opts->max_http_header_size);
#endif /* DEBUG_OPTS */
	} else if (!strncmp(name, "ProxySpec", 10)) {
		/* Use MAX_TOKEN instead of computing the actual number of tokens in value */
		char **argv = malloc(sizeof(char *) * MAX_TOKEN);
		char **save_argv = argv;
		int argc = 0;
		char *p, *last = NULL;

		for ((p = strtok_r(value, " ", &last));
		     p;
		     (p = strtok_r(NULL, " ", &last))) {
			/* Limit max # token */
			if (argc < MAX_TOKEN) {
				argv[argc++] = p;
			} else {
				break;
			}
		}

		proxyspec_parse(&argc, &argv, *natengine, &opts->spec);
		free(save_argv);
	} else if (!strncasecmp(name, "VerifyPeer", 11)) {
		yes = check_value_yesno(value, "VerifyPeer", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_verify_peer(opts) : opts_unset_verify_peer(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("VerifyPeer: %u\n", opts->verify_peer);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "AllowWrongHost", 15)) {
		yes = check_value_yesno(value, "AllowWrongHost", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_allow_wrong_host(opts)
		    : opts_unset_allow_wrong_host(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("AllowWrongHost: %u\n", opts->allow_wrong_host);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "ConnIdleTimeout", 16)) {
		unsigned int i = atoi(value);
		if (i >= 10 && i <= 3600) {
			opts->conn_idle_timeout = i;
		} else {
			fprintf(stderr, "Invalid ConnIdleTimeout %s at line %d, use 10-3600\n", value, line_num);
			goto leave;
		}
#ifdef DEBUG_OPTS
		log_dbg_printf("ConnIdleTimeout: %u\n", opts->conn_idle_timeout);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "ExpiredConnCheckPeriod", 23)) {
		unsigned int i = atoi(value);
		if (i >= 10 && i <= 60) {
			opts->expired_conn_check_period = i;
		} else {
			fprintf(stderr, "Invalid ExpiredConnCheckPeriod %s at line %d, use 10-60\n", value, line_num);
			goto leave;
		}
#ifdef DEBUG_OPTS
		log_dbg_printf("ExpiredConnCheckPeriod: %u\n", opts->expired_conn_check_period);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "SSLShutdownRetryDelay", 22)) {
		unsigned int i = atoi(value);
		if (i >= 100 && i <= 10000) {
			opts->ssl_shutdown_retry_delay = i;
		} else {
			fprintf(stderr, "Invalid SSLShutdownRetryDelay %s at line %d, use 100-10000\n", value, line_num);
			goto leave;
		}
#ifdef DEBUG_OPTS
		log_dbg_printf("SSLShutdownRetryDelay: %u\n", opts->ssl_shutdown_retry_delay);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "LogStats", 9)) {
		yes = check_value_yesno(value, "LogStats", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_statslog(opts) : opts_unset_statslog(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("LogStats: %u\n", opts->statslog);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "StatsPeriod", 12)) {
		unsigned int i = atoi(value);
		if (i >= 1 && i <= 10) {
			opts->stats_period = i;
		} else {
			fprintf(stderr, "Invalid StatsPeriod %s at line %d, use 1-10\n", value, line_num);
			goto leave;
		}
#ifdef DEBUG_OPTS
		log_dbg_printf("StatsPeriod: %u\n", opts->stats_period);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "RemoveHTTPAcceptEncoding", 25)) {
		yes = check_value_yesno(value, "RemoveHTTPAcceptEncoding", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_remove_http_accept_encoding(opts) : opts_unset_remove_http_accept_encoding(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("RemoveHTTPAcceptEncoding: %u\n", opts->remove_http_accept_encoding);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "RemoveHTTPReferer", 18)) {
		yes = check_value_yesno(value, "RemoveHTTPReferer", line_num);
		if (yes == -1) {
			goto leave;
		}
		yes ? opts_set_remove_http_referer(opts) : opts_unset_remove_http_referer(opts);
#ifdef DEBUG_OPTS
		log_dbg_printf("RemoveHTTPReferer: %u\n", opts->remove_http_referer);
#endif /* DEBUG_OPTS */
	} else if (!strncasecmp(name, "OpenFilesLimit", 15)) {
		opts_set_open_files_limit(value, line_num);
	} else if (!strncmp(name, "PassSite", 9)) {
		opts_set_pass_site(opts, value, line_num);
	} else {
		fprintf(stderr, "Error in conf: Unknown option "
		                "'%s' at line %d\n", name, line_num);
		goto leave;
	}

	retval = 0;
leave:
	return retval;
}

/*
 * Separator param is needed for command line options only.
 * Conf file option separator is ' '.
 */
static int
get_name_value(char **name, char **value, const char sep)
{
	char *n, *v, *value_end;
	int retval = -1;

	/* Skip to the end of option name and terminate it with '\0' */
	for (n = *name;; n++) {
		/* White spaces possible around separator,
		 * if the command line option is passed between the quotes */
		if (*n == ' ' || *n == '\t' || *n == sep) {
			*n = '\0';
			n++;
			break;
		}
		if (*n == '\0') {
			n = NULL;
			break;
		}
	}

	/* No option name */
	if (n == NULL) {
		fprintf(stderr, "Error in option: No option name\n");
		goto leave;
	}

	/* White spaces possible before value and around separator,
	 * if the command line option is passed between the quotes */
	while (*n == ' ' || *n == '\t' || *n == sep) {
		n++;
	}

	*value = n;

	/* Find end of value and terminate it with '\0'
	 * Find first occurrence of trailing white space */
	value_end = NULL;
	for (v = *value;; v++) {
		if (*v == '\0') {
			break;
		}
		if (*v == '\r' || *v == '\n') {
			*v = '\0';
			break;
		}
		if (*v == ' ' || *v == '\t') {
			if (!value_end) {
				value_end = v;
			}
		} else {
			value_end = NULL;
		}
	}

	if (value_end) {
		*value_end = '\0';
	}

	retval = 0;
leave:
	return retval;
}

int
opts_set_option(opts_t *opts, const char *argv0, const char *optarg,
                char **natengine)
{
	char *name, *value;
	int retval = -1;
	char *line = strdup(optarg);

	/* White spaces possible before option name,
	 * if the command line option is passed between the quotes */
	for (name = line; *name == ' ' || *name == '\t'; name++); 

	/* Command line option separator is '=' */
	retval = get_name_value(&name, &value, '=');
	if (retval == 0) {
		/* Line number param is for conf file, pass 0 for command line options */
		retval = set_option(opts, argv0, name, value, natengine, 0);
	}

	if (line) {
		free(line);
	}
	return retval;
}

int
opts_load_conffile(opts_t *opts, const char *argv0, char **natengine)
{
	int retval, line_num;
	char *line, *name, *value;
	size_t line_len;
	FILE *f;
	
	f = fopen(opts->conffile, "r");
	if (!f) {
		fprintf(stderr, "Error opening conf file '%s': %s\n", opts->conffile, strerror(errno));
		return -1;
	}

	line = NULL;
	line_num = 0;
	retval = -1;
	while (!feof(f)) {
		if (getline(&line, &line_len, f) == -1) {
			break;
		}
		if (line == NULL) {
			fprintf(stderr, "Error in conf file: getline() returns NULL line after line %d\n", line_num);
			goto leave;
		}
		line_num++;

		/* Skip white space */
		for (name = line; *name == ' ' || *name == '\t'; name++); 

		/* Skip comments and empty lines */
		if ((name[0] == '\0') || (name[0] == '#') || (name[0] == ';') ||
			(name[0] == '\r') || (name[0] == '\n')) {
			continue;
		}

		retval = get_name_value(&name, &value, ' ');
		if (retval == 0) {
			retval = set_option(opts, argv0, name, value, natengine, line_num);
		}

		if (retval == -1) {
			goto leave;
		}
	}

leave:
	fclose(f);
	if (line) {
		free(line);
	}
	return retval;
}

/* vim: set noet ft=c: */
