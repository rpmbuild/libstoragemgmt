/*
 * Copyright (C) 2011-2014 Red Hat, Inc.
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Author: tasleson
 */

#include <libstoragemgmt/libstoragemgmt.h>
#include <libstoragemgmt/libstoragemgmt_error.h>
#include <libstoragemgmt/libstoragemgmt_plug_interface.h>
#include <libstoragemgmt/libstoragemgmt_types.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <libxml/uri.h>

#include "lsm_datatypes.hpp"
#include "lsm_convert.hpp"

/**
 * Common code to validate and initialize the connection.
 */
#define CONN_SETUP(c)   do {            \
    if(!LSM_IS_CONNECT(c)) {            \
        return LSM_ERR_INVALID_CONN;    \
    }                                   \
    lsm_error_free(c->error);             \
    c->error = NULL;                    \
    } while (0)

/**
 * Strings are non null with a len >= 1
 */
#define CHECK_STR(x) ( !(x) || !strlen(x) )

/**
 * When we pass in a pointer for an out value we want to make sure that
 * the pointer isn't null, and that the dereferenced value is != NULL to prevent
 * memory leaks.
 */
#define CHECK_RP(x)  (!(x) || *(x) != NULL)

int lsm_connect_password(const char *uri, const char *password,
                        lsm_connect **conn, uint32_t timeout, lsm_error_ptr *e,
                        lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    lsm_connect *c = NULL;

    /* Password is optional */
    if(  CHECK_STR(uri) || CHECK_RP(conn) || !timeout || CHECK_RP(e) ||
         LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    c = connection_get();
    if(c) {
        c->uri = xmlParseURI(uri);
        if( c->uri && c->uri->scheme ) {
            c->raw_uri = strdup(uri);
            if( c->raw_uri ) {
                rc = driver_load(c, c->uri->scheme, password, timeout, e, 1,
                                flags);
                if( rc == LSM_ERR_OK ) {
                    *conn = (lsm_connect *)c;
                }
            } else {
                rc = LSM_ERR_NO_MEMORY;
            }
        } else {
            rc = LSM_ERR_INVALID_URI;
        }

        /*If we fail for any reason free resources associated with connection*/
        if( rc != LSM_ERR_OK ) {
            connection_free(c);
        }
    } else {
        rc = LSM_ERR_NO_MEMORY;
    }
    return rc;
}

static int lsmErrorLog(lsm_connect *c, lsm_error_ptr error)
{
    if (!LSM_IS_CONNECT(c)) {
        return LSM_ERR_INVALID_CONN;
    }

    if (!LSM_IS_ERROR(error)) {
        return LSM_ERR_INVALID_ERR;
    }

    if (c->error) {
        lsm_error_free(c->error);
        c->error = NULL;
    }

    c->error = error;
    return LSM_ERR_OK;
}

static lsm_error_number logException(lsm_connect *c, lsm_error_number error,
                                const char *message, const char *exception_msg)
{
    lsm_error_ptr err = lsm_error_create(error, LSM_ERR_DOMAIN_FRAME_WORK,
                                        LSM_ERR_LEVEL_ERROR, message,
                                        exception_msg, NULL,
                                        NULL, 0);
    if( err ) {
        lsmErrorLog(c, err);
    }
    return error;
}

static int rpc(lsm_connect *c, const char *method, const Value &parameters,
                Value &response) throw ()
{
    try {
        response = c->tp->rpc(method,parameters);
    } catch ( const ValueException &ve ) {
        return logException(c, LSM_ERR_TRANSPORT_SERIALIZATION, "Serialization error",
                            ve.what());
    } catch ( const LsmException &le ) {
        return logException(c, (lsm_error_number)le.error_code, le.what(),
                            NULL);
    } catch ( const EOFException &eof ) {
        return logException(c, LSM_ERR_TRANSPORT_COMMUNICATION, "Plug-in died",
                                "Check syslog");
    } catch (...) {
        return logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected exception",
                            "Unknown exception");
    }
    return LSM_ERR_OK;
}

static int jobCheck( lsm_connect *c, int rc, Value &response, char **job )
{
    try {
        if( LSM_ERR_OK == rc ) {
            //We get a value back, either null or job id.
            if( Value::string_t == response.valueType() ) {
                *job = strdup(response.asString().c_str());

                if( *job ) {
                    rc = LSM_ERR_JOB_STARTED;
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            } else {
                *job = NULL;
            }
        }
    } catch (const ValueException &ve) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Wrong type",
                            ve.what());
    }
    return rc;
}

static int getAccessGroups( lsm_connect *c, int rc, Value &response,
                            lsm_access_group **groups[], uint32_t *count)
{
    try {
        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            *groups = value_to_access_group_list(response, count);
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_connect_close(lsm_connect *c, lsm_flag flags)
{
    CONN_SETUP(c);

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["flags"] = Value(flags);
    Value parameters(p);
    Value response;

    //No response data needed on shutdown.
    int rc = rpc(c, "shutdown", parameters, response);

    //Free the connection.
    connection_free(c);
    return rc;
}

int lsm_plugin_info_get(lsm_connect *c, char **desc,
                                        char **version, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    if( CHECK_RP(desc) || CHECK_RP(version) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "plugin_info", parameters, response);

        if( rc == LSM_ERR_OK ) {
            std::vector<Value> j = response.asArray();
            *desc = strdup(j[0].asC_str());
            *version = strdup(j[1].asC_str());

            if( !*desc || !*version ) {
                rc = LSM_ERR_NO_MEMORY;
                free(*desc);
                free(*version);
            }
        }
    } catch (const ValueException &ve) {
        free(*desc);
        *desc = NULL;
        free(*version);
        *version = NULL;
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }

    return rc;
}

int lsm_available_plugins_list(const char *sep,
                                            lsm_string_list **plugins,
                                            lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    DIR *dirp = NULL;
    struct dirent *dp = NULL;
    lsm_connect *c = NULL;
    lsm_error_ptr e = NULL;
    char *desc = NULL;
    char *version = NULL;
    char *s = NULL;
    const char *uds_dir = uds_path();
    lsm_string_list *plugin_list = NULL;

    if( CHECK_STR(sep) || CHECK_RP(plugins) || LSM_FLAG_UNUSED_CHECK(flags)) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    plugin_list = lsm_string_list_alloc(0);
    if( !plugin_list ) {
        return LSM_ERR_NO_MEMORY;
    }

    dirp = opendir(uds_dir);
    if( dirp ) {
        for(;;) {
            dp = readdir(dirp);
            if( NULL == dp ) {
                break;
            }

            // Check to see if we have a socket
            if( DT_SOCK == dp->d_type ) {
                c = connection_get();
                if ( c ) {
                    rc = driver_load(c, dp->d_name, NULL, 30000, &e, 0, 0);
                    if( LSM_ERR_OK == rc) {
                        // Get the plugin information
                        rc = lsm_plugin_info_get(c, &desc, &version, 0);
                        if( LSM_ERR_OK == rc) {
                            int format = asprintf(&s, "%s%s%s", desc, sep, version);
                            free(desc);
                            desc = NULL;
                            free(version);
                            version = NULL;

                            if( -1 == format ) {
                                rc = LSM_ERR_NO_MEMORY;
                                break;
                            }

                            rc = lsm_string_list_append(plugin_list, s);
                            free(s);
                            s = NULL;
                            if( LSM_ERR_OK != rc ) {
                                break;
                            }

                        }
                    } else {
                        break;
                    }

                    connection_free(c);
                    c = NULL;
                }
            }
        }   /* for(;;) */

        if( e ) {
            lsm_error_free(e);
            e = NULL;
        }

        if( c ) {
           connection_free(c);
           c = NULL;
        }


        if( -1 == closedir(dirp)) {
            //log the error
            rc = LSM_ERR_INTERNAL_ERROR;
        }

    } else {  /* If dirp == NULL */
        //Log the error
        rc = LSM_ERR_INTERNAL_ERROR;
    }

    if (LSM_ERR_OK == rc) {
        *plugins = plugin_list;
    } else {
        lsm_string_list_free(plugin_list);
        plugin_list = NULL;
    }

    return rc;
}

int lsm_connect_timeout_set(lsm_connect *c, uint32_t timeout, lsm_flag flags)
{
    CONN_SETUP(c);

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["ms"] = Value(timeout);
    p["flags"] = Value(flags);
    Value parameters(p);
    Value response;

    //No response data needed on set time out.
    return rpc(c, "time_out_set", parameters, response);
}

int lsm_connect_timeout_get(lsm_connect *c, uint32_t *timeout, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "time_out_get", parameters, response);
        if( rc == LSM_ERR_OK ) {
            *timeout = response.asUint32_t();
        }
    }
    catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

static int jobStatus( lsm_connect *c, const char *job,
                        lsm_job_status *status, uint8_t *percentComplete,
                        Value &returned_value, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !job || !status || !percentComplete ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["job_id"] = Value(job);
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "job_status", parameters, response);
        if( LSM_ERR_OK == rc ) {
            //We get back an array [status, percent, volume]
            std::vector<Value> j = response.asArray();
            *status = (lsm_job_status)j[0].asInt32_t();
            *percentComplete = (uint8_t)j[1].asUint32_t();

            returned_value = j[2];
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_job_status_get(lsm_connect *c, const char *job_id,
                    lsm_job_status *status, uint8_t *percentComplete,
                    lsm_flag flags)
{
    CONN_SETUP(c);

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    Value rv;
    return jobStatus(c, job_id, status, percentComplete, rv, flags);
}

int lsm_job_status_pool_get(lsm_connect *c,
                                const char *job, lsm_job_status *status,
                                uint8_t *percentComplete, lsm_pool **pool,
                                lsm_flag flags)
{
    Value rv;
    int rc = LSM_ERR_OK;

    CONN_SETUP(c);

    if( CHECK_RP(pool) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {

        rc = jobStatus(c, job, status, percentComplete, rv, flags);

        if( LSM_ERR_OK == rc ) {
            if( Value::object_t ==  rv.valueType() ) {
                *pool = value_to_pool(rv);
            } else {
                *pool = NULL;
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_job_status_volume_get( lsm_connect *c, const char *job,
                        lsm_job_status *status, uint8_t *percentComplete,
                        lsm_volume **vol, lsm_flag flags)
{
    Value rv;
    int rc = LSM_ERR_OK;

    CONN_SETUP(c);

    if( CHECK_RP(vol) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {

        rc = jobStatus(c, job, status, percentComplete, rv, flags);

        if( LSM_ERR_OK == rc ) {
            if( Value::object_t ==  rv.valueType() ) {
                *vol = value_to_volume(rv);
            } else {
                *vol = NULL;
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_job_status_fs_get(lsm_connect *c, const char *job,
                                lsm_job_status *status, uint8_t *percentComplete,
                                lsm_fs **fs, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    Value rv;

    if( CHECK_RP(fs) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {

        rc = jobStatus(c, job, status, percentComplete, rv, flags);

        if( LSM_ERR_OK == rc ) {
            if( Value::object_t ==  rv.valueType() ) {
                *fs = value_to_fs(rv);
            } else {
                *fs = NULL;
            }
        }
    } catch( const ValueException &ve) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_job_status_ss_get(lsm_connect *c, const char *job,
                                lsm_job_status *status, uint8_t *percentComplete,
                                lsm_fs_ss **ss, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    Value rv;

    if( CHECK_RP(ss) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {

        rc = jobStatus(c, job, status, percentComplete, rv, flags);

        if( LSM_ERR_OK == rc ) {
            if( Value::object_t ==  rv.valueType() ) {
                *ss = value_to_ss(rv);
            } else {
                *ss = NULL;
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_job_free(lsm_connect *c, char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( job == NULL || strlen(*job) < 1 || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["job_id"] = Value(*job);
    p["flags"] = Value(flags);
    Value parameters(p);
    Value response;

    int rc = rpc(c, "job_free", parameters, response);

    if( LSM_ERR_OK == rc ) {
        /* Free the memory for the job id */
        free(*job);
        *job = NULL;
    }
    return rc;
}

int lsm_capabilities(lsm_connect *c, lsm_system *system,
                    lsm_storage_capabilities **cap, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !LSM_IS_SYSTEM(system) ) {
        return LSM_ERR_INVALID_SYSTEM;
    }

    if( CHECK_RP(cap) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;

    p["system"] = system_to_value(system);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    try {
        rc = rpc(c, "capabilities", parameters, response);

        if( LSM_ERR_OK == rc && Value::object_t == response.valueType() ) {
            *cap = value_to_capabilities(response);
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }

    return rc;
}

int lsm_pool_list(lsm_connect *c, lsm_pool **poolArray[],
                        uint32_t *count, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !poolArray || !count || CHECK_RP(poolArray) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "pools", parameters, response);
        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            std::vector<Value> pools = response.asArray();

            *count = pools.size();

            if( pools.size() ) {
                *poolArray = lsm_pool_record_array_alloc(pools.size());

                for( size_t i = 0; i < pools.size(); ++i ) {
                    (*poolArray)[i] = value_to_pool(pools[i]);
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
        if( *poolArray && *count ) {
            lsm_pool_record_array_free(*poolArray, *count);
            *poolArray = NULL;
            *count = 0;
        }
    }
    return rc;
}

static int get_initiator_array(lsm_connect *c, int rc, Value &response,
                                lsm_initiator **initiators[], uint32_t *count)
{
    try {
        *count = 0;

        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            std::vector<Value> inits = response.asArray();

            *count = inits.size();

            if( inits.size() ) {

                *initiators = lsm_initiator_record_array_alloc(inits.size());

                if( *initiators ) {
                    for( size_t i = 0; i < inits.size(); ++i ) {
                        (*initiators)[i] = value_to_initiator(inits[i]);
                    }
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            }
        }
    } catch ( const ValueException &ve ) {
        if( *initiators && *count ) {
            lsm_initiator_record_array_free(*initiators, *count);
            *initiators = NULL;
            *count = 0;
        }

        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_initiator_list(lsm_connect *c, lsm_initiator **initiators[],
                                uint32_t *count, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !initiators || !count || CHECK_RP(initiators) ||
        LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["flags"] = Value(flags);
    Value parameters(p);
    Value response;

    int rc = rpc(c, "initiators", parameters, response);
    return get_initiator_array(c, rc, response, initiators, count);
}

static int get_volume_array(lsm_connect *c, int rc, Value &response,
                            lsm_volume **volumes[], uint32_t *count)
{
    if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
        rc = value_array_to_volumes(response, volumes, count);

        if( LSM_ERR_OK != rc ) {
            rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type", NULL);
        }
    }
    return rc;
}


int lsm_volume_list(lsm_connect *c, lsm_volume **volumes[], uint32_t *count,
                    lsm_flag flags)
{
    CONN_SETUP(c);

    if( !volumes || !count || CHECK_RP(volumes) ||
        LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "volumes", parameters, response);
    return get_volume_array(c, rc, response, volumes, count);
}

static int get_disk_array(lsm_connect *c, int rc, Value &response,
                            lsm_disk **disks[], uint32_t *count)
{
    if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
        rc = value_array_to_disks(response, disks, count);

        if( LSM_ERR_OK != rc ) {
            rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type", NULL);
        }
    }

    return rc;
}

int lsm_disk_list(lsm_connect *c, lsm_disk **disks[],
                uint32_t *count, lsm_flag flags)
{
    CONN_SETUP(c);

    if (CHECK_RP(disks) || !count ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "disks", parameters, response);
    return get_disk_array(c, rc, response, disks, count);
}

typedef void* (*convert)(Value &v);

static void* parse_job_response(lsm_connect *c, Value response, int &rc,
                                char **job, convert conv)
{
    void *val = NULL;

    try {
        //We get an array back. first value is job, second is data of interest.
        if( Value::array_t == response.valueType() ) {
            std::vector<Value> r = response.asArray();
            if( Value::string_t == r[0].valueType()) {
                *job = strdup((r[0].asString()).c_str());
                if( *job ) {
                    rc = LSM_ERR_JOB_STARTED;
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }

                rc = LSM_ERR_JOB_STARTED;
            }
            if( Value::object_t == r[1].valueType() ) {
                val = conv(r[1]);
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
        free(*job);
        *job = NULL;
    }
    return val;
}

static int valid_pool_raid_type(lsm_pool_raid_type validate)
{
    switch(validate) {
        case (LSM_POOL_RAID_TYPE_0):
        case (LSM_POOL_RAID_TYPE_1):
        case (LSM_POOL_RAID_TYPE_3):
        case (LSM_POOL_RAID_TYPE_5):
        case (LSM_POOL_RAID_TYPE_6):
        case (LSM_POOL_RAID_TYPE_10):
        case (LSM_POOL_RAID_TYPE_15):
        case (LSM_POOL_RAID_TYPE_16):
        case (LSM_POOL_RAID_TYPE_50):
        case (LSM_POOL_RAID_TYPE_60):
        case (LSM_POOL_RAID_TYPE_51):
        case (LSM_POOL_RAID_TYPE_61):
        case (LSM_POOL_RAID_TYPE_JBOD):
        case (LSM_POOL_RAID_TYPE_UNKNOWN):
        case (LSM_POOL_RAID_TYPE_NOT_APPLICABLE):
        case (LSM_POOL_RAID_TYPE_MIXED):
            break;
        default:
            return 0;
    }
    return 1;
}

static int valid_pool_member_type(lsm_pool_member_type validate)
{
    switch(validate) {
        case (LSM_POOL_MEMBER_TYPE_UNKNOWN):
        case (LSM_POOL_MEMBER_TYPE_DISK):
        case (LSM_POOL_MEMBER_TYPE_POOL):
        case (LSM_POOL_MEMBER_TYPE_VOLUME):
        case (LSM_POOL_MEMBER_TYPE_DISK_MIX):
        case (LSM_POOL_MEMBER_TYPE_DISK_ATA):
        case (LSM_POOL_MEMBER_TYPE_DISK_SATA):
        case (LSM_POOL_MEMBER_TYPE_DISK_SAS):
        case (LSM_POOL_MEMBER_TYPE_DISK_FC):
        case (LSM_POOL_MEMBER_TYPE_DISK_SOP):
        case (LSM_POOL_MEMBER_TYPE_DISK_SCSI):
        case (LSM_POOL_MEMBER_TYPE_DISK_NL_SAS):
        case (LSM_POOL_MEMBER_TYPE_DISK_HDD):
        case (LSM_POOL_MEMBER_TYPE_DISK_SSD):
        case (LSM_POOL_MEMBER_TYPE_DISK_HYBRID):
            break;
        default:
            return 0;
    }
    return 1;
}

int lsm_pool_create(lsm_connect *c, lsm_system *system,
                            const char *pool_name, uint64_t size_bytes,
                            lsm_pool_raid_type raid_type,
                            lsm_pool_member_type member_type, lsm_pool** pool,
                            char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_SYSTEM(system) ) {
        return LSM_ERR_INVALID_SYSTEM;
    }

    if( CHECK_STR(pool_name) || !size_bytes ||
        CHECK_RP(pool)|| CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ||
        !valid_pool_raid_type(raid_type) ||
        !valid_pool_member_type(member_type)) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["system"] = system_to_value(system);
    p["pool_name"] = Value(pool_name);
    p["size_bytes"] = Value(size_bytes);
    p["raid_type"] = Value((int32_t)raid_type);
    p["member_type"] = Value((int32_t)member_type);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "pool_create", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *pool = (lsm_pool *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_pool);
    }
    return rc;
}


static int lsm_pool_create_from(lsm_connect *c,
                        lsm_system *system, const char *pool_name,
                        std::vector<Value> &member_ids, lsm_pool_raid_type raid_type,
                        lsm_pool** pool, char **job, lsm_flag flags,
                        const char *member_id, const char *method)
{
    CONN_SETUP(c);

    if( !LSM_IS_SYSTEM(system) ) {
        return LSM_ERR_INVALID_SYSTEM;
    }

    if( CHECK_STR(pool_name) ||
        CHECK_RP(pool)|| CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ||
        !valid_pool_raid_type(raid_type) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }


    std::map<std::string, Value> p;
    p["system"] = system_to_value(system);
    p["pool_name"] = Value(pool_name);
    p[member_id] = Value(member_ids);
    p["raid_type"] = Value((int32_t)raid_type);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, method, parameters, response);
    if( LSM_ERR_OK == rc ) {
        *pool = (lsm_pool *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_pool);
    }
    return rc;
}

int LSM_DLL_EXPORT lsm_pool_create_from_disks(lsm_connect *c,
                        lsm_system *system, const char *pool_name,
                        lsm_disk *disks[], uint32_t num_disks,
                        lsm_pool_raid_type raid_type,
                        lsm_pool** pool, char **job, lsm_flag flags)
{
    uint32_t i;

    CONN_SETUP(c);

    if( !disks || !num_disks ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    /* Create disks container */
    std::vector<Value> d;
    for( i = 0; i < num_disks; ++i ) {
        d.push_back(disk_to_value(disks[i]));
    }

    return lsm_pool_create_from(c, system, pool_name, d, raid_type, pool, job,
        flags, "disks", "pool_create_from_disks");

}

int LSM_DLL_EXPORT lsm_pool_create_from_volumes(lsm_connect *c,
                        lsm_system *system, const char *pool_name,
                        lsm_volume *volumes[], uint32_t num_volumes,
                        lsm_pool_raid_type raid_type,
                        lsm_pool** pool, char **job, lsm_flag flags)
{
    uint32_t i;

    if( !volumes || !num_volumes ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    /* Create disks container */
    std::vector<Value> vols;
    for( i = 0; i < num_volumes; ++i ) {
        vols.push_back(volume_to_value(volumes[i]));
    }

    return lsm_pool_create_from(c, system, pool_name, vols, raid_type,
                                pool, job, flags, "volumes",
                                "pool_create_from_volumes");
}


 int lsm_pool_create_from_pool(lsm_connect *c, lsm_system *system,
                        const char *pool_name, lsm_pool *pool,
                        uint64_t size_bytes, lsm_pool **created_pool, char **job,
                        lsm_flag flags)
 {
    CONN_SETUP(c);

    if( !LSM_IS_SYSTEM(system) ) {
        return LSM_ERR_INVALID_SYSTEM;
    }

    if( !LSM_IS_POOL(pool) ) {
        return LSM_ERR_INVALID_POOL;
    }

    if( CHECK_STR(pool_name) ||
        !size_bytes || CHECK_RP(created_pool)|| CHECK_RP(job) ||
        LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["system"] = system_to_value(system);
    p["pool_name"] = Value(pool_name);
    p["size_bytes"] = Value(size_bytes);
    p["pool"] = pool_to_value(pool);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "pool_create_from_pool", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *created_pool = (lsm_pool *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_pool);
    }
    return rc;
 }

int lsm_pool_delete(lsm_connect *c, lsm_pool *pool, char **job, lsm_flag flags)
{
    int rc;
    CONN_SETUP(c);

    if( !LSM_IS_POOL(pool) ) {
        return LSM_ERR_INVALID_POOL;
    }

    if (CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {

        std::map<std::string, Value> p;
        p["pool"] = pool_to_value(pool);
        p["flags"] = Value(flags);

        Value parameters(p);
        Value response;

        rc = rpc(c, "pool_delete", parameters, response);
        if( LSM_ERR_OK == rc ) {
            //We get a value back, either null or job id.
            if( Value::string_t == response.valueType() ) {
                *job = strdup(response.asString().c_str());

                if( *job ) {
                    rc = LSM_ERR_JOB_STARTED;
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
 }

int lsm_volume_create(lsm_connect *c, lsm_pool *pool, const char *volumeName,
                        uint64_t size, lsm_provision_type provisioning,
                        lsm_volume **newVolume, char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_POOL(pool)) {
        return LSM_ERR_INVALID_POOL;
    }

    if( CHECK_STR(volumeName) || !size || CHECK_RP(newVolume) ||
        CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ){
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["pool"] = pool_to_value(pool);
    p["volume_name"] = Value(volumeName);
    p["size_bytes"] = Value(size);
    p["provisioning"] = Value((int32_t)provisioning);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "volume_create", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *newVolume = (lsm_volume *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_volume);
    }
    return rc;
}

int lsm_volume_resize(lsm_connect *c, lsm_volume *volume,
                        uint64_t newSize, lsm_volume **resizedVolume,
                        char **job, lsm_flag flags )
{
    CONN_SETUP(c);

    if( !LSM_IS_VOL(volume)) {
        return LSM_ERR_INVALID_VOL;
    }

    if( !newSize || CHECK_RP(resizedVolume) || CHECK_RP(job)
        || newSize == 0 || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    //If you try to resize to same size, we will return error.
    if( ( newSize/volume->block_size) == volume->number_of_blocks ) {
        return LSM_ERR_VOLUME_SAME_SIZE;
    }

    std::map<std::string, Value> p;
    p["volume"] = volume_to_value(volume);
    p["new_size_bytes"] = Value(newSize);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "volume_resize", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *resizedVolume = (lsm_volume *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_volume);
    }
    return rc;
}

int lsm_volume_replicate(lsm_connect *c, lsm_pool *pool,
                        lsm_replication_type repType, lsm_volume *volumeSrc,
                        const char *name, lsm_volume **newReplicant,
                        char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( pool && !LSM_IS_POOL(pool) ) {
        return LSM_ERR_INVALID_POOL;
    }

    if( !LSM_IS_VOL(volumeSrc) ) {
        return LSM_ERR_INVALID_VOL;
    }

    if(CHECK_STR(name) || CHECK_RP(newReplicant) || CHECK_RP(job) ||
        LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["pool"] = pool_to_value(pool);
    p["rep_type"] = Value((int32_t)repType);
    p["volume_src"] = volume_to_value(volumeSrc);
    p["name"] = Value(name);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "volume_replicate", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *newReplicant = (lsm_volume *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_volume);
    }
    return rc;

}

int lsm_volume_replicate_range_block_size(lsm_connect *c, lsm_system *system,
                                        uint32_t *bs, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !bs || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    if( !LSM_IS_SYSTEM(system) ) {
        return LSM_ERR_INVALID_SYSTEM;
    }

    try {
        std::map<std::string, Value> p;
        p["system"] = system_to_value(system);
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "volume_replicate_range_block_size", parameters, response);
        if( LSM_ERR_OK == rc ) {
            if( Value::numeric_t == response.valueType() ) {
                *bs = response.asUint32_t();
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}


int lsm_volume_replicate_range(lsm_connect *c,
                                                lsm_replication_type repType,
                                                lsm_volume *source,
                                                lsm_volume *dest,
                                                lsm_block_range **ranges,
                                                uint32_t num_ranges,
                                                char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_VOL(source) || !LSM_IS_VOL(dest) ) {
        return LSM_ERR_INVALID_VOL;
    }

    if( !ranges || !num_ranges || CHECK_RP(job) ||
        LSM_FLAG_UNUSED_CHECK(flags)) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["rep_type"] = Value((int32_t)repType);
    p["volume_src"] = volume_to_value(source);
    p["volume_dest"] = volume_to_value(dest);
    p["ranges"] = block_range_list_to_value(ranges, num_ranges);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "volume_replicate_range", parameters, response);
    return jobCheck(c, rc, response, job);
}

int lsm_volume_delete(lsm_connect *c, lsm_volume *volume, char **job,
                    lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !LSM_IS_VOL(volume) ) {
        return LSM_ERR_INVALID_VOL;
    }

    if( CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["volume"] = volume_to_value(volume);
        p["flags"] = Value(flags);

        Value parameters(p);
        Value response;

        rc = rpc(c, "volume_delete", parameters, response);
        if( LSM_ERR_OK == rc ) {
            //We get a value back, either null or job id.
            if( Value::string_t == response.valueType() ) {
                *job = strdup(response.asString().c_str());

                if( *job ) {
                    rc = LSM_ERR_JOB_STARTED;
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;

}

int lsm_iscsi_chap_auth(lsm_connect *c, lsm_initiator *initiator,
                                const char *username, const char *password,
                                const char *out_user, const char *out_password,
                                lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_INIT(initiator) ) {
        return LSM_ERR_INVALID_INIT;
    }

    if(LSM_FLAG_UNUSED_CHECK(flags)) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["initiator"] = initiator_to_value(initiator);
    p["in_user"] = Value(username);
    p["in_password"] = Value(password);
    p["out_user"] = Value(out_user);
    p["out_password"] = Value(out_password);
    p["flags"] = Value(flags);


    Value parameters(p);
    Value response;

    return rpc(c, "iscsi_chap_auth", parameters, response);
}

int lsm_initiator_grant(lsm_connect *c,
                    const char *initiator_id,
                    lsm_initiator_type initiator_type,
                    lsm_volume *volume,
                    lsm_access_type access,
                    lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_VOL(volume)){
        return LSM_ERR_INVALID_VOL;
    }

    if( CHECK_STR(initiator_id) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["initiator_id"] = Value(initiator_id);
    p["initiator_type"] = Value((int32_t)initiator_type);
    p["volume"] = volume_to_value(volume);
    p["access"] = Value((int32_t)access);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    return rpc(c, "initiator_grant", parameters, response);
}

int lsm_initiator_revoke( lsm_connect *c, lsm_initiator *i, lsm_volume *v,
                        lsm_flag flags)
{
    CONN_SETUP(c);

    if(!LSM_IS_INIT(i)) {
        return LSM_ERR_INVALID_INIT;
    }

    if( !LSM_IS_VOL(v) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_VOL;
    }

    std::map<std::string, Value> p;
    p["initiator"] = initiator_to_value(i);
    p["volume"] = volume_to_value(v);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    return rpc(c, "initiator_revoke", parameters, response);
}

int lsm_volumes_accessible_by_initiator(lsm_connect *c,
                                        lsm_initiator *initiator,
                                        lsm_volume **volumes[],
                                        uint32_t *count, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_INIT(initiator) ){
        return LSM_ERR_INVALID_INIT;
    }

    if( CHECK_RP(volumes) || !count || *count != 0 ||
        LSM_FLAG_UNUSED_CHECK(flags) ){
        return LSM_ERR_INVALID_ARGUMENT;
    }


    std::map<std::string, Value> p;
    p["initiator"] = initiator_to_value(initiator);
    p["flags"] = Value(flags);
    Value parameters(p);
    Value response;

    int rc = rpc(c, "volumes_accessible_by_initiator", parameters, response);
    return get_volume_array(c, rc, response, volumes, count);
}

int lsm_initiators_granted_to_volume(lsm_connect *c, lsm_volume *volume,
                                lsm_initiator **initiators[], uint32_t *count,
                                lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_VOL(volume) ) {
        return LSM_ERR_INVALID_VOL;
    }

    if( CHECK_RP(initiators) || !count || LSM_FLAG_UNUSED_CHECK(flags)) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["volume"] = volume_to_value(volume);
    p["flags"] = Value(flags);
    Value parameters(p);
    Value response;

    int rc = rpc(c, "initiators_granted_to_volume", parameters, response);
    return get_initiator_array(c, rc, response, initiators, count);
}


static int online_offline(lsm_connect *c, lsm_volume *v,
                            const char* operation, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_VOL(v)) {
        return LSM_ERR_INVALID_VOL;
    }

    if ( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["volume"] = volume_to_value(v);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;
    return rpc(c, operation, parameters, response);
}

int lsm_volume_online(lsm_connect *c, lsm_volume *volume, lsm_flag flags)
{
    return online_offline(c, volume, "volume_online", flags);
}

int lsm_volume_offline(lsm_connect *c, lsm_volume *volume, lsm_flag flags)
{
    return online_offline(c, volume, "volume_offline", flags);
}

int lsm_access_group_list( lsm_connect *c, lsm_access_group **groups[],
                        uint32_t *groupCount, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !groups || !groupCount || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["flags"] = Value(flags);
    Value parameters(p);
    Value response;

    int rc = rpc(c, "access_group_list", parameters, response);
    return getAccessGroups(c, rc, response, groups, groupCount);
}

int lsm_access_group_create(lsm_connect *c, const char *name,
                            const char *initiator_id, lsm_initiator_type id_type,
                            const char *system_id,
                            lsm_access_group **access_group, lsm_flag flags)
{
    CONN_SETUP(c);

    if( CHECK_STR(name) || CHECK_STR(initiator_id) || CHECK_STR(system_id)
        || CHECK_RP(access_group) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["name"] = Value(name);
    p["initiator_id"] = Value(initiator_id);
    p["id_type"] = Value((int32_t)id_type);
    p["system_id"] = Value(system_id);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    *access_group = NULL;

    int rc = rpc(c, "access_group_create", parameters, response);
    if( LSM_ERR_OK == rc ) {
        //We should be getting a value back.
        if( Value::object_t == response.valueType() ) {
            *access_group = value_to_access_group(response);
        }
    }
    return rc;
}

int lsm_access_group_delete(lsm_connect *c, lsm_access_group *group, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_ACCESS_GROUP(group) ){
        return LSM_ERR_INVALID_ACCESS_GROUP;
    }

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["group"] = access_group_to_value(group);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    return rpc(c, "access_group_del", parameters, response);
}

int lsm_access_group_initiator_add(lsm_connect *c,
                                lsm_access_group *group,
                                const char *initiator_id,
                                lsm_initiator_type id_type,
                                lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_ACCESS_GROUP(group) ) {
        return LSM_ERR_INVALID_ACCESS_GROUP;
    }

    if( CHECK_STR(initiator_id) ||
        LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["group"] = access_group_to_value(group);
    p["initiator_id"] = initiator_id;
    p["id_type"] = Value((int32_t)id_type);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    return rpc(c, "access_group_add_initiator", parameters, response);
}

int lsm_access_group_initiator_delete(lsm_connect *c, lsm_access_group *group,
                                const char* initiator_id, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_ACCESS_GROUP(group)) {
        return LSM_ERR_INVALID_ACCESS_GROUP;
    }

    if( CHECK_STR(initiator_id) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["group"] = access_group_to_value(group);
    p["initiator_id"] = Value(initiator_id);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    return rpc(c, "access_group_del_initiator", parameters, response);
}

int lsm_access_group_grant(lsm_connect *c, lsm_access_group *group,
                                            lsm_volume *volume,
                                            lsm_access_type access,
                                            lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_ACCESS_GROUP(group)) {
        return LSM_ERR_INVALID_ACCESS_GROUP;
    }

    if( !LSM_IS_VOL(volume) ) {
        return LSM_ERR_INVALID_VOL;
    }

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["group"] = access_group_to_value(group);
    p["volume"] = volume_to_value(volume);
    p["access"] = Value((int32_t)access);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    return rpc(c, "access_group_grant", parameters, response);
}


int lsm_access_group_revoke(lsm_connect *c, lsm_access_group *group,
                                            lsm_volume *volume,
                                            lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_ACCESS_GROUP(group)) {
        return LSM_ERR_INVALID_ACCESS_GROUP;
    }

    if( !LSM_IS_VOL(volume)) {
        return LSM_ERR_INVALID_VOL;
    }

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["group"] = access_group_to_value(group);
    p["volume"] = volume_to_value(volume);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    return rpc(c, "access_group_revoke", parameters, response);
}

int lsm_volumes_accessible_by_access_group(lsm_connect *c,
                                        lsm_access_group *group,
                                        lsm_volume **volumes[],
                                        uint32_t *count, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !LSM_IS_ACCESS_GROUP(group)) {
        return LSM_ERR_INVALID_ACCESS_GROUP;
    }

    if( !volumes || !count || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["group"] = access_group_to_value(group);
        p["flags"] = Value(flags);

        Value parameters(p);
        Value response;

        rc = rpc(c, "volumes_accessible_by_access_group", parameters, response);
        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            std::vector<Value> vol = response.asArray();

            *count = vol.size();

            if( vol.size() ) {
                *volumes = lsm_volume_record_array_alloc(vol.size());

                for( size_t i = 0; i < vol.size(); ++i ) {
                    (*volumes)[i] = value_to_volume(vol[i]);
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
        if( *volumes && *count ) {
            lsm_volume_record_array_free(*volumes, *count);
            *volumes = NULL;
            *count = 0;
        }
    }
    return rc;
}

int lsm_access_groups_granted_to_volume(lsm_connect *c,
                                    lsm_volume *volume,
                                    lsm_access_group **groups[],
                                    uint32_t *groupCount, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_VOL(volume)) {
        return LSM_ERR_INVALID_VOL;
    }

    if( !groups || !groupCount || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["volume"] = volume_to_value(volume);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "access_groups_granted_to_volume", parameters, response);
    return getAccessGroups(c, rc, response, groups, groupCount);
}

int lsm_volume_child_dependency(lsm_connect *c, lsm_volume *volume,
                                uint8_t *yes, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !LSM_IS_VOL(volume)) {
        return LSM_ERR_INVALID_VOL;
    }

    if( !yes || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["volume"] = volume_to_value(volume);
        p["flags"] = Value(flags);

        Value parameters(p);
        Value response;

        *yes = 0;

        rc = rpc(c, "volume_child_dependency", parameters, response);
        if( LSM_ERR_OK == rc ) {
            //We should be getting a boolean value back.
            if( Value::boolean_t == response.valueType() ) {
                if( response.asBool() ) {
                    *yes = 1;
                }
            } else {
                rc = LSM_ERR_INTERNAL_ERROR;
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_volume_child_dependency_delete(lsm_connect *c, lsm_volume *volume,
                                char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_VOL(volume)) {
        return LSM_ERR_INVALID_VOL;
    }

    if( CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["volume"] = volume_to_value(volume);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "volume_child_dependency_rm", parameters, response);
    return jobCheck(c, rc, response, job);
}

int lsm_system_list(lsm_connect *c, lsm_system **systems[],
                    uint32_t *systemCount, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !systems || ! systemCount || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "systems", parameters, response);
        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            std::vector<Value> sys = response.asArray();

            *systemCount = sys.size();

            if( sys.size() ) {
                *systems = lsm_system_record_array_alloc(sys.size());

                if( *systems ) {
                    for( size_t i = 0; i < sys.size(); ++i ) {
                        (*systems)[i] = value_to_system(sys[i]);
                        if( !(*systems)[i] ) {
                            lsm_system_record_array_free(*systems, i);
                            rc = LSM_ERR_NO_MEMORY;
                            break;
                        }
                    }
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
        if( *systems ) {
            lsm_system_record_array_free( *systems, *systemCount);
            *systems = NULL;
            *systemCount = 0;
        }
    }
    return rc;
}

int lsm_fs_list(lsm_connect *c, lsm_fs **fs[], uint32_t *fsCount,
                lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !fs || !fsCount || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "fs", parameters, response);
        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            std::vector<Value> sys = response.asArray();

            *fsCount = sys.size();

            if( sys.size() ) {
                *fs = lsm_fs_record_array_alloc(sys.size());

                if( *fs ) {
                    for( size_t i = 0; i < sys.size(); ++i ) {
                        (*fs)[i] = value_to_fs(sys[i]);
                    }
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
        if( *fs && *fsCount) {
            lsm_fs_record_array_free(*fs, *fsCount);
            *fs = NULL;
            *fsCount = 0;
        }
    }
    return rc;

}

int lsm_fs_create(lsm_connect *c, lsm_pool *pool, const char *name,
                    uint64_t size_bytes, lsm_fs **fs, char **job,
                    lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_POOL(pool)) {
        return LSM_ERR_INVALID_POOL;
    }

    if( CHECK_STR(name) || !size_bytes || CHECK_RP(fs) || CHECK_RP(job)
        || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["pool"] = pool_to_value(pool);
    p["name"] = Value(name);
    p["size_bytes"] = Value(size_bytes);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_create", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *fs = (lsm_fs *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_fs);
    }
    return rc;
}

int lsm_fs_delete(lsm_connect *c, lsm_fs *fs, char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs) ) {
        return LSM_ERR_INVALID_FS;
    }

    if( CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_delete", parameters, response);
    return jobCheck(c, rc, response, job);
}


int lsm_fs_resize(lsm_connect *c, lsm_fs *fs,
                                    uint64_t new_size_bytes, lsm_fs **rfs,
                                    char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs)) {
        return LSM_ERR_INVALID_FS;
    }

    if( !new_size_bytes || CHECK_RP(rfs) || CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["new_size_bytes"] = Value(new_size_bytes);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_resize", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *rfs = (lsm_fs *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_fs);
    }
    return rc;
}

int lsm_fs_clone(lsm_connect *c, lsm_fs *src_fs,
                                    const char *name, lsm_fs_ss *optional_ss,
                                    lsm_fs **cloned_fs,
                                    char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(src_fs)) {
        return LSM_ERR_INVALID_FS;
    }

    if( CHECK_STR(name) || CHECK_RP(cloned_fs) || CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["src_fs"] = fs_to_value(src_fs);
    p["dest_fs_name"] = Value(name);
    p["snapshot"] = ss_to_value(optional_ss);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_clone", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *cloned_fs = (lsm_fs *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_fs);
    }
    return rc;
}

int lsm_fs_file_clone(lsm_connect *c, lsm_fs *fs, const char *src_file_name,
                    const char *dest_file_name, lsm_fs_ss *snapshot, char **job,
                    lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs)) {
        return LSM_ERR_INVALID_FS;
    }

    if( CHECK_STR(src_file_name) || CHECK_STR(dest_file_name)
        || CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["src_file_name"] = Value(src_file_name);
    p["dest_file_name"] = Value(dest_file_name);
    p["snapshot"] = ss_to_value(snapshot);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "file_clone", parameters, response);
    return jobCheck(c, rc, response, job);
}

int lsm_fs_child_dependency( lsm_connect *c, lsm_fs *fs, lsm_string_list *files,
                            uint8_t *yes, lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs) ) {
        return LSM_ERR_INVALID_FS;
    }

    if( files ) {
        if( !LSM_IS_STRING_LIST(files) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

    if( !yes || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["fs"] = fs_to_value(fs);
        p["files"] = string_list_to_value(files);
        p["flags"] = Value(flags);

        Value parameters(p);
        Value response;

        *yes = 0;

        rc = rpc(c, "fs_child_dependency", parameters, response);
        if( LSM_ERR_OK == rc ) {
            //We should be getting a boolean value back.
            if( Value::boolean_t == response.valueType() ) {
                if( response.asBool() ) {
                    *yes = 1;
                }
            } else {
                rc = LSM_ERR_INTERNAL_ERROR;
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
    }
    return rc;
}

int lsm_fs_child_dependency_delete( lsm_connect *c, lsm_fs *fs, lsm_string_list *files,
                            char **job, lsm_flag flags )
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs)) {
        return LSM_ERR_INVALID_FS;
    }

    if( files ) {
        if( !LSM_IS_STRING_LIST(files) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

    if( CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["files"] = string_list_to_value(files);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_child_dependency_rm", parameters, response);
    return jobCheck(c, rc, response, job);
}

int lsm_fs_ss_list(lsm_connect *c, lsm_fs *fs, lsm_fs_ss **ss[],
                                uint32_t *ssCount, lsm_flag flags )
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs) ) {
        return LSM_ERR_INVALID_FS;
    }

    if( CHECK_RP(ss) || !ssCount || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    try {
        rc = rpc(c, "fs_snapshots", parameters, response);
        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            std::vector<Value> sys = response.asArray();

            *ssCount = sys.size();

            if( sys.size() ) {
                *ss = lsm_fs_ss_record_array_alloc(sys.size());

                if( *ss ) {
                    for( size_t i = 0; i < sys.size(); ++i ) {
                        (*ss)[i] = value_to_ss(sys[i]);
                    }
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
        if( *ss && *ssCount ) {
            lsm_fs_ss_record_array_free(*ss, *ssCount);
            *ss = NULL;
            *ssCount = 0;
        }
    }
    return rc;

}

int lsm_fs_ss_create(lsm_connect *c, lsm_fs *fs, const char *name,
                    lsm_string_list *files, lsm_fs_ss **snapshot, char **job,
                    lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs)) {
        return LSM_ERR_INVALID_FS;
    }

    if( files ) {
        if( !LSM_IS_STRING_LIST(files) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

    if( CHECK_STR(name) || CHECK_RP(snapshot) || CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["snapshot_name"] = Value(name);
    p["files"] = string_list_to_value(files);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_snapshot_create", parameters, response);
    if( LSM_ERR_OK == rc ) {
        *snapshot = (lsm_fs_ss *)parse_job_response(c, response, rc, job,
                                                        (convert)value_to_ss);
    }
    return rc;
}

int lsm_fs_ss_delete(lsm_connect *c, lsm_fs *fs, lsm_fs_ss *ss, char **job,
                    lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs) ) {
        return LSM_ERR_INVALID_FS;
    }

    if( !LSM_IS_SS(ss) ) {
        return LSM_ERR_INVALID_SS;
    }

    if( CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["snapshot"] = ss_to_value(ss);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_snapshot_delete", parameters, response);
    return jobCheck(c, rc, response, job);
}

int lsm_fs_ss_revert(lsm_connect *c, lsm_fs *fs, lsm_fs_ss *ss,
                                    lsm_string_list *files,
                                    lsm_string_list *restore_files,
                                    int all_files, char **job, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_FS(fs) ) {
        return LSM_ERR_INVALID_FS;
    }

    if( !LSM_IS_SS(ss) ) {
        return LSM_ERR_INVALID_SS;
    }

    if( files ) {
        if( !LSM_IS_STRING_LIST(files) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

    if( restore_files ) {
        if( !LSM_IS_STRING_LIST(restore_files) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

    if( CHECK_RP(job) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["fs"] = fs_to_value(fs);
    p["snapshot"] = ss_to_value(ss);
    p["files"] = string_list_to_value(files);
    p["restore_files"] = string_list_to_value(restore_files);
    p["all_files"] = Value((all_files)?true:false);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "fs_snapshot_revert", parameters, response);
    return jobCheck(c, rc, response, job);

}

int lsm_nfs_list( lsm_connect *c, lsm_nfs_export **exports[], uint32_t *count,
                lsm_flag flags)
{
    int rc = LSM_ERR_OK;
    CONN_SETUP(c);

    if( CHECK_RP(exports) || !count || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    try {
        std::map<std::string, Value> p;
        p["flags"] = Value(flags);
        Value parameters(p);
        Value response;

        rc = rpc(c, "exports", parameters, response);
        if( LSM_ERR_OK == rc && Value::array_t == response.valueType()) {
            std::vector<Value> exps = response.asArray();

            *count = exps.size();

            if( *count ) {
                *exports = lsm_nfs_export_record_array_alloc(*count);

                if( *exports ) {
                    for( size_t i = 0; i < *count; ++i ) {
                        (*exports)[i] = value_to_nfs_export(exps[i]);
                    }
                } else {
                    rc = LSM_ERR_NO_MEMORY;
                }
            }
        }
    } catch( const ValueException &ve ) {
        rc = logException(c, LSM_ERR_INTERNAL_ERROR, "Unexpected type",
                            ve.what());
        if( *exports && *count ) {
            lsm_nfs_export_record_array_free( *exports, *count );
            *exports = NULL;
            *count = 0;
        }
    }
    return rc;
}

int lsm_nfs_export_fs( lsm_connect *c,
                                        const char *fs_id,
                                        const char *export_path,
                                        lsm_string_list *root_list,
                                        lsm_string_list *rw_list,
                                        lsm_string_list *ro_list,
                                        uint64_t anon_uid,
                                        uint64_t anon_gid,
                                        const char *auth_type,
                                        const char *options,
                                        lsm_nfs_export **exported,
                                        lsm_flag flags
                                        )
{
    CONN_SETUP(c);

    if( root_list ) {
        if( !LSM_IS_STRING_LIST(root_list) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

    if( rw_list ) {
        if( !LSM_IS_STRING_LIST(rw_list) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

     if( ro_list ) {
        if( !LSM_IS_STRING_LIST(ro_list) ) {
            return LSM_ERR_INVALID_SL;
        }
    }

    if( CHECK_STR(fs_id) || CHECK_RP(exported)
        || !(root_list || rw_list || ro_list) || LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;

    p["fs_id"] = Value(fs_id);
    p["export_path"] = Value(export_path);
    p["root_list"] = string_list_to_value(root_list);
    p["rw_list"] = string_list_to_value(rw_list);
    p["ro_list"] = string_list_to_value(ro_list);
    p["anon_uid"] = Value(anon_uid);
    p["anon_gid"] = Value(anon_gid);
    p["auth_type"] = Value(auth_type);
    p["options"] = Value(options);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "export_fs", parameters, response);
    if( LSM_ERR_OK == rc && Value::object_t == response.valueType()) {
        *exported = value_to_nfs_export(response);
    }
    return rc;
}

int lsm_nfs_export_delete( lsm_connect *c, lsm_nfs_export *e, lsm_flag flags)
{
    CONN_SETUP(c);

    if( !LSM_IS_NFS_EXPORT(e) ) {
        return LSM_ERR_INVALID_NFS;
    }

    if( LSM_FLAG_UNUSED_CHECK(flags) ) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    std::map<std::string, Value> p;
    p["export"] = nfs_export_to_value(e);
    p["flags"] = Value(flags);

    Value parameters(p);
    Value response;

    int rc = rpc(c, "export_remove", parameters, response);
    return rc;
}