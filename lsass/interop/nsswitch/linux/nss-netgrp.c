/* Editor Settings: expandtabs and use 4 spaces for indentation
 * ex: set softtabstop=4 tabstop=8 expandtab shiftwidth=4: *
 * -*- mode: c, c-basic-offset: 4 -*- */

/*
 * Copyright © BeyondTrust Software 2004 - 2019
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * BEYONDTRUST MAKES THIS SOFTWARE AVAILABLE UNDER OTHER LICENSING TERMS AS
 * WELL. IF YOU HAVE ENTERED INTO A SEPARATE LICENSE AGREEMENT WITH
 * BEYONDTRUST, THEN YOU MAY ELECT TO USE THE SOFTWARE UNDER THE TERMS OF THAT
 * SOFTWARE LICENSE AGREEMENT INSTEAD OF THE TERMS OF THE APACHE LICENSE,
 * NOTWITHSTANDING THE ABOVE NOTICE.  IF YOU HAVE QUESTIONS, OR WISH TO REQUEST
 * A COPY OF THE ALTERNATE LICENSING TERMS OFFERED BY BEYONDTRUST, PLEASE CONTACT
 * BEYONDTRUST AT beyondtrust.com/contact
 */

/*
 * Copyright (C) BeyondTrust Software. All rights reserved.
 *
 * Module Name:
 *
 *        nss-group.c
 *
 * Abstract:
 *
 *        Name Server Switch (BeyondTrust LSASS)
 *
 *        Handle NSS Group Information
 *
 * Authors: Brian Koropoff (bkoropoff@likewisesoftware.com)
 *
 */

#include "lsanss.h"
#include "nss-netgrp.h"
#include "externs.h"
#include <assert.h>

#define MAX_NUM_ARTEFACTS 500

NSS_STATUS
buffer_alloc(char** buffer, size_t* buflen, size_t needed, char** chunk, int *errnop)
{
    size_t needed_aligned = ((needed / sizeof(size_t)) + 1) * sizeof(size_t);
    if (needed_aligned > *buflen)
    {
        *errnop = ENOMEM;
        return NSS_STATUS_UNAVAIL;
    }
    else
    {
        *chunk = *buffer;
        *buffer += needed_aligned;
        *buflen -= needed_aligned;
        return NSS_STATUS_SUCCESS;
    }
}


NSS_STATUS
_nss_lsass_setnetgrent (
    char *group,
    struct __netgrent *result
    )
{
    NSS_STATUS ret = NSS_STATUS_SUCCESS;
    PSTR pszValue = NULL;

    NSS_LOCK();

    ret = LsaNssCommonNetgroupFindByName(
        &lsaConnection,
        group,
        &pszValue);
    BAIL_ON_NSS_ERROR(ret);

    result->data = pszValue;
    result->data_size = strlen(pszValue);
    result->cursor = result->data;
    result->first = 1;

error:

    NSS_UNLOCK();

    return ret;
}

NSS_STATUS
_nss_lsass_endnetgrent (struct __netgrent * result)
{
    LW_SAFE_FREE_MEMORY(result->data);

    return NSS_STATUS_SUCCESS;
}

static
NSS_STATUS
nss_lsass_parse_entry(
    struct __netgrent* result,
    char** buffer,
    size_t* buflen,
    int* errnop)
{
    NSS_STATUS ret = NSS_STATUS_SUCCESS;
    LSA_NSS_NETGROUP_ENTRY_TYPE type;
    char* host = NULL;
    char* user = NULL;
    char* domain = NULL;
    char* group = NULL;

    ret = LsaNssCommonNetgroupParse(
        &result->cursor,
        &type,
        &host,
        &user,
        &domain,
        &group);
    BAIL_ON_NSS_ERROR(ret);

    switch (type)
    {
    case LSA_NSS_NETGROUP_ENTRY_TRIPLE:
        result->type = triple_val;
        memset(&result->val.triple, 0, sizeof(result->val.triple));

        /* Copy the values into the provided buffer */
        if (host)
        {
            ret = buffer_alloc(buffer, buflen, strlen(host) + 1, (char**) &result->val.triple.host, errnop);
            BAIL_ON_NSS_ERROR(ret);
            strcpy(*((char**)&result->val.triple.host), host);
        }
        if (user)
        {
            ret = buffer_alloc(buffer, buflen, strlen(user) + 1, (char**) &result->val.triple.user, errnop);
            BAIL_ON_NSS_ERROR(ret);
            strcpy(*((char**)&result->val.triple.user), user);
        }
        if (domain)
        {
            ret = buffer_alloc(buffer, buflen, strlen(domain) + 1, (char**) &result->val.triple.domain, errnop);
            BAIL_ON_NSS_ERROR(ret);
            strcpy(*((char**)&result->val.triple.domain), domain);
        }
        break;
    case LSA_NSS_NETGROUP_ENTRY_GROUP:
        result->type = group_val;

        /* Copy the group into the provided buffer */
        ret = buffer_alloc(buffer, buflen, strlen(group) + 1, (char**) &result->val.group, errnop);
        BAIL_ON_NSS_ERROR(ret);
        strcpy(*((char**)&result->val.group), group);
        break;
    case LSA_NSS_NETGROUP_ENTRY_END:
        /* End of entries, return the appropriate status code */
        ret = result->first ? NSS_STATUS_NOTFOUND : NSS_STATUS_RETURN;
        BAIL_ON_NSS_ERROR(ret);
        break;
    }

    result->first = 0;

error:

    return ret;
}

NSS_STATUS
_nss_lsass_getnetgrent_r (struct __netgrent *result,
			 char *buffer, size_t buflen, int *errnop)
{
    NSS_STATUS status = NSS_STATUS_SUCCESS;

    status = nss_lsass_parse_entry(
        result,
        &buffer,
        &buflen,
        errnop);
    BAIL_ON_NSS_ERROR(status);

error:

    return status;
}
