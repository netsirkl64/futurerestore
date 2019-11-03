//
//  futurerestore.cpp
//  futurerestore
//
//  Created by tihmstar on 14.09.16.
//  Copyright © 2016 tihmstar. All rights reserved.
//

#if defined _WIN32 || defined __CYGWIN__
#ifndef WIN32
//make sure WIN32 is defined if compiling for windows
#define WIN32
#endif
#endif

#include <libgeneral/macros.h>

#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <zlib.h>
#include "futurerestore.hpp"

#ifdef HAVE_LIBIPATCHER
#include <libipatcher/libipatcher.hpp>
#endif

#include <img4tool/img4tool.hpp>

extern "C"{
#include "common.h"
#include "normal.h"
#include "recovery.h"
#include "dfu.h"
#include "ipsw.h"
#include "locking.h"
#include "restore.h"
#include "tsschecker.h"
#include "all_tsschecker.h"
#include <libirecovery.h>
}


//(re)define __mkdir
#ifdef __mkdir
#undef __mkdir
#endif
#ifdef WIN32
#include <windows.h>
#define __mkdir(path, mode) mkdir(path)
#else
#include <sys/stat.h>
#define __mkdir(path, mode) mkdir(path, mode)
#endif

#define USEC_PER_SEC 1000000

#define TMP_PATH "/tmp"
#define FUTURERESTORE_TMP_PATH TMP_PATH"/futurerestore"

#define BASEBAND_TMP_PATH FUTURERESTORE_TMP_PATH"/baseband.bbfw"
#define BASEBAND_MANIFEST_TMP_PATH FUTURERESTORE_TMP_PATH"/basebandManifest.plist"
#define SEP_TMP_PATH FUTURERESTORE_TMP_PATH"/sep.im4p"
#define SEP_MANIFEST_TMP_PATH FUTURERESTORE_TMP_PATH"/sepManifest.plist"

#ifdef __APPLE__
#   include <CommonCrypto/CommonDigest.h>
#   define SHA1(d, n, md) CC_SHA1(d, n, md)
#   define SHA384(d, n, md) CC_SHA384(d, n, md)
#else
#   include <openssl/sha.h>
#endif // __APPLE__

#ifndef HAVE_LIBIPATCHER
#define _enterPwnRecoveryRequested false
#endif

using namespace tihmstar;

#pragma mark helpers

extern "C"{
    void irecv_event_cb(const irecv_device_event_t* event, void *userdata);
    void idevice_event_cb(const idevice_event_t *event, void *userdata);
};

#pragma mark futurerestore

futurerestore::futurerestore(bool isUpdateInstall, bool isPwnDfu) : _isUpdateInstall(isUpdateInstall), _isPwnDfu(isPwnDfu){
    _client = idevicerestore_client_new();
    if (_client == NULL) throw std::string("could not create idevicerestore client\n");
    
    struct stat st{0};
    if (stat(FUTURERESTORE_TMP_PATH, &st) == -1) __mkdir(FUTURERESTORE_TMP_PATH, 0755);
    
    //tsschecker nocache
    nocache = 1;
    _foundnonce = -1;
}

bool futurerestore::init(){
    if (_didInit) return _didInit;
    _didInit = (check_mode(_client) != MODE_UNKNOWN);
    if (!(_client->image4supported = is_image4_supported(_client))){
        info("[INFO] 32bit device detected\n");
    }else{
        info("[INFO] 64bit device detected\n");
    }
    return _didInit;
}

uint64_t futurerestore::getDeviceEcid(){
    retassure(_didInit, "did not init\n");
    uint64_t ecid;
    
    get_ecid(_client, &ecid);
    
    return ecid;
}

int futurerestore::getDeviceMode(bool reRequest){
    retassure(_didInit, "did not init\n");
    if (!reRequest && _client->mode && _client->mode->index != MODE_UNKNOWN) {
        return _client->mode->index;
    }else{
        dfu_client_free(_client);
        recovery_client_free(_client);
        return check_mode(_client);
    }
}

void futurerestore::putDeviceIntoRecovery(){
    retassure(_didInit, "did not init\n");

#ifdef HAVE_LIBIPATCHER
    _enterPwnRecoveryRequested = _isPwnDfu;
#endif
    
    getDeviceMode(false);
    info("Found device in %s mode\n", _client->mode->string);
    if (_client->mode->index == MODE_NORMAL){
#ifdef HAVE_LIBIPATCHER
        retassure(!_isPwnDfu, "isPwnDfu enabled, but device was found in normal mode\n");
#endif
        info("Entering recovery mode...\n");
        retassure(!normal_enter_recovery(_client),"Unable to place device into recovery mode from %s mode\n", _client->mode->string);
    }else if (_client->mode->index == MODE_RECOVERY){
        info("Device already in Recovery mode\n");
    }else if (_client->mode->index == MODE_DFU && _isPwnDfu &&
#ifdef HAVE_LIBIPATCHER
              true
#else
              false
#endif
              ){
        info("requesting to get into pwnRecovery later\n");
    }else if (!_client->image4supported){
        info("32bit device in DFU mode found, assuming user wants to use iOS9 re-restore bug. Not failing here\n");
    }else{
        reterror("unsupported devicemode, please put device in recovery mode or normal mode\n");
    }
    
    //only needs to be freed manually when function did't throw exception
    safeFree(_client->udid);
    
    //these get also freed by destructor
    dfu_client_free(_client);
    recovery_client_free(_client);
}

void futurerestore::setAutoboot(bool val){
    retassure(_didInit, "did not init\n");

    retassure(getDeviceMode(false) == MODE_RECOVERY, "can't set autoboot, when device isn't in recovery mode\n");
    
    retassure(!_client->recovery && recovery_client_new(_client),"Could not connect to device in recovery mode.\n");
    retassure(!recovery_set_autoboot(_client, val),"Setting auto-boot failed?!\n");
}

plist_t futurerestore::nonceMatchesApTickets(){
    retassure(_didInit, "did not init\n");

    if (getDeviceMode(true) != MODE_RECOVERY){
        if (getDeviceMode(false) != MODE_DFU || *_client->version != '9')
            reterror("Device not in recovery mode, can't check apnonce\n");
        else
            _rerestoreiOS9 = (info("Detected iOS 9 re-restore, proceeding in DFU mode\n"),true);
    }
    
    
    unsigned char* realnonce;
    int realNonceSize = 0;
    if (_rerestoreiOS9) {
        info("Skipping APNonce check\n");
    }else{
        recovery_get_ap_nonce(_client, &realnonce, &realNonceSize);
        
        info("Got APNonce from device: ");
        int i = 0;
        for (i = 0; i < realNonceSize; i++) {
            info("%02x ", ((unsigned char *)realnonce)[i]);
        }
        info("\n");
    }
    
    vector<const char*>nonces;
    
    if (_client->image4supported){
        for (int i=0; i< _im4ms.size(); i++){
            auto nonce = img4tool::getValFromIM4M({_im4ms[i].first,_im4ms[i].second}, 'BNCH');
            if (nonce.payloadSize() == realNonceSize && memcmp(realnonce, nonce.payload(), realNonceSize) == 0) return _aptickets[i];
        }
    }else{
        for (int i=0; i< _im4ms.size(); i++){
            size_t ticketNonceSize = 0;
            const char *nonce = NULL;
            try {
                //nonce might not exist, which we use in re-restoring iOS9
                auto n = getNonceFromSCAB(_im4ms[i].first, _im4ms[i].second);
                ticketNonceSize = n.second;
                nonce = n.first;
            } catch (...) {
                //
            }
            if (memcmp(realnonce, nonce, ticketNonceSize) == 0 &&
                 (  (ticketNonceSize == realNonceSize && realNonceSize+ticketNonceSize > 0) ||
                        (!ticketNonceSize && *_client->version == '9' &&
                            (getDeviceMode(false) == MODE_DFU ||
                                ( getDeviceMode(false) == MODE_RECOVERY && !strncmp(getiBootBuild(), "iBoot-2817", strlen("iBoot-2817")) )
                            )
                         )
                 )
               )
                //either nonce needs to match or using re-restore bug in iOS 9
                return _aptickets[i];
        }
    }
    
    
    return NULL;
}

std::pair<const char *,size_t> futurerestore::nonceMatchesIM4Ms(){
    retassure(_didInit, "did not init\n");

    retassure(getDeviceMode(true) == MODE_RECOVERY, "Device not in recovery mode, can't check apnonce\n");
    
    unsigned char* realnonce;
    int realNonceSize = 0;
    recovery_get_ap_nonce(_client, &realnonce, &realNonceSize);
    
    vector<const char*>nonces;
    
    if (_client->image4supported) {
        for (int i=0; i< _im4ms.size(); i++){
            auto nonce = img4tool::getValFromIM4M({_im4ms[i].first,_im4ms[i].second}, 'BNCH');
            if (nonce.payloadSize() == realNonceSize && memcmp(realnonce, nonce.payload(), realNonceSize) == 0) return _im4ms[i];
        }
    }else{
        for (int i=0; i< _im4ms.size(); i++){
            size_t ticketNonceSize = 0;
            const char *nonce = NULL;
            try {
                //nonce might not exist, which we use in re-restoring iOS9
                auto n = getNonceFromSCAB(_im4ms[i].first, _im4ms[i].second);
                ticketNonceSize = n.second;
                nonce = n.first;
            } catch (...) {
                //
            }
            if (memcmp(realnonce, nonce, ticketNonceSize) == 0) return _im4ms[i];
        }
    }
    
    return {NULL,0};
}



void futurerestore::waitForNonce(vector<const char *>nonces, size_t nonceSize){
    retassure(_didInit, "did not init\n");
    setAutoboot(false);
    
    unsigned char* realnonce;
    int realNonceSize = 0;
    
    for (auto nonce : nonces){
        info("waiting for nonce: ");
        int i = 0;
        for (i = 0; i < nonceSize; i++) {
            info("%02x ", ((unsigned char *)nonce)[i]);
        }
        info("\n");
    }
    
    do {
        if (realNonceSize){
            recovery_send_reset(_client);
            recovery_client_free(_client);
            usleep(1*USEC_PER_SEC);
        }
        while (getDeviceMode(true) != MODE_RECOVERY) usleep(USEC_PER_SEC*0.5);
        retassure(!recovery_client_new(_client), "Could not connect to device in recovery mode.\n");
        
        recovery_get_ap_nonce(_client, &realnonce, &realNonceSize);
        info("Got ApNonce from device: ");
        int i = 0;
        for (i = 0; i < realNonceSize; i++) {
            info("%02x ", realnonce[i]);
        }
        info("\n");
        for (int i=0; i<nonces.size(); i++){
            if (memcmp(realnonce, (unsigned const char*)nonces[i], realNonceSize) == 0) _foundnonce = i;
        }
    } while (_foundnonce == -1);
    info("Device has requested ApNonce now\n");
    
    setAutoboot(true);
}
void futurerestore::waitForNonce(){
    retassure(_im4ms.size(), "No IM4M loaded\n");
    
    size_t nonceSize = 0;
    vector<const char*>nonces;
    
    retassure(_client->image4supported, "Error: waitForNonce is not supported on 32bit devices\n");
    
    for (auto im4m : _im4ms){
        auto nonce = img4tool::getValFromIM4M({im4m.first,im4m.second}, 'BNCH');
        if (!nonceSize) {
            nonceSize = nonce.payloadSize();
        }
        retassure(nonceSize == nonce.payloadSize(), "Nonces have different lengths!");
        nonces.push_back((const char*)nonce.payload());
    }
    
    waitForNonce(nonces,nonceSize);
}

void futurerestore::loadAPTickets(const vector<const char *> &apticketPaths){
    for (auto apticketPath : apticketPaths){
        plist_t apticket = NULL;
        char *im4m = NULL;
        struct stat fst;
        
        retassure(!stat(apticketPath, &fst), "failed to load apticket at %s\n",apticketPath);
        
        gzFile zf = gzopen(apticketPath, "rb");
        if (zf) {
            int blen = 0;
            int readsize = 16384; //0x4000
            int bufsize = readsize;
            char* bin = (char*)malloc(bufsize);
            char* p = bin;
            do {
                int bytes_read = gzread(zf, p, readsize);
                retassure(bytes_read>0, "Error reading gz compressed data\n");
                blen += bytes_read;
                if (bytes_read < readsize) {
                    if (gzeof(zf)) {
                        bufsize += bytes_read;
                        break;
                    }
                }
                bufsize += readsize;
                bin = (char*)realloc(bin, bufsize);
                p = bin + blen;
            } while (!gzeof(zf));
            gzclose(zf);
            if (blen > 0) {
                if (memcmp(bin, "bplist00", 8) == 0)
                    plist_from_bin(bin, blen, &apticket);
                else
                    plist_from_xml(bin, blen, &apticket);
            }
            free(bin);
        }
        
        if (_isUpdateInstall) {
            if(plist_t update =  plist_dict_get_item(apticket, "updateInstall")){
                plist_t cpy = plist_copy(update);
                plist_free(apticket);
                apticket = cpy;
            }
        }
        
        plist_t ticket = plist_dict_get_item(apticket, (_client->image4supported) ? "ApImg4Ticket" : "APTicket");
        uint64_t im4msize=0;
        plist_get_data_val(ticket, &im4m, &im4msize);
        
        retassure(im4msize, "Error: failed to load shsh file %s\n",apticketPath);
        
        _im4ms.push_back({im4m,im4msize});
        _aptickets.push_back(apticket);
        printf("reading ticket %s done\n",apticketPath);
    }
}

uint64_t futurerestore::getBasebandGoldCertIDFromDevice(){
    if (!_client->preflight_info){
        if (normal_get_preflight_info(_client, &_client->preflight_info) == -1){
            printf("[WARNING] failed to read BasebandGoldCertID from device! Is it already in recovery?\n");
            return 0;
        }
    }
    plist_t node;
    node = plist_dict_get_item(_client->preflight_info, "CertID");
    if (!node || plist_get_node_type(node) != PLIST_UINT) {
        error("Unable to find required BbGoldCertId in parameters\n");
        return 0;
    }
    uint64_t val = 0;
    plist_get_uint_val(node, &val);
    return val;
}

char *futurerestore::getiBootBuild(){
    if (!_ibootBuild){
        if (_client->recovery == NULL) {
            retassure(!recovery_client_new(_client), "Error: can't create new recovery client");
        }
        irecv_getenv(_client->recovery->client, "build-version", &_ibootBuild);
        retassure(_ibootBuild, "Error: can't get build-version");
    }
    return _ibootBuild;
}


pair<ptr_smart<char*>, size_t> getIPSWComponent(struct idevicerestore_client_t* client, plist_t build_identity, string component){
    ptr_smart<char *> path;
    unsigned char* component_data = NULL;
    unsigned int component_size = 0;

    if (!(char*)path) {
        retassure(!build_identity_get_component_path(build_identity, component.c_str(), &path),"ERROR: Unable to get path for component '%s'\n", component.c_str());
    }
    
    retassure(!extract_component(client->ipsw, (char*)path, &component_data, &component_size),"ERROR: Unable to extract component: %s\n", component.c_str());
    
    return {(char*)component_data,component_size};
}


void futurerestore::enterPwnRecovery(plist_t build_identity, string bootargs){
#ifndef HAVE_LIBIPATCHER
    reterror("compiled without libipatcher");
#else
    if (_client->image4supported) {
        retassure(libipatcher::has64bitSupport(), "libipatcher was compiled without 64bit support");
    }
    
    int mode = 0;
    libipatcher::fw_key iBSSKeys;
    libipatcher::fw_key iBECKeys;
    
    retassure(!dfu_client_new(_client),"Unable to connect to DFU device\n");

    irecv_get_mode(_client->dfu->client, &mode);
    
    
    try {
        iBSSKeys = libipatcher::getFirmwareKey(_client->device->product_type, _client->build, "iBSS");
        iBECKeys = libipatcher::getFirmwareKey(_client->device->product_type, _client->build, "iBEC");
    } catch (tihmstar::exception e) {
        reterror("getting keys failed with error: %d (%s). Are keys publicly available?",e.code(),e.what());
    }
    
    
    auto iBSS = getIPSWComponent(_client, build_identity, "iBSS");
    iBSS = move(libipatcher::patchiBSS((char*)iBSS.first, iBSS.second, iBSSKeys));
    
    
    auto iBEC = getIPSWComponent(_client, build_identity, "iBEC");
    iBEC = move(libipatcher::patchiBEC((char*)iBEC.first, iBEC.second, iBECKeys, bootargs));
        
    if (_client->image4supported) {
        //if this is 64bit, we need to back IM4P to IMG4
        //also due to the nature of iBoot64Patchers sigpatches we need to stich a valid signed im4m to it (but nonce is ignored)
        iBSS = move(libipatcher::packIM4PToIMG4(iBSS.first, iBSS.second, _im4ms[0].first, _im4ms[0].second));
        iBEC = move(libipatcher::packIM4PToIMG4(iBEC.first, iBEC.second, _im4ms[0].first, _im4ms[0].second));
    }
    
    bool modeIsRecovery = false;
    if (mode != IRECV_K_DFU_MODE) {
        info("NOTE: device is not in DFU mode, assuming pwn recovery mode.\n");
        for (int i=IRECV_K_RECOVERY_MODE_1; i<=IRECV_K_RECOVERY_MODE_4; i++) {
            if (mode == i)
                modeIsRecovery = true;
        }
        retassure(modeIsRecovery, "device not in recovery mode\n");
    }else{
        info("Sending %s (%lu bytes)...\n", "iBSS", iBSS.second);
        // FIXME: Did I do this right????
        irecv_error_t err = irecv_send_buffer(_client->dfu->client, (unsigned char*)(char*)iBSS.first, (unsigned long)iBSS.second, 1);
        retassure(err == IRECV_E_SUCCESS,"ERROR: Unable to send %s component: %s\n", "iBSS", irecv_strerror(err));
    }
    
    if (_client->build_major > 8) {
        /* reconnect */
        dfu_client_free(_client);
        sleep(3);
        dfu_client_new(_client);
        
       retassure(!irecv_usb_set_configuration(_client->dfu->client, 1),"ERROR: set configuration failed\n");
        
        /* send iBEC */
        info("Sending %s (%lu bytes)...\n", "iBEC", iBEC.second);
        // FIXME: Did I do this right????
        irecv_error_t err = irecv_send_buffer(_client->dfu->client, (unsigned char*)(char*)iBEC.first, (unsigned long)iBEC.second, 1);
        retassure(err == IRECV_E_SUCCESS,"ERROR: Unable to send %s component: %s\n", "iBSS", irecv_strerror(err));
        if (modeIsRecovery)
            irecv_send_command(_client->dfu->client, "go");
    }
    
    dfu_client_free(_client);
    
    sleep(7);
    
    // Reconnect to device, but this time make sure we're not still in DFU mode
    if (recovery_client_new(_client) < 0) {
        if (_client->recovery->client) {
            irecv_close(_client->recovery->client);
            _client->recovery->client = NULL;
        }
        reterror("ERROR: Unable to connect to recovery device\n");
    }
    
#warning THIS FAILS on iPhone5s 10.3.3 for some reason :/
//    irecv_get_mode(_client->recovery->client, &mode);
//    if (mode == IRECV_K_DFU_MODE) {
//        if (_client->recovery->client) {
//            irecv_close(_client->recovery->client);
//            _client->recovery->client = NULL;
//        }
//        reterror("ERROR: Unable to connect to recovery device\n");
//    }
#endif
}

void get_custom_component(struct idevicerestore_client_t* client, plist_t build_identity, const char* component, unsigned char** data, unsigned int *size){
#ifndef HAVE_LIBIPATCHER
    reterror("compiled without libipatcher");
#else
    try {
        auto comp = getIPSWComponent(client, build_identity, component);
        comp = move(libipatcher::decryptFile3((char*)comp.first, comp.second, libipatcher::getFirmwareKey(client->device->product_type, client->build, component)));
        *data = (unsigned char*)(char*)comp.first;
        *size = comp.second;
        comp.first = NULL; //don't free on destruction
    } catch (tihmstar::exception &e) {
        reterror("ERROR: libipatcher failed with reason %d (%s)\n",e.code(),e.what());
    }
    
#endif
}


void futurerestore::doRestore(const char *ipsw){
    plist_t buildmanifest = NULL;
    int delete_fs = 0;
    char* filesystem = NULL;
    cleanup([&]{
        info("Cleaning up...\n");
        safeFreeCustom(buildmanifest, plist_free);
        if (delete_fs && filesystem) unlink(filesystem);
    });
    struct idevicerestore_client_t* client = _client;
    plist_t build_identity = NULL;
    plist_t sep_build_identity = NULL;

    client->ipsw = strdup(ipsw);
    if (!_isUpdateInstall) client->flags |= FLAG_ERASE;
    
    irecv_device_event_subscribe(&client->irecv_e_ctx, irecv_event_cb, client);
    idevice_event_subscribe(idevice_event_cb, client);
    client->idevice_e_ctx = (void*)idevice_event_cb;

    mutex_lock(&client->device_event_mutex);
    cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 10000);
    
    retassure(client->mode != &idevicerestore_modes[MODE_UNKNOWN],  "Unable to discover device mode. Please make sure a device is attached.\n");
    if (client->mode != &idevicerestore_modes[MODE_RECOVERY]) {
        retassure(client->mode == &idevicerestore_modes[MODE_DFU], "Device in unexpected mode detected!");
        retassure(_enterPwnRecoveryRequested, "Device in DFU mode detected, but we were expecting recovery mode!");
    }else{
        retassure(!_enterPwnRecoveryRequested, "--pwn-dfu was specified, but device found in recovery mode!");
    }

    info("Found device in %s mode\n", client->mode->string);
    mutex_unlock(&client->device_event_mutex);

    info("Identified device as %s, %s\n", getDeviceBoardNoCopy(), getDeviceModelNoCopy());

    // verify if ipsw file exists
    retassure(!access(client->ipsw, F_OK),"ERROR: Firmware file %s does not exist.\n", client->ipsw);


    info("Extracting BuildManifest from IPSW\n");
    {
        int unused;
        retassure(!ipsw_extract_build_manifest(client->ipsw, &buildmanifest, &unused),"ERROR: Unable to extract BuildManifest from %s. Firmware file might be corrupt.\n", client->ipsw);
    }

    /* check if device type is supported by the given build manifest */
    retassure(!build_manifest_check_compatibility(buildmanifest, client->device->product_type),"ERROR: Could not make sure this firmware is suitable for the current device. Refusing to continue.\n");
    /* print iOS information from the manifest */
    build_manifest_get_version_information(buildmanifest, client);

    info("Product Version: %s\n", client->version);
    info("Product Build: %s Major: %d\n", client->build, client->build_major);

    client->image4supported = is_image4_supported(client);
    info("Device supports Image4: %s\n", (client->image4supported) ? "true" : "false");

    if (_enterPwnRecoveryRequested) //we are in pwnDFU, so we don't need to check nonces
        client->tss = _aptickets.at(0);
    else if (!(client->tss = nonceMatchesApTickets()))
        reterror("Devicenonce does not match APTicket nonce\n");

    plist_dict_remove_item(client->tss, "BBTicket");
    plist_dict_remove_item(client->tss, "BasebandFirmware");

    retassure(build_identity = getBuildidentityWithBoardconfig(buildmanifest, client->device->hardware_model, _isUpdateInstall),"ERROR: Unable to find any build identities for IPSW\n");

    if (_client->image4supported) {
        if (!(sep_build_identity = getBuildidentityWithBoardconfig(_sepbuildmanifest, client->device->hardware_model, _isUpdateInstall))){
            retassure(_isPwnDfu, "ERROR: Unable to find any build identities for SEP\n");
            warning("can't find buildidentity for SEP with InstallType=%s. However pwnDfu was requested, so trying fallback to %s",(_isUpdateInstall ? "UPDATE" : "ERASE"),(!_isUpdateInstall ? "UPDATE" : "ERASE"));
            retassure((sep_build_identity = getBuildidentityWithBoardconfig(_sepbuildmanifest, client->device->hardware_model, !_isUpdateInstall)),
                      "ERROR: Unable to find any build identities for SEP\n");
        }
    }


    //this is the buildidentity used for restore
    plist_t manifest = plist_dict_get_item(build_identity, "Manifest");

    printf("checking APTicket to be valid for this restore...\n");
    //if we are in pwnDFU, just use first apticket. no need to check nonces
    auto im4m = (_enterPwnRecoveryRequested || _rerestoreiOS9) ? _im4ms.at(0) : nonceMatchesIM4Ms();

    uint64_t deviceEcid = getDeviceEcid();
    uint64_t im4mEcid = 0;
    if (_client->image4supported) {
        auto ecid = img4tool::getValFromIM4M({im4m.first,im4m.second}, 'ECID');
        im4mEcid = ecid.getIntegerValue();
    }else{
        im4mEcid = getEcidFromSCAB(im4m.first, im4m.second);
    }

    retassure(im4mEcid, "Failed to read ECID from APTicket\n");

    if (im4mEcid != deviceEcid) {
        error("ECID inside APTicket does not match device ECID\n");
        printf("APTicket is valid for %16llu (dec) but device is %16llu (dec)\n",im4mEcid,deviceEcid);
        reterror("APTicket can't be used for restoring this device\n");
    }else
        printf("Verified ECID in APTicket matches device ECID\n");

    if (_client->image4supported) {
        printf("checking APTicket to be valid for this restore...\n");
        uint64_t deviceEcid = getDeviceEcid();

        if (im4mEcid != deviceEcid) {
            error("ECID inside APTicket does not match device ECID\n");
            printf("APTicket is valid for %16llu (dec) but device is %16llu (dec)\n",im4mEcid,deviceEcid);
            reterror("APTicket can't be used for restoring this device\n");
        }else
            printf("Verified ECID in APTicket matches device ECID\n");

        plist_t ticketIdentity = img4tool::getBuildIdentityForIm4m({im4m.first,im4m.second}, buildmanifest);
        //TODO: make this nicer!
        //for now a simple pointercompare should be fine, because both plist_t should point into the same buildidentity inside the buildmanifest
        if (ticketIdentity != build_identity ){
            error("BuildIdentity selected for restore does not match APTicket\n\n");
            printf("BuildIdentity selected for restore:\n");
            img4tool::printGeneralBuildIdentityInformation(build_identity);
            printf("\nBuildIdentiy valid for the APTicket:\n");

            if (ticketIdentity) img4tool::printGeneralBuildIdentityInformation(ticketIdentity),putchar('\n');
            else{
                printf("IM4M is not valid for any restore within the Buildmanifest\n");
                printf("This APTicket can't be used for restoring this firmware\n");
            }
            reterror("APTicket can't be used for this restore\n");
        }else{
            if (!img4tool::isIM4MSignatureValid({im4m.first,im4m.second})){
                printf("IM4M signature is not valid!\n");
                reterror("APTicket can't be used for this restore\n");
            }
            printf("Verified APTicket to be valid for this restore\n");
        }
    }else if (_enterPwnRecoveryRequested){
        info("[WARNING] skipping ramdisk hash check, since device is in pwnDFU according to user\n");

    }else{
        info("[WARNING] full buildidentity check is not implemented, only comparing ramdisk hash.\n");

        auto ticket = getRamdiskHashFromSCAB(im4m.first, im4m.second);
        const char *tickethash = ticket.first;
        size_t tickethashSize = ticket.second;


        uint64_t manifestDigestSize = 0;
        char *manifestDigest = NULL;

        plist_t restoreRamdisk = plist_dict_get_item(manifest, "RestoreRamDisk");
        plist_t digest = plist_dict_get_item(restoreRamdisk, "Digest");

        plist_get_data_val(digest, &manifestDigest, &manifestDigestSize);


        if (tickethashSize == manifestDigestSize && memcmp(tickethash, manifestDigest, tickethashSize) == 0){
            printf("Verified APTicket to be valid for this restore\n");
            free(manifestDigest);
        }else{
            free(manifestDigest);
            printf("APTicket ramdisk hash does not match the ramdisk we are trying to boot. Are you using correct install type (Update/Erase)?\n");
            reterror("APTicket can't be used for this restore\n");
        }
    }


    if (_basebandbuildmanifest){
        if (!(client->basebandBuildIdentity = getBuildidentityWithBoardconfig(_basebandbuildmanifest, client->device->hardware_model, _isUpdateInstall))){
            retassure(client->basebandBuildIdentity = getBuildidentityWithBoardconfig(_basebandbuildmanifest, client->device->hardware_model, !_isUpdateInstall), "ERROR: Unable to find any build identities for Baseband\n");
            info("[WARNING] Unable to find Baseband buildidentities for restore type %s, using fallback %s\n", (_isUpdateInstall) ? "Update" : "Erase",(!_isUpdateInstall) ? "Update" : "Erase");
        }

        client->bbfwtmp = (char*)_basebandPath;

        plist_t bb_manifest = plist_dict_get_item(client->basebandBuildIdentity, "Manifest");
        plist_t bb_baseband = plist_copy(plist_dict_get_item(bb_manifest, "BasebandFirmware"));
        plist_dict_set_item(manifest, "BasebandFirmware", bb_baseband);

        retassure(_client->basebandBuildIdentity, "BasebandBuildIdentity not loaded, refusing to continue");
    }else{
        warning("WARNING: we don't have a basebandbuildmanifest, not flashing baseband!\n");
    }

    if (_client->image4supported) {
        plist_t sep_manifest = plist_dict_get_item(sep_build_identity, "Manifest");
        plist_t sep_sep = plist_copy(plist_dict_get_item(sep_manifest, "SEP"));
        plist_dict_set_item(manifest, "SEP", sep_sep);
        //check SEP
        unsigned char genHash[48]; //SHA384 digest length
        ptr_smart<unsigned char *>sephash = NULL;
        uint64_t sephashlen = 0;
        plist_t digest = plist_dict_get_item(sep_sep, "Digest");

        retassure(digest && plist_get_node_type(digest) == PLIST_DATA, "ERROR: can't find sep digest\n");

        plist_get_data_val(digest, reinterpret_cast<char **>(&sephash), &sephashlen);

        if (sephashlen == 20)
            SHA1((unsigned char*)_client->sepfwdata, (unsigned int)_client->sepfwdatasize, genHash);
        else
            SHA384((unsigned char*)_client->sepfwdata, (unsigned int)_client->sepfwdatasize, genHash);
        retassure(!memcmp(genHash, sephash, sephashlen), "ERROR: SEP does not match sepmanifest\n");
    }

    /* print information about current build identity */
    build_identity_print_information(build_identity);

    //check for enterpwnrecovery, because we could be in DFU mode
    if (_enterPwnRecoveryRequested){
        retassure(getDeviceMode(true) == MODE_DFU, "unexpected device mode\n");
        enterPwnRecovery(build_identity);
    }

    // Get filesystem name from build identity
    char* fsname = NULL;
    retassure(!build_identity_get_component_path(build_identity, "OS", &fsname), "ERROR: Unable get path for filesystem component\n");

    // check if we already have an extracted filesystem
    struct stat st;
    memset(&st, '\0', sizeof(struct stat));
    char tmpf[1024];
    if (client->cache_dir) {
        if (stat(client->cache_dir, &st) < 0) {
            mkdir_with_parents(client->cache_dir, 0755);
        }
        strcpy(tmpf, client->cache_dir);
        strcat(tmpf, "/");
        char *ipswtmp = strdup(client->ipsw);
        strcat(tmpf, basename(ipswtmp));
        free(ipswtmp);
    } else {
        strcpy(tmpf, client->ipsw);
    }
    char* p = strrchr(tmpf, '.');
    if (p) {
        *p = '\0';
    }

    if (stat(tmpf, &st) < 0) {
        __mkdir(tmpf, 0755);
    }
    strcat(tmpf, "/");
    strcat(tmpf, fsname);

    memset(&st, '\0', sizeof(struct stat));
    if (stat(tmpf, &st) == 0) {
        off_t fssize = 0;
        ipsw_get_file_size(client->ipsw, fsname, (uint64_t*)&fssize);
        if ((fssize > 0) && (st.st_size == fssize)) {
            info("Using cached filesystem from '%s'\n", tmpf);
            filesystem = strdup(tmpf);
        }
    }

    if (!filesystem) {
        char extfn[1024];
        strcpy(extfn, tmpf);
        strcat(extfn, ".extract");
        char lockfn[1024];
        strcpy(lockfn, tmpf);
        strcat(lockfn, ".lock");
        lock_info_t li;

        lock_file(lockfn, &li);
        FILE* extf = NULL;
        if (access(extfn, F_OK) != 0) {
            extf = fopen(extfn, "w");
        }
        unlock_file(&li);
        if (!extf) {
            // use temp filename
            filesystem = tempnam(NULL, "ipsw_");
            if (!filesystem) {
                error("WARNING: Could not get temporary filename, using '%s' in current directory\n", fsname);
                filesystem = strdup(fsname);
            }
            delete_fs = 1;
        } else {
            // use <fsname>.extract as filename
            filesystem = strdup(extfn);
            fclose(extf);
        }
        remove(lockfn);

        // Extract filesystem from IPSW
        info("Extracting filesystem from IPSW\n");
        retassure(!ipsw_extract_to_file_with_progress(client->ipsw, fsname, filesystem, 1),"ERROR: Unable to extract filesystem from IPSW\n");

        if (strstr(filesystem, ".extract")) {
            // rename <fsname>.extract to <fsname>
            remove(tmpf);
            rename(filesystem, tmpf);
            free(filesystem);
            filesystem = strdup(tmpf);
        }
    }

    if (_rerestoreiOS9) {
        
        if (dfu_send_component(client, build_identity, "iBSS") < 0) {
            irecv_close(client->dfu->client);
            client->dfu->client = NULL;
            reterror("ERROR: Unable to send iBSS to device\n");
        }

        /* reconnect */
        dfu_client_free(client);
        
        debug("Waiting for device to disconnect...\n");
        cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 10000);
        retassure((client->mode == &idevicerestore_modes[MODE_UNKNOWN] || (mutex_unlock(&client->device_event_mutex),0)), "Device did not disconnect. Possibly invalid iBSS. Reset device and try again");

        debug("Waiting for device to reconnect...\n");
        cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 10000);
        retassure((client->mode == &idevicerestore_modes[MODE_DFU] || (mutex_unlock(&client->device_event_mutex),0)), "Device did not disconnect. Possibly invalid iBSS. Reset device and try again");
        mutex_unlock(&client->device_event_mutex);
        
        dfu_client_new(client);

        /* send iBEC */
        if (dfu_send_component(client, build_identity, "iBEC") < 0) {
            irecv_close(client->dfu->client);
            client->dfu->client = NULL;
            reterror("ERROR: Unable to send iBEC to device\n");
        }
        
        dfu_client_free(client);
        
        debug("Waiting for device to disconnect...\n");
        cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 10000);
        retassure((client->mode == &idevicerestore_modes[MODE_UNKNOWN] || (mutex_unlock(&client->device_event_mutex),0)), "Device did not disconnect. Possibly invalid iBEC. Reset device and try again");

        debug("Waiting for device to reconnect...\n");
        cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 10000);
        retassure((client->mode == &idevicerestore_modes[MODE_RECOVERY] || (mutex_unlock(&client->device_event_mutex),0)), "Device did not disconnect. Possibly invalid iBEC. Reset device and try again");
        mutex_unlock(&client->device_event_mutex);

        
    }else{
        if ((client->build_major > 8)) {
            if (!client->image4supported) {
                /* send ApTicket */
                if (recovery_send_ticket(client) < 0) {
                    error("WARNING: Unable to send APTicket\n");
                }
            }
        }
    }



    if (_enterPwnRecoveryRequested){ //if pwnrecovery send all components decrypted, unless we're dealing with iOS 10
        if (!_client->image4supported) {
            if (strncmp(client->version, "10.", 3))
                client->recovery_custom_component_function = get_custom_component;
        }
    }else if (!_rerestoreiOS9){
        /* now we load the iBEC */
        retassure(!recovery_send_ibec(client, build_identity),"ERROR: Unable to send iBEC\n");

        printf("waiting for device to reconnect... ");
        recovery_client_free(client);
        
        debug("Waiting for device to disconnect...\n");
        cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 10000);
        retassure((client->mode == &idevicerestore_modes[MODE_UNKNOWN] || (mutex_unlock(&client->device_event_mutex),0)), "Device did not disconnect. Possibly invalid iBEC. Reset device and try again");

        debug("Waiting for device to reconnect...\n");
        cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 10000);
        retassure((client->mode == &idevicerestore_modes[MODE_RECOVERY] || (mutex_unlock(&client->device_event_mutex),0)), "Device did not disconnect. Possibly invalid iBEC. Reset device and try again");
        mutex_unlock(&client->device_event_mutex);
    }

    retassure(client->mode == &idevicerestore_modes[MODE_RECOVERY], "failed to reconnect to device in recovery (iBEC) mode\n");

    //do magic
    if (_client->image4supported) get_sep_nonce(client, &client->sepnonce, &client->sepnonce_size);
    get_ap_nonce(client, &client->nonce, &client->nonce_size);

    get_ecid(client, &client->ecid);

    if (client->mode->index == MODE_RECOVERY) {
        retassure(client->srnm,"ERROR: could not retrieve device serial number. Can't continue.\n");

        retassure(!irecv_send_command(client->recovery->client, "bgcolor 0 255 0"), "ERROR: Unable to set bgcolor\n");

        info("[WARNING] Setting bgcolor to green! If you don't see a green screen, then your device didn't boot iBEC correctly\n");
        sleep(2); //show the user a green screen!

        retassure(!recovery_enter_restore(client, build_identity),"ERROR: Unable to place device into restore mode\n");

        recovery_client_free(client);
    }

    if (_client->image4supported) {
        retassure(!get_tss_response(client, sep_build_identity, &client->septss), "ERROR: Unable to get SHSH blobs for SEP\n");
        retassure(_client->sepfwdatasize && _client->sepfwdata, "SEP not loaded, refusing to continue");
    }

    
    
    mutex_lock(&client->device_event_mutex);
    debug("Waiting for device to disconnect...\n");
    cond_wait_timeout(&client->device_event_cond, &client->device_event_mutex, 180000);
    retassure((client->mode == &idevicerestore_modes[MODE_RESTORE] || (mutex_unlock(&client->device_event_mutex),0)), "Device failed to enter restore mode");
    mutex_unlock(&client->device_event_mutex);

    info("About to restore device... \n");
    int result = 0;
    retassure(!(result = restore_device(client, build_identity, filesystem)), "ERROR: Unable to restore device\n");
}

int futurerestore::doJustBoot(const char *ipsw, string bootargs){
    reterror("not implemented");
//    int err = 0;
//    //some memory might not get freed if this function throws an exception, but you probably don't want to catch that anyway.
//
//    struct idevicerestore_client_t* client = _client;
//    int unused;
//    int result = 0;
//    plist_t buildmanifest = NULL;
//    plist_t build_identity = NULL;
//
//    client->ipsw = strdup(ipsw);
//
//    getDeviceMode(true);
//    info("Found device in %s mode\n", client->mode->string);
//
//    retassure((client->mode->index == MODE_DFU || client->mode->index == MODE_RECOVERY) && _enterPwnRecoveryRequested, "device not in DFU/Recovery mode\n");
//
//    // discover the device type
//    retassure(check_hardware_model(client) && client->device,"ERROR: Unable to discover device model\n");
//    info("Identified device as %s, %s\n", client->device->hardware_model, client->device->product_type);
//
//    // verify if ipsw file exists
//    retassure(!access(client->ipsw, F_OK), "ERROR: Firmware file %s does not exist.\n", client->ipsw);
//    info("Extracting BuildManifest from IPSW\n");
//
//    retassure(!ipsw_extract_build_manifest(client->ipsw, &buildmanifest, &unused),"ERROR: Unable to extract BuildManifest from %s. Firmware file might be corrupt.\n", client->ipsw);
//
//    /* check if device type is supported by the given build manifest */
//    retassure(!build_manifest_check_compatibility(buildmanifest, client->device->product_type),"ERROR: Could not make sure this firmware is suitable for the current device. Refusing to continue.\n");
//
//    /* print iOS information from the manifest */
//    build_manifest_get_version_information(buildmanifest, client);
//
//    info("Product Version: %s\n", client->version);
//    info("Product Build: %s Major: %d\n", client->build, client->build_major);
//
//    client->image4supported = is_image4_supported(client);
//    info("Device supports Image4: %s\n", (client->image4supported) ? "true" : "false");
//
//    retassure(build_identity = getBuildidentityWithBoardconfig(buildmanifest, client->device->hardware_model, 0),"ERROR: Unable to find any build identities for IPSW\n");
//
//
//    /* print information about current build identity */
//    build_identity_print_information(build_identity);
//
//
//    //check for enterpwnrecovery, because we could be in DFU mode
//    retassure(_enterPwnRecoveryRequested, "enterPwnRecoveryRequested is not set, but required");
//
//    retassure(getDeviceMode(true) == MODE_DFU || getDeviceMode(false) == MODE_RECOVERY, "unexpected device mode\n");
//
//    enterPwnRecovery(build_identity, bootargs);
//
//    client->recovery_custom_component_function = get_custom_component;
//
//    for (int i=0;getDeviceMode(true) != MODE_RECOVERY && i<40; i++) putchar('.'),usleep(USEC_PER_SEC*0.5);
//    putchar('\n');
//
//    retassure(check_mode(client), "failed to reconnect to device in recovery (iBEC) mode\n");
//
//    get_ecid(client, &client->ecid);
//
//    client->flags |= FLAG_BOOT;
//
//    if (client->mode->index == MODE_RECOVERY) {
//        retassure(client->srnm,"ERROR: could not retrieve device serial number. Can't continue.\n");
//
//        retassure(!irecv_send_command(client->recovery->client, "bgcolor 0 255 0"), "ERROR: Unable to set bgcolor\n");
//
//        info("[WARNING] Setting bgcolor to green! If you don't see a green screen, then your device didn't boot iBEC correctly\n");
//        sleep(2); //show the user a green screen!
//        client->image4supported = true; //dirty hack to not require apticket
//
//        retassure(!recovery_enter_restore(client, build_identity),"ERROR: Unable to place device into restore mode\n");
//
//        client->image4supported = false;
//        recovery_client_free(client);
//    }
//
//    info("Cleaning up...\n");
//
//error:
//    safeFree(client->sepfwdata);
//    safeFreeCustom(buildmanifest, plist_free);
//    if (!result && !err) info("DONE\n");
//    return result ? abs(result) : err;
}

futurerestore::~futurerestore(){
    recovery_client_free(_client);
    idevicerestore_client_free(_client);
    for (auto im4m : _im4ms){
        safeFree(im4m.first);
    }
    safeFree(_ibootBuild);
    safeFree(_firmwareJson);
    safeFree(_firmwareTokens);
    safeFree(__latestManifest);
    safeFree(__latestFirmwareUrl);
    for (auto plist : _aptickets){
        safeFreeCustom(plist, plist_free);
    }
    safeFreeCustom(_sepbuildmanifest, plist_free);
    safeFreeCustom(_basebandbuildmanifest, plist_free);
}

void futurerestore::loadFirmwareTokens(){
    if (!_firmwareTokens){
        if (!_firmwareJson) _firmwareJson = getFirmwareJson();
        retassure(_firmwareJson,"[TSSC] could not get firmware.json\n");
        int cnt = parseTokens(_firmwareJson, &_firmwareTokens);
        retassure(cnt>0,"[TSSC] parsing %s.json failed\n",(0) ? "ota" : "firmware");
    }
}

const char *futurerestore::getDeviceModelNoCopy(){
    if (!_client->device || !_client->device->product_type){

        int mode = getDeviceMode(true);
        retassure(mode == MODE_NORMAL || mode == MODE_RECOVERY || mode == MODE_DFU, "unexpected device mode=%d\n",mode);
        
        switch (mode) {
        case MODE_RESTORE:
            _client->device = restore_get_irecv_device(_client);
            break;
        case MODE_NORMAL:
            _client->device = normal_get_irecv_device(_client);
            break;
        case MODE_DFU:
        case MODE_RECOVERY:
            _client->device = dfu_get_irecv_device(_client);
            break;
        default:
            break;
        }
    }

    return _client->device->product_type;
}

const char *futurerestore::getDeviceBoardNoCopy(){
    if (!_client->device || !_client->device->product_type){

        int mode = getDeviceMode(true);
        retassure(mode == MODE_NORMAL || mode == MODE_RECOVERY || mode == MODE_DFU, "unexpected device mode=%d\n",mode);
        
        switch (mode) {
        case MODE_RESTORE:
            _client->device = restore_get_irecv_device(_client);
            break;
        case MODE_NORMAL:
            _client->device = normal_get_irecv_device(_client);
            break;
        case MODE_DFU:
        case MODE_RECOVERY:
            _client->device = dfu_get_irecv_device(_client);
            break;
        default:
            break;
        }
    }
    return _client->device->hardware_model;
}


char *futurerestore::getLatestManifest(){
    if (!__latestManifest){
        loadFirmwareTokens();

        const char *device = getDeviceModelNoCopy();
        t_iosVersion versVals;
        memset(&versVals, 0, sizeof(versVals));
        
        int versionCnt = 0;
        int i = 0;
        char **versions = getListOfiOSForDevice(_firmwareTokens, device, 0, &versionCnt);
        retassure(versionCnt, "[TSSC] failed finding latest iOS\n");
        char *bpos = NULL;
        while((bpos = strstr((char*)(versVals.version = strdup(versions[i++])),"[B]")) != 0){
            free((char*)versVals.version);
            if (--versionCnt == 0) reterror("[TSSC] automatic iOS selection couldn't find non-beta iOS\n");
        }
        info("[TSSC] selecting latest iOS: %s\n",versVals.version);
        if (bpos) *bpos= '\0';
        if (versions) free(versions[versionCnt-1]),free(versions);
        
        //make sure it get's freed after function finishes execution by either reaching end or throwing exception
        ptr_smart<const char*>autofree(versVals.version);
        
        __latestFirmwareUrl = getFirmwareUrl(device, &versVals, _firmwareTokens);
        retassure(__latestFirmwareUrl, "could not find url of latest firmware\n");
        
        __latestManifest = getBuildManifest(__latestFirmwareUrl, device, versVals.version, versVals.buildID, 0);
        retassure(__latestManifest, "could not get buildmanifest of latest firmware\n");
    }
    
    return __latestManifest;
}

char *futurerestore::getLatestFirmwareUrl(){
    return getLatestManifest(),__latestFirmwareUrl;
}


void futurerestore::loadLatestBaseband(){
    char * manifeststr = getLatestManifest();
    char *pathStr = getPathOfElementInManifest("BasebandFirmware", manifeststr, getDeviceModelNoCopy(), 0);
    info("downloading Baseband\n\n");
    retassure(!downloadPartialzip(getLatestFirmwareUrl(), pathStr, _basebandPath = BASEBAND_TMP_PATH), "could not download baseband\n");
    saveStringToFile(manifeststr, BASEBAND_MANIFEST_TMP_PATH);
    setBasebandManifestPath(BASEBAND_MANIFEST_TMP_PATH);
    setBasebandPath(BASEBAND_TMP_PATH);
}

void futurerestore::loadLatestSep(){
    char * manifeststr = getLatestManifest();
    char *pathStr = getPathOfElementInManifest("SEP", manifeststr, getDeviceModelNoCopy(), 0);
    info("downloading SEP\n\n");
    retassure(!downloadPartialzip(getLatestFirmwareUrl(), pathStr, SEP_TMP_PATH), "could not download SEP\n");
    loadSep(SEP_TMP_PATH);
    saveStringToFile(manifeststr, SEP_MANIFEST_TMP_PATH);
    setSepManifestPath(SEP_MANIFEST_TMP_PATH);
}

void futurerestore::setSepManifestPath(const char *sepManifestPath){
    retassure(_sepbuildmanifest = loadPlistFromFile(_sepbuildmanifestPath = sepManifestPath), "failed to load SEPManifest");
}

void futurerestore::setBasebandManifestPath(const char *basebandManifestPath){
    retassure(_basebandbuildmanifest = loadPlistFromFile(_basebandbuildmanifestPath = basebandManifestPath), "failed to load BasebandManifest");
};

void futurerestore::loadSep(const char *sepPath){
    FILE *fsep = NULL;
    retassure(fsep = fopen(sepPath, "rb"), "failed to read SEP\n");
    
    fseek(fsep, 0, SEEK_END);
    _client->sepfwdatasize = ftell(fsep);
    fseek(fsep, 0, SEEK_SET);
    
    retassure(_client->sepfwdata = (char*)malloc(_client->sepfwdatasize), "failed to malloc memory for SEP\n");
    
    size_t freadRet=0;
    retassure((freadRet = fread(_client->sepfwdata, 1, _client->sepfwdatasize, fsep)) == _client->sepfwdatasize,
              "failed to load SEP. size=%zu but fread returned %zu\n",_client->sepfwdatasize,freadRet);
    
    fclose(fsep);
}


void futurerestore::setBasebandPath(const char *basebandPath){
    FILE *fbb = NULL;

    retassure(fbb = fopen(basebandPath, "rb"), "failed to read Baseband");
    _basebandPath = basebandPath;
    fclose(fbb);
}


#pragma mark static methods

inline void futurerestore::saveStringToFile(const char *str, const char *path){
    FILE *f = NULL;
    retassure(f = fopen(path, "w"), "can't save file at %s\n",path);
    size_t len = strlen(str);
    size_t wlen = fwrite(str, 1, len, f);
    fclose(f);
    retassure(len == wlen, "saving file failed, wrote=%zu actual=%zu\n",wlen,len);
}

std::pair<const char *,size_t> futurerestore::getNonceFromSCAB(const char* scab, size_t scabSize){
    retassure(scab, "Got empty SCAB\n");
    
    img4tool::ASN1DERElement bacs(scab,scabSize);
    
    try {
        bacs[3];
    } catch (...) {
        reterror("unexpected number of Elements in SCAB sequence (expects 4)\n");
    }
        
    img4tool::ASN1DERElement mainSet = bacs[1];

    for (auto &elem : mainSet) {
        if (*(uint8_t*)elem.buf() == 0x92) {
            return {(char*)elem.payload(),elem.payloadSize()};
        }
    }
    reterror("failed to get nonce from SCAB");
    return {NULL,0};
}

uint64_t futurerestore::getEcidFromSCAB(const char* scab, size_t scabSize){
    retassure(scab, "Got empty SCAB\n");
    
    img4tool::ASN1DERElement bacs(scab,scabSize);
    
    try {
        bacs[3];
    } catch (...) {
        reterror("unexpected number of Elements in SCAB sequence (expects 4)\n");
    }
    
    img4tool::ASN1DERElement mainSet = bacs[1];

    for (auto &elem : mainSet) {
        if (*(uint8_t*)elem.buf() == 0x81) {
            uint64_t ret = 0;
            for (int i=0; i<elem.payloadSize(); i++) {
                ret <<=8;
                ret |= ((uint8_t*)elem.payload())[i];
            }
            return ret;
        }
    }

    reterror("failed to get ECID from SCAB");
    return 0;
}

std::pair<const char *,size_t>futurerestore::getRamdiskHashFromSCAB(const char* scab, size_t scabSize){
    retassure(scab, "Got empty SCAB\n");

    img4tool::ASN1DERElement bacs(scab,scabSize);

    try {
        bacs[3];
    } catch (...) {
        reterror("unexpected number of Elements in SCAB sequence (expects 4)\n");
    }

    img4tool::ASN1DERElement mainSet = bacs[1];

    for (auto &elem : mainSet) {
        if (*(uint8_t*)elem.buf() == 0x9A) {
            return {(char*)elem.payload(),elem.payloadSize()};
        }
    }
    reterror("failed to get nonce from SCAB");
    return {NULL,0};
}

plist_t futurerestore::loadPlistFromFile(const char *path){
    plist_t ret = NULL;
    
    FILE *f = fopen(path,"rb");
    if (!f){
        error("could not open file %s\n",path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size_t bufSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = (char*)malloc(bufSize);
    if (!buf){
        error("failed to alloc memory\n");
        return NULL;
    }
    
    size_t freadRet = 0;
    if ((freadRet = fread(buf, 1, bufSize, f)) != bufSize){
        error("fread=%zu but bufsize=%zu",freadRet,bufSize);
        return NULL;
    }
    fclose(f);
    
    if (memcmp(buf, "bplist00", 8) == 0)
        plist_from_bin(buf, (uint32_t)bufSize, &ret);
    else
        plist_from_xml(buf, (uint32_t)bufSize, &ret);
    free(buf);
    
    return ret;
}

char *futurerestore::getPathOfElementInManifest(const char *element, const char *manifeststr, const char *model, int isUpdateInstall){
    char *pathStr = NULL;
    ptr_smart<plist_t> buildmanifest(NULL,plist_free);
    
    plist_from_xml(manifeststr, (uint32_t)strlen(manifeststr), &buildmanifest);
    
    if (plist_t identity = getBuildidentity(buildmanifest._p, model, isUpdateInstall))
        if (plist_t manifest = plist_dict_get_item(identity, "Manifest"))
            if (plist_t elem = plist_dict_get_item(manifest, element))
                if (plist_t info = plist_dict_get_item(elem, "Info"))
                    if (plist_t path = plist_dict_get_item(info, "Path"))
                        if (plist_get_string_val(path, &pathStr), pathStr)
                            goto noerror;
    
    reterror("could not get %s path\n",element);
noerror:
    return pathStr;
}

