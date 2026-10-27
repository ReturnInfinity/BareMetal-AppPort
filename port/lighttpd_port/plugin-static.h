/* plugin-static.h -- the modules linked into lighttpd.app (LIGHTTPD_STATIC).
 * Must match the mod_*.c files setup.sh compiles. Left out: anything
 * needing fork() (mod_cgi), an optional library this port doesn't have
 * (mod_deflate, mod_magnet, mod_webdav's locking, TLS modules), or
 * AF_UNIX/rrdtool/vhost databases. */
PLUGIN_INIT(mod_access)
PLUGIN_INIT(mod_accesslog)
PLUGIN_INIT(mod_alias)
PLUGIN_INIT(mod_auth)
PLUGIN_INIT(mod_authn_file)
PLUGIN_INIT(mod_dirlisting)
PLUGIN_INIT(mod_evhost)
PLUGIN_INIT(mod_expire)
PLUGIN_INIT(mod_extforward)
PLUGIN_INIT(mod_fastcgi)
PLUGIN_INIT(mod_indexfile)
PLUGIN_INIT(mod_proxy)
PLUGIN_INIT(mod_redirect)
PLUGIN_INIT(mod_rewrite)
PLUGIN_INIT(mod_scgi)
PLUGIN_INIT(mod_setenv)
PLUGIN_INIT(mod_simple_vhost)
PLUGIN_INIT(mod_staticfile)
PLUGIN_INIT(mod_status)
