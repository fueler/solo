/*
 *  Wrapper for crypto implementation on device
 *
 * */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "crypto.h"

#include "sha256.h"
#include "uECC.h"
#include "aes.h"
#include "ctap.h"

#include "ssi_pal_types.h"
#include "ssi_pal_mem.h"
#include "sns_silib.h"
#include "crys_ecpki_build.h"
#include "crys_ecpki_ecdsa.h"
#include "crys_ecpki_dh.h"
#include "crys_ecpki_kg.h"
#include "crys_ecpki_domain.h"
#include "crys_rnd.h"
#include "nrf52840.h"


const uint8_t attestation_cert_der[];
const uint16_t attestation_cert_der_size;
const uint8_t attestation_key[];
const uint16_t attestation_key_size;



/*static SHA256_CTX sha256_ctx;*/

struct CRYS_HASHUserContext_t sha256_ctx;

const CRYS_ECPKI_Domain_t* _es256_curve;
CRYS_RND_State_t     rndState_ptr;
CRYS_RND_WorkBuff_t  rndWorkBuff_ptr;




static const uint8_t * _signing_key = NULL;

// Secrets for testing only
static uint8_t master_secret[32] = "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff"
                                "\xff\xee\xdd\xcc\xbb\xaa\x99\x88\x77\x66\x55\x44\x33\x22\x11\x00";

static uint8_t transport_secret[32] = "\x10\x01\x22\x33\x44\x55\x66\x77\x87\x90\x0a\xbb\x3c\xd8\xee\xff"
                                "\xff\xee\x8d\x1c\x3b\xfa\x99\x88\x77\x86\x55\x44\xd3\xff\x33\x00";



void crypto_sha256_init()
{
    /*sha256_init(&sha256_ctx);*/
    int ret = CRYS_HASH_Init(&sha256_ctx, CRYS_HASH_SHA256_mode);
    if (ret != CRYS_OK )
    {
        printf("sha init fail\n");
        exit(1);
    }
}

void crypto_reset_master_secret()
{
    ctap_generate_rng(master_secret, 32);
}


void crypto_sha256_update(uint8_t * data, size_t len)
{
    /*sha256_update(&sha256_ctx, data, len);*/
    int ret = CRYS_HASH_Update(&sha256_ctx, data, len);
    if (ret != CRYS_OK )
    {
        printf("sha update fail\n");
        exit(1);
    }

}

void crypto_sha256_update_secret()
{
    /*sha256_update(&sha256_ctx, master_secret, 32);*/
    int ret = CRYS_HASH_Update(&sha256_ctx, master_secret, 32);
    if (ret != CRYS_OK )
    {
        printf("sha update secret fail\n");
        exit(1);
    }
}

void crypto_sha256_final(uint8_t * hash)
{
    /*sha256_final(&sha256_ctx, hash);*/
    int ret = CRYS_HASH_Finish(&sha256_ctx, (uint32_t*)hash);
    if (ret != CRYS_OK )
    {
        printf("sha finish fail\n");
        exit(1);
    }


}


void crypto_sha256_hmac_init(uint8_t * key, uint32_t klen, uint8_t * hmac)
{
    uint8_t buf[64];
    int i;
    memset(buf, 0, sizeof(buf));

    if (key == CRYPTO_MASTER_KEY)
    {
        key = master_secret;
        klen = sizeof(master_secret);
    }

    if(klen > 64)
    {
        printf("Error, key size must be <= 64\n");
        exit(1);
    }

    memmove(buf, key, klen);

    for (i = 0; i < sizeof(buf); i++)
    {
        buf[i] = buf[i] ^ 0x36;
    }

    crypto_sha256_init();
    crypto_sha256_update(buf, 64);
}

void crypto_sha256_hmac_final(uint8_t * key, uint32_t klen, uint8_t * hmac)
{
    uint8_t buf[64];
    int i;
    crypto_sha256_final(hmac);
    memset(buf, 0, sizeof(buf));
    if (key == CRYPTO_MASTER_KEY)
    {
        key = master_secret;
        klen = sizeof(master_secret);
    }


    if(klen > 64)
    {
        printf("Error, key size must be <= 64\n");
        exit(1);
    }
    memmove(buf, key, klen);

    for (i = 0; i < sizeof(buf); i++)
    {
        buf[i] = buf[i] ^ 0x5c;
    }

    crypto_sha256_init();
    crypto_sha256_update(buf, 64);
    crypto_sha256_update(hmac, 32);
    crypto_sha256_final(hmac);
}


void crypto_ecc256_init()
{
    int ret;
    NVIC_EnableIRQ(CRYPTOCELL_IRQn);
    NRF_CRYPTOCELL->ENABLE = 1;

    ret = SaSi_LibInit();
    if (ret != SA_SILIB_RET_OK) {
        printf("Failed SaSi_LibInit - ret = 0x%x\n", ret);
        exit(1);
    }

    memset(&rndState_ptr, 0, sizeof(CRYS_RND_State_t));
    memset(&rndWorkBuff_ptr, 0, sizeof(CRYS_RND_WorkBuff_t));


    ret = CRYS_RndInit(&rndState_ptr, &rndWorkBuff_ptr);
    if (ret != SA_SILIB_RET_OK) {
        printf("Failed CRYS_RndInit - ret = 0x%x\n", ret);
        exit(1);
    }

    _es256_curve = CRYS_ECPKI_GetEcDomain(CRYS_ECPKI_DomainID_secp256r1);

    //
    uECC_set_rng((uECC_RNG_Function)ctap_generate_rng);
    //
}


void crypto_ecc256_load_attestation_key()
{
    _signing_key = attestation_key;
}

void crypto_ecc256_sign(uint8_t * data, int len, uint8_t * sig)
{
    CRYS_ECPKI_UserPrivKey_t UserPrivKey;
    CRYS_ECDSA_SignUserContext_t    SignUserContext;
    uint32_t sigsz = 64;
    int ret = CRYS_ECPKI_BuildPrivKey(_es256_curve,
                    _signing_key,
                    32,
                    &UserPrivKey);

    if (ret != SA_SILIB_RET_OK){
        printf(" CRYS_ECPKI_BuildPrivKey failed with 0x%x \n",ret);
        exit(1);
    }

    ret = CRYS_ECDSA_Sign(&rndState_ptr,
            CRYS_RND_GenerateVector,
            &SignUserContext,
            &UserPrivKey,
            CRYS_ECPKI_AFTER_HASH_SHA256_mode,
            data,
            len,
            sig,
            &sigsz);

    if (ret != SA_SILIB_RET_OK){
        printf(" CRYS_ECDSA_Sign failed with 0x%x \n",ret);
        exit(1);
    }
}


/*int uECC_compute_public_key(const uint8_t *private_key, uint8_t *public_key, uECC_Curve curve);*/
void derive_private_key_pair(uint8_t * data, int len, uint8_t * data2, int len2, uint8_t * privkey, uint8_t * pubkey);
void crypto_ecc256_derive_public_key(uint8_t * data, int len, uint8_t * x, uint8_t * y)
{
    uint8_t privkey[32];
    uint8_t pubkey[64];

    derive_private_key_pair(data,len,NULL,0,privkey,pubkey);

    /*memset(pubkey,0,sizeof(pubkey));*/
    /*uECC_compute_public_key(privkey, pubkey, uECC_secp256r1());*/
    memmove(x,pubkey,32);
    memmove(y,pubkey+32,32);
}


void crypto_ecc256_load_key(uint8_t * data, int len, uint8_t * data2, int len2)
{
    static uint8_t privkey[32];
    generate_private_key(data,len,data2,len2,privkey);
    _signing_key = privkey;
}

void crypto_ecc256_make_key_pair(uint8_t * pubkey, uint8_t * privkey)
{
    CRYS_ECPKI_UserPrivKey_t nrfpriv;
    CRYS_ECPKI_UserPublKey_t nrfpub;
    CRYS_ECPKI_KG_TempData_t tmp;
    uint8_t pubkey1[65];
    int ret;
    uint32_t sz;

    ret = CRYS_ECPKI_GenKeyPair(&rndState_ptr,
            CRYS_RND_GenerateVector,
            _es256_curve,
            &nrfpriv, &nrfpub, &tmp, NULL);

    if (ret != SA_SILIB_RET_OK){
        printf(" gen key failed with 0x%x \n",ret);
        exit(1);
    }

    sz = 32;
    CRYS_ECPKI_ExportPrivKey(&nrfpriv, privkey, &sz);
    sz = 65;
    CRYS_ECPKI_ExportPublKey(&nrfpub,CRYS_EC_PointUncompressed, pubkey1, &sz);
    memmove(pubkey, pubkey1+1, 64);

}

void crypto_ecc256_shared_secret(const uint8_t * pubkey, const uint8_t * privkey, uint8_t * shared_secret)
{
    if (uECC_shared_secret(pubkey, privkey, shared_secret, uECC_secp256r1()) != 1)
    {
        printf("Error, uECC_shared_secret failed\n");
        exit(1);
    }

}

uint8_t fixed_vector_hmac[32];
int fixed_vector_iter = 31;
uint32_t fixed_vector(void * rng, uint16_t sz, uint8_t * out)
{
    while(sz--)
    {
        *out++ = fixed_vector_hmac[fixed_vector_iter--];
        if (fixed_vector_iter == -1)
        {
            fixed_vector_iter = 31;
        }
    }
    return 0;
}

void derive_private_key_pair(uint8_t * data, int len, uint8_t * data2, int len2, uint8_t * privkey, uint8_t * pubkey)
{
    CRYS_ECPKI_UserPrivKey_t nrfpriv;
    CRYS_ECPKI_UserPublKey_t nrfpub;
    CRYS_ECPKI_KG_TempData_t tmp;
    uint32_t ret;
    uint32_t sz;
    int i;
    uint8_t pubkey1[65];

    crypto_sha256_hmac_init(CRYPTO_MASTER_KEY, 0, privkey);
    crypto_sha256_update(data, len);
    crypto_sha256_update(data2, len2);
    crypto_sha256_update(master_secret, 32);
    crypto_sha256_hmac_final(CRYPTO_MASTER_KEY, 0, privkey);

    memmove(fixed_vector_hmac, privkey, 32);
    fixed_vector_iter=31;

    /*privkey[31] += 1;*/
    for (i = 31; i > -1; i++)
    {
        privkey[i] += 1;
        if (privkey[i] != 0)
            break;
    }

    // There isn't a CC310 function for calculating a public key from a private,
    // so to get around it, we can "fix" the RNG input to GenKeyPair

    if(pubkey != NULL)
    {
        ret = CRYS_ECPKI_GenKeyPair(&rndState_ptr,
                fixed_vector,
                /*CRYS_RND_GenerateVector,*/
                _es256_curve,
                &nrfpriv, &nrfpub, &tmp, NULL);

        if (ret != SA_SILIB_RET_OK){
            printf(" gen key failed with 0x%x \n",ret);
            exit(1);
        }

    /*sz = 32;*/
    /*ret = CRYS_ECPKI_ExportPrivKey(&nrfpriv, privkey, &sz);*/
    /*if (ret != 0)*/
    /*{*/
        /*printf("privkey export fail\n");*/
        /*exit(1);*/
    /*}*/


        sz = 65;
        ret = CRYS_ECPKI_ExportPublKey(&nrfpub,CRYS_EC_PointUncompressed , pubkey1, &sz);
        if (ret != 0 || sz != 65)
        {
            printf("pubkey export fail 0x%04x\n",ret);
            exit(1);
        }
        if (pubkey1[0] != 0x04)
        {
            printf("pubkey uncompressed export fail 0x%02x\n",(int)pubkey1[0]);
            exit(1);
        }
        memmove(pubkey, pubkey1+1 , 64);
    }
}

void generate_private_key(uint8_t * data, int len, uint8_t * data2, int len2, uint8_t * privkey)
{
    derive_private_key_pair(data,len,data2,len2,privkey,NULL);
}

struct AES_ctx aes_ctx;
void crypto_aes256_init(uint8_t * key, uint8_t * nonce)
{
    if (key == CRYPTO_TRANSPORT_KEY)
    {
        AES_init_ctx(&aes_ctx, transport_secret);
    }
    else
    {
        AES_init_ctx(&aes_ctx, key);
    }
    if (nonce == NULL)
    {
        memset(aes_ctx.Iv, 0, 16);
    }
    else
    {
        memmove(aes_ctx.Iv, nonce, 16);
    }
}

// prevent round key recomputation
void crypto_aes256_reset_iv(uint8_t * nonce)
{
    if (nonce == NULL)
    {
        memset(aes_ctx.Iv, 0, 16);
    }
    else
    {
        memmove(aes_ctx.Iv, nonce, 16);
    }
}

void crypto_aes256_decrypt(uint8_t * buf, int length)
{
    AES_CBC_decrypt_buffer(&aes_ctx, buf, length);
}

void crypto_aes256_encrypt(uint8_t * buf, int length)
{
    AES_CBC_encrypt_buffer(&aes_ctx, buf, length);
}


const uint8_t attestation_cert_der[] =
"\x30\x82\x01\xfb\x30\x82\x01\xa1\xa0\x03\x02\x01\x02\x02\x01\x00\x30\x0a\x06\x08"
"\x2a\x86\x48\xce\x3d\x04\x03\x02\x30\x2c\x31\x0b\x30\x09\x06\x03\x55\x04\x06\x13"
"\x02\x55\x53\x31\x0b\x30\x09\x06\x03\x55\x04\x08\x0c\x02\x4d\x44\x31\x10\x30\x0e"
"\x06\x03\x55\x04\x0a\x0c\x07\x54\x45\x53\x54\x20\x43\x41\x30\x20\x17\x0d\x31\x38"
"\x30\x35\x31\x30\x30\x33\x30\x36\x32\x30\x5a\x18\x0f\x32\x30\x36\x38\x30\x34\x32"
"\x37\x30\x33\x30\x36\x32\x30\x5a\x30\x7c\x31\x0b\x30\x09\x06\x03\x55\x04\x06\x13"
"\x02\x55\x53\x31\x0b\x30\x09\x06\x03\x55\x04\x08\x0c\x02\x4d\x44\x31\x0f\x30\x0d"
"\x06\x03\x55\x04\x07\x0c\x06\x4c\x61\x75\x72\x65\x6c\x31\x15\x30\x13\x06\x03\x55"
"\x04\x0a\x0c\x0c\x54\x45\x53\x54\x20\x43\x4f\x4d\x50\x41\x4e\x59\x31\x22\x30\x20"
"\x06\x03\x55\x04\x0b\x0c\x19\x41\x75\x74\x68\x65\x6e\x74\x69\x63\x61\x74\x6f\x72"
"\x20\x41\x74\x74\x65\x73\x74\x61\x74\x69\x6f\x6e\x31\x14\x30\x12\x06\x03\x55\x04"
"\x03\x0c\x0b\x63\x6f\x6e\x6f\x72\x70\x70\x2e\x63\x6f\x6d\x30\x59\x30\x13\x06\x07"
"\x2a\x86\x48\xce\x3d\x02\x01\x06\x08\x2a\x86\x48\xce\x3d\x03\x01\x07\x03\x42\x00"
"\x04\x45\xa9\x02\xc1\x2e\x9c\x0a\x33\xfa\x3e\x84\x50\x4a\xb8\x02\xdc\x4d\xb9\xaf"
"\x15\xb1\xb6\x3a\xea\x8d\x3f\x03\x03\x55\x65\x7d\x70\x3f\xb4\x02\xa4\x97\xf4\x83"
"\xb8\xa6\xf9\x3c\xd0\x18\xad\x92\x0c\xb7\x8a\x5a\x3e\x14\x48\x92\xef\x08\xf8\xca"
"\xea\xfb\x32\xab\x20\xa3\x62\x30\x60\x30\x46\x06\x03\x55\x1d\x23\x04\x3f\x30\x3d"
"\xa1\x30\xa4\x2e\x30\x2c\x31\x0b\x30\x09\x06\x03\x55\x04\x06\x13\x02\x55\x53\x31"
"\x0b\x30\x09\x06\x03\x55\x04\x08\x0c\x02\x4d\x44\x31\x10\x30\x0e\x06\x03\x55\x04"
"\x0a\x0c\x07\x54\x45\x53\x54\x20\x43\x41\x82\x09\x00\xf7\xc9\xec\x89\xf2\x63\x94"
"\xd9\x30\x09\x06\x03\x55\x1d\x13\x04\x02\x30\x00\x30\x0b\x06\x03\x55\x1d\x0f\x04"
"\x04\x03\x02\x04\xf0\x30\x0a\x06\x08\x2a\x86\x48\xce\x3d\x04\x03\x02\x03\x48\x00"
"\x30\x45\x02\x20\x18\x38\xb0\x45\x03\x69\xaa\xa7\xb7\x38\x62\x01\xaf\x24\x97\x5e"
"\x7e\x74\x64\x1b\xa3\x7b\xf7\xe6\xd3\xaf\x79\x28\xdb\xdc\xa5\x88\x02\x21\x00\xcd"
"\x06\xf1\xe3\xab\x16\x21\x8e\xd8\xc0\x14\xaf\x09\x4f\x5b\x73\xef\x5e\x9e\x4b\xe7"
"\x35\xeb\xdd\x9b\x6d\x8f\x7d\xf3\xc4\x3a\xd7";


const uint16_t attestation_cert_der_size = sizeof(attestation_cert_der)-1;


const uint8_t attestation_key[] = "\xcd\x67\xaa\x31\x0d\x09\x1e\xd1\x6e\x7e\x98\x92\xaa\x07\x0e\x19\x94\xfc\xd7\x14\xae\x7c\x40\x8f\xb9\x46\xb7\x2e\x5f\xe7\x5d\x30";
const uint16_t attestation_key_size = sizeof(attestation_key)-1;


