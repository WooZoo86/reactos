/*
 * Implementation of the Microsoft Installer (msi.dll)
 *
 * Copyright 2005 Aric Stewart for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * Actions focused on in this module
 *
 * FindRelatedProducts
 * MigrateFeatureStates (TODO)
 * RemoveExistingProducts (TODO)
 */

#include "msipriv.h"

WINE_DEFAULT_DEBUG_CHANNEL(msi);

static BOOL check_language(DWORD lang1, LPCWSTR lang2, DWORD attributes)
{
    DWORD langdword;

    if (!lang2 || lang2[0]==0)
        return TRUE;

    langdword = atoiW(lang2);

    if (attributes & msidbUpgradeAttributesLanguagesExclusive)
        return (lang1 != langdword);
    else
        return (lang1 == langdword);
}

static void append_productcode(MSIPACKAGE* package, LPCWSTR action_property,
                               LPCWSTR productid)
{
    LPWSTR prop;
    LPWSTR newprop;
    DWORD len;
    UINT r;

    prop = msi_dup_property(package->db, action_property );
    if (prop)
        len = strlenW(prop);
    else
        len = 0;

    /*separator*/
    len ++;

    len += strlenW(productid);

    /*null*/
    len++;

    newprop = msi_alloc( len*sizeof(WCHAR) );

    if (prop)
    {
        strcpyW(newprop,prop);
        strcatW(newprop,szSemiColon);
    }
    else
        newprop[0] = 0;
    strcatW(newprop,productid);

    r = msi_set_property( package->db, action_property, newprop, -1 );
    if (r == ERROR_SUCCESS && !strcmpW( action_property, szSourceDir ))
        msi_reset_folders( package, TRUE );

    TRACE("Found Related Product... %s now %s\n",
          debugstr_w(action_property), debugstr_w(newprop));

    msi_free( prop );
    msi_free( newprop );
}

static UINT ITERATE_FindRelatedProducts(MSIRECORD *rec, LPVOID param)
{
    MSIPACKAGE *package = param;
    WCHAR product[SQUASHED_GUID_SIZE];
    DWORD index = 0, attributes = 0, sz = sizeof(product)/sizeof(product[0]);
    LPCWSTR upgrade_code;
    HKEY hkey = 0;
    UINT rc = ERROR_SUCCESS;
    MSIRECORD *uirow;

    upgrade_code = MSI_RecordGetString(rec,1);

    rc = MSIREG_OpenUpgradeCodesKey(upgrade_code, &hkey, FALSE);
    if (rc != ERROR_SUCCESS)
        return ERROR_SUCCESS;

    uirow = MSI_CreateRecord(1);
    attributes = MSI_RecordGetInteger(rec,5);

    while (rc == ERROR_SUCCESS)
    {
        rc = RegEnumValueW(hkey, index, product, &sz, NULL, NULL, NULL, NULL);
        if (rc == ERROR_SUCCESS)
        {
            WCHAR productid[GUID_SIZE];
            LPCWSTR ver, language, action_property;
            DWORD check = 0, comp_ver, sz = 0x100;
            HKEY hukey;
            INT r;

            TRACE("Looking at index %u product %s\n", index, debugstr_w(product));

            unsquash_guid(product, productid);
            if (MSIREG_OpenProductKey(productid, NULL, MSIINSTALLCONTEXT_USERMANAGED, &hukey, FALSE) &&
                MSIREG_OpenProductKey(productid, NULL, MSIINSTALLCONTEXT_USERUNMANAGED, &hukey, FALSE) &&
                MSIREG_OpenProductKey(productid, NULL, MSIINSTALLCONTEXT_MACHINE, &hukey, FALSE))
            {
                TRACE("product key not found\n");
                rc = ERROR_SUCCESS;
                index ++;
                continue;
            }

            sz = sizeof(DWORD);
            RegQueryValueExW(hukey, INSTALLPROPERTY_VERSIONW, NULL, NULL, (LPBYTE)&check, &sz);

            /* check version minimum */
            ver = MSI_RecordGetString(rec,2);
            if (ver)
            {
                comp_ver = msi_version_str_to_dword(ver);
                r = check - comp_ver;
                if (r < 0 || (r == 0 && !(attributes & msidbUpgradeAttributesVersionMinInclusive)))
                {
                    TRACE("version below minimum\n");
                    RegCloseKey(hukey);
                    index ++;
                    continue;
                }
            }

            /* check version maximum */
            ver = MSI_RecordGetString(rec,3);
            if (ver)
            {
                comp_ver = msi_version_str_to_dword(ver);
                r = check - comp_ver;
                if (r > 0 || (r == 0 && !(attributes & msidbUpgradeAttributesVersionMaxInclusive)))
                {
                    RegCloseKey(hukey);
                    index ++;
                    continue;
                }
                TRACE("version above maximum\n");
            }

            /* check language */
            sz = sizeof(DWORD);
            RegQueryValueExW(hukey, INSTALLPROPERTY_LANGUAGEW, NULL, NULL, (LPBYTE)&check, &sz);
            RegCloseKey(hukey);
            language = MSI_RecordGetString(rec,4);
            if (!check_language(check, language, attributes))
            {
                index ++;
                TRACE("language doesn't match\n");
                continue;
            }
            TRACE("found related product\n");

            action_property = MSI_RecordGetString(rec, 7);
            append_productcode(package, action_property, productid);
            MSI_RecordSetStringW(uirow, 1, productid);
            MSI_ProcessMessage(package, INSTALLMESSAGE_ACTIONDATA, uirow);
        }
        index ++;
    }
    RegCloseKey(hkey);
    msiobj_release( &uirow->hdr);
    
    return ERROR_SUCCESS;
}

UINT ACTION_FindRelatedProducts(MSIPACKAGE *package)
{
    static const WCHAR query[] = {
        'S','E','L','E','C','T',' ','*',' ','F','R','O','M',' ',
        '`','U','p','g','r','a','d','e','`',0};
    MSIQUERY *view;
    UINT rc;

    if (msi_get_property_int(package->db, szInstalled, 0))
    {
        TRACE("Skipping FindRelatedProducts action: product already installed\n");
        return ERROR_SUCCESS;
    }
    if (msi_action_is_unique(package, szFindRelatedProducts))
    {
        TRACE("Skipping FindRelatedProducts action: already done in UI sequence\n");
        return ERROR_SUCCESS;
    }
    else
        msi_register_unique_action(package, szFindRelatedProducts);

    rc = MSI_DatabaseOpenViewW(package->db, query, &view);
    if (rc != ERROR_SUCCESS)
        return ERROR_SUCCESS;
    
    rc = MSI_IterateRecords(view, NULL, ITERATE_FindRelatedProducts, package);
    msiobj_release(&view->hdr);
    return rc;
}
