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
 *        main.c
 *
 * Abstract:
 *
 *        BeyondTrust Security and Authentication Subsystem (LSASS)
 *
 *        Test Program for exercising LsaFindGroupById
 *
 * Authors: Krishna Ganugapati (krishnag@likewisesoftware.com)
 *          Sriram Nambakam (snambakam@likewisesoftware.com)
 */

#include "config.h"
#include "lsasystem.h"
#include "lsadef.h"
#include "lsa/lsa.h"

#include "lsaclient.h"
#include "lsaipc.h"
#include "common.h"

#define LW_PRINTF_STRING(x) ((x) ? (x) : "<null>")

static
DWORD
ParseArgs(
    int    argc,
    char*  argv[],
    PDWORD pdwGroupId,
    PLSA_FIND_FLAGS pFindFlags,
    PDWORD pdwInfoLevel,
    PBOOLEAN pbCountOnly
    );

static
VOID
ShowUsage();

static
VOID
PrintGroupInfo_0(
    PLSA_GROUP_INFO_0 pGroupInfo
    );

static
VOID
PrintGroupInfo_1(
    PLSA_GROUP_INFO_1 pGroupInfo,
    BOOLEAN bCountOnly
    );

static
DWORD
MapErrorCode(
    DWORD dwError
    );

int
find_group_by_id_main(
    int argc,
    char* argv[]
    )
{
    DWORD dwError = 0;
    DWORD gid = 0;
    DWORD dwInfoLevel = 0;
    HANDLE hLsaConnection = (HANDLE)NULL;
    PVOID pGroupInfo = NULL;
    size_t dwErrorBufferSize = 0;
    BOOLEAN bPrintOrigError = TRUE;
    BOOLEAN bCountOnly = FALSE;
    LSA_FIND_FLAGS FindFlags = 0;

    dwError = ParseArgs(argc, argv, &gid, &FindFlags, &dwInfoLevel, &bCountOnly);
    BAIL_ON_LSA_ERROR(dwError);

    dwError = LsaOpenServer(&hLsaConnection);
    BAIL_ON_LSA_ERROR(dwError);

    dwError = LsaFindGroupById(
                    hLsaConnection,
                    gid,
                    FindFlags,
                    dwInfoLevel,
                    &pGroupInfo);
    BAIL_ON_LSA_ERROR(dwError);

    switch(dwInfoLevel)
    {
        case 0:
            PrintGroupInfo_0((PLSA_GROUP_INFO_0)pGroupInfo);
            break;
        case 1:
            PrintGroupInfo_1((PLSA_GROUP_INFO_1)pGroupInfo, bCountOnly);
            break;
        default:

            fprintf(stderr, "Error: Invalid group info level [%u]\n", dwInfoLevel);
            break;
    }

cleanup:

    if (pGroupInfo) {
       LsaFreeGroupInfo(dwInfoLevel, pGroupInfo);
    }

    if (hLsaConnection != (HANDLE)NULL) {
        LsaCloseServer(hLsaConnection);
    }

    return (dwError);

error:

    dwError = MapErrorCode(dwError);

    dwErrorBufferSize = LwGetErrorString(dwError, NULL, 0);

    if (dwErrorBufferSize > 0)
    {
        DWORD dwError2 = 0;
        PSTR   pszErrorBuffer = NULL;

        dwError2 = LwAllocateMemory(
                    dwErrorBufferSize,
                    (PVOID*)&pszErrorBuffer);

        if (!dwError2)
        {
            DWORD dwLen = LwGetErrorString(dwError, pszErrorBuffer, dwErrorBufferSize);

            if ((dwLen == dwErrorBufferSize) && !LW_IS_NULL_OR_EMPTY_STR(pszErrorBuffer))
            {
                fprintf(stderr,
                        "Failed to locate group.  Error code %u (%s).\n%s\n",
                        dwError,
                        LW_PRINTF_STRING(LwWin32ExtErrorToName(dwError)),
                        pszErrorBuffer);
                bPrintOrigError = FALSE;
            }
        }

        LW_SAFE_FREE_STRING(pszErrorBuffer);
    }

    if (bPrintOrigError)
    {
        fprintf(stderr,
                "Failed to locate group.  Error code %u (%s).\n",
                dwError,
                LW_PRINTF_STRING(LwWin32ExtErrorToName(dwError)));
    }

    goto cleanup;
}

DWORD
ParseArgs(
    int    argc,
    char*  argv[],
    PDWORD pdwGroupId,
    PLSA_FIND_FLAGS pFindFlags,
    PDWORD pdwInfoLevel,
    PBOOLEAN pbCountOnly
    )
{
    DWORD dwError = 0;
    int iArg = 1;
    PSTR pszArg = NULL;
    PCSTR pszGid = NULL;
    DWORD gid = 0;
    LSA_FIND_FLAGS FindFlags = 0;
    DWORD dwInfoLevel = 0;
    BOOLEAN bCountOnly = FALSE;
    int nRead = 0;

    for (iArg = 1; iArg < argc; iArg++)
    {
        pszArg = argv[iArg];
        if (!strcmp(pszArg, "--help") ||
            !strcmp(pszArg, "-h"))
        {
            ShowUsage();
            exit(0);
        }
        else if (!strcmp(pszArg, "--count"))
        {
            bCountOnly = TRUE;
        }
        else if (!strcmp(pszArg, "--level"))
        {
            PCSTR pszValue;
            if (iArg + 1 >= argc)
            {
                fprintf(stderr, "Missing argument for %s option.\n", pszArg);
                ShowUsage();
                exit(1);
            }
            pszValue = argv[++iArg];
            if (!IsUnsignedInteger(pszValue))
            {
                fprintf(stderr, "Please enter an info level which is an unsigned integer.\n");
                ShowUsage();
                exit(1);
            }
            dwInfoLevel = atoi(pszValue);
        }
        else if (!strcmp(pszArg, "--flags"))
        {
            PCSTR pszValue;
            if (iArg + 1 >= argc)
            {
                fprintf(stderr, "Missing argument for %s option.\n", pszArg);
                ShowUsage();
                exit(1);
            }
            pszValue = argv[++iArg];
            if (!IsUnsignedInteger(pszValue))
            {
                fprintf(stderr, "Please enter a flags value which is an unsigned integer.\n");
                ShowUsage();
                exit(1);
            }
            FindFlags = atoi(pszValue);
        }
        else if (pszArg[0] == '-')
        {
            fprintf(stderr, "Invalid option '%s'.\n", pszArg);
            ShowUsage();
            exit(1);
        }
        else
        {
            break;
        }
    }

    if ((argc - iArg) < 1)
    {
        fprintf(stderr, "Missing required group id argument.\n");
        ShowUsage();
        exit(1);
    }

    // Grab gid information
    pszGid = argv[iArg++];

    if ((argc - iArg) > 0)
    {
        fprintf(stderr, "Too many arguments.\n");
        ShowUsage();
        exit(1);
    }

    if (LW_IS_NULL_OR_EMPTY_STR(pszGid) || !IsUnsignedInteger(pszGid))
    {
        fprintf(stderr, "Please specify a valid gid to query for.\n");
        ShowUsage();
        exit(1);
    }

    nRead = sscanf(pszGid, "%u", (unsigned int*)&gid);
    if ((EOF == nRead) || (nRead == 0))
    {
       fprintf(stderr, "Please specify a valid gid to query for.\n");
       ShowUsage();
       exit(1);
    }

    *pdwGroupId = gid;
    *pFindFlags = FindFlags;
    *pdwInfoLevel = dwInfoLevel;
    *pbCountOnly = bCountOnly;

    return dwError;
}

void
ShowUsage()
{
    PCSTR pszProgramName = "find-group-by-id";
    printf("Usage: %s [OPTIONS] <GROUP_GID>\n"
           "\n"
           "    Lookup a group by id.\n"
           "\n"
           "  Options:\n"
           "\n"
           "    --level LEVEL   - Output level can be 0 or 1.\n"
           "                      0 does not include membership info.\n"
           "                      1 include group membership info.\n"
           "\n"
           "    --count         - If used with level 1, shows membership count only\n"
           "                      instead of listing group members.\n"
           "\n"
           "    --flags FLAGS   - Find flags can be 0 or 1.\n"
           "\n"
           "  Examples:\n"
           "\n"
           "    %s 239625286\n"
           "    %s --level 1 239625286\n"
           "\n",
           pszProgramName,
           pszProgramName,
           pszProgramName);
}

VOID
PrintGroupInfo_0(
    PLSA_GROUP_INFO_0 pGroupInfo
    )
{
    printf("Group info (Level 0):\n"
           "====================\n"
           "Name: %s\n"
           "Gid:  %u\n"
           "SID:  %s\n",
           LSA_SAFE_LOG_STRING(pGroupInfo->pszName),
           (unsigned int)pGroupInfo->gid,
           LSA_SAFE_LOG_STRING(pGroupInfo->pszSid));
}

VOID
PrintGroupInfo_1(
    PLSA_GROUP_INFO_1 pGroupInfo,
    BOOLEAN bCountOnly
    )
{
    PSTR* ppszMembers = NULL;
    DWORD iMember = 0;

    printf("Group info (Level 1):\n"
           "====================\n"
           "Name: %s\n"
           "Gid:  %u\n"
           "SID:  %s\n"
           "DN:   %s\n",
           LSA_SAFE_LOG_STRING(pGroupInfo->pszName),
           (unsigned int)pGroupInfo->gid,
           LSA_SAFE_LOG_STRING(pGroupInfo->pszSid),
           LSA_SAFE_LOG_STRING(pGroupInfo->pszDN));

    if (!bCountOnly)
    {
        printf("Members:\n");
    }

    ppszMembers = pGroupInfo->ppszMembers;
    if (ppszMembers)
    {
        while (!LW_IS_NULL_OR_EMPTY_STR(ppszMembers[iMember]))
        {
            if (!bCountOnly)
            {
                printf("%s\n", ppszMembers[iMember]);
            }
            iMember++;
        }
    }

    printf("Members Count: %u\n", iMember);
}

DWORD
MapErrorCode(
    DWORD dwError
    )
{
    DWORD dwError2 = dwError;

    switch (dwError)
    {
        case ECONNREFUSED:
        case ENETUNREACH:
        case ETIMEDOUT:

            dwError2 = LW_ERROR_LSA_SERVER_UNREACHABLE;

            break;

        default:

            break;
    }

    return dwError2;
}
