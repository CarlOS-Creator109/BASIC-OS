/* crypttest.c - prueba crypto.c contra vectores oficiales. Compila en Linux: make bn-crypto-test */
#include "crypto.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static int hex(char c){ return c<='9'?c-'0':(c|32)-'a'+10; }
static void unhex(const char *h, u8 *o, int n){ int i; for(i=0;i<n;i++) o[i]=(u8)(hex(h[2*i])<<4|hex(h[2*i+1])); }
static void check(const char *name, const u8 *got, const char *want_hex, int n)
{
    u8 w[256]; unhex(want_hex, w, n);
    if (memcmp(got, w, n)) {
        int i; printf("  FALLO %s\n    esperado: %s\n    obtenido: ", name, want_hex);
        for (i = 0; i < n; i++) printf("%02x", got[i]); printf("\n"); fails++;
    } else printf("  ok    %s\n", name);
}

int main(void)
{
    u8 o[256], key[64], in[256], tag[16], aad[64];

    printf("SHA-256:\n");
    sha256((const u8 *)"abc", 3, o);
    check("\"abc\"", o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);
    sha256((const u8 *)"", 0, o);
    check("vacio", o, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32);

    printf("HMAC-SHA256 (RFC 4231 #2):\n");
    hmac_sha256((const u8 *)"Jefe", 4, (const u8 *)"what do ya want for nothing?", 28, o);
    check("Jefe", o, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);

    printf("HKDF-SHA256 (RFC 5869 #1):\n");
    {
        u8 ikm[22], salt[13], info[10], prk[32];
        memset(ikm, 0x0b, 22);
        unhex("000102030405060708090a0b0c", salt, 13);
        unhex("f0f1f2f3f4f5f6f7f8f9", info, 10);
        hkdf_extract(salt, 13, ikm, 22, prk);
        check("extract", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32);
        hkdf_expand(prk, info, 10, o, 42);
        check("expand", o, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", 42);
    }

    printf("AES-128 (FIPS-197):\n");
    {
        aes128 a; unhex("000102030405060708090a0b0c0d0e0f", key, 16);
        unhex("00112233445566778899aabbccddeeff", in, 16);
        aes128_init(&a, key); aes128_encrypt(&a, in, o);
        check("bloque", o, "69c4e0d86a7b0430d8cdb78070b4c55a", 16);
    }

    printf("AES-128-GCM (NIST #4):\n");
    {
        u8 iv[12];
        unhex("feffe9928665731c6d6a8f9467308308", key, 16);
        unhex("cafebabefacedbaddecaf888", iv, 12);
        unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", in, 60);
        unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
        aes128_gcm_encrypt(key, iv, 12, aad, 20, in, 60, o, tag);
        check("cifrado", o, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", 60);
        check("tag", tag, "5bc94fbc3221a5db94fae95ae7121a47", 16);
        {
            u8 dec[60], t[16]; unhex("5bc94fbc3221a5db94fae95ae7121a47", t, 16);
            int r = aes128_gcm_decrypt(key, iv, 12, aad, 20, o, 60, t, dec);
            printf("  %s  descifrado+tag\n", (!r && !memcmp(dec, in, 60)) ? "ok   " : "FALLO");
            if (r || memcmp(dec, in, 60)) fails++;
            t[0] ^= 1;
            printf("  %s  rechaza tag malo\n", aes128_gcm_decrypt(key, iv, 12, aad, 20, o, 60, t, dec) ? "ok   " : "FALLO");
            if (!aes128_gcm_decrypt(key, iv, 12, aad, 20, o, 60, t, dec)) fails++;
        }
    }

    printf("X25519 (RFC 7748 §5.2):\n");
    {
        u8 s[32], u[32];
        unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", s, 32);
        unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
        x25519(o, s, u);
        check("escalar*punto", o, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);
    }
    printf("X25519 (ida y vuelta Alice/Bob):\n");
    {
        u8 ka[32], kb[32], pa[32], pb[32], sa[32], sb[32];
        unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", ka, 32);
        unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", kb, 32);
        x25519_base(pa, ka); x25519_base(pb, kb);
        check("pub Alice", pa, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
        check("pub Bob", pb, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
        x25519(sa, ka, pb); x25519(sb, kb, pa);
        printf("  %s  secreto compartido igual\n", !memcmp(sa, sb, 32) ? "ok   " : "FALLO");
        if (memcmp(sa, sb, 32)) fails++;
        check("secreto", sa, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    }

    printf("SHA-512 / SHA-384 (FIPS):\n");
    {
        u8 o64[64];
        sha512((const u8 *)"abc", 3, o64);
        check("sha512 abc", o64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 64);
        sha384((const u8 *)"abc", 3, o64);
        check("sha384 abc", o64, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7", 48);
    }

    printf("ECDSA P-256 (RFC 6979, msg \"sample\"):\n");
    {
        u8 pub[64], r[32], s[32], z[32];
        unhex("60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6", pub, 32);
        unhex("7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299", pub + 32, 32);
        unhex("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716", r, 32);
        unhex("f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8", s, 32);
        sha256((const u8 *)"sample", 6, z);
        printf("  %s  firma valida\n", ecdsa_p256_verify(pub, z, 32, r, s) ? "ok   " : "FALLO");
        if (!ecdsa_p256_verify(pub, z, 32, r, s)) fails++;
        r[31] ^= 1;
        printf("  %s  rechaza firma alterada\n", ecdsa_p256_verify(pub, z, 32, r, s) ? "FALLO" : "ok   ");
        if (ecdsa_p256_verify(pub, z, 32, r, s)) fails++;
    }

    printf(fails ? "\n%d PRUEBAS FALLARON\n" : "\nTODO BIEN\n", fails);
    return fails ? 1 : 0;
}
