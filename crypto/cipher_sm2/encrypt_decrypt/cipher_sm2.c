#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/crypto.h>
#include <crypto/akcipher.h>
#include <crypto/public_key.h>
#include <crypto/sm2.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/oid_registry.h>
#include <keys/asymmetric-subtype.h>
#include <keys/asymmetric-type.h>
#include <keys/system_keyring.h>
#include <crypto/public_key.h>

// 测试密钥对 - 实际应用中应从安全源获取
static const u8 private_der[32] = {
        0x16, 0x49, 0x29, 0x84, 0x5F, 0x19, 0xE2, 0xE1,
        0xA9, 0xFB, 0x56, 0x69, 0xEA, 0x3C, 0x32, 0x77,
        0x55, 0x22, 0x80, 0x0B, 0x2E, 0x9B, 0x9F, 0xC5,
        0x4A, 0x81, 0x72, 0x9E, 0x2B, 0xCB, 0x8E, 0x40
};
unsigned int private_der_len = 32;
static const u8 public_der[65] = {
        0x04, // 未压缩格式标识
        // X坐标
        0x32, 0x10, 0x6C, 0xD0, 0x9F, 0xE7, 0x5F, 0x9E,
        0xE9, 0x34, 0xDB, 0x37, 0x68, 0x87, 0x4C, 0x07,
        0x18, 0x3D, 0xF1, 0xE6, 0x51, 0x7D, 0x3D, 0x9D,
        0x79, 0xDE, 0xFE, 0x8C, 0x01, 0x7A, 0x41, 0x5D,
        // Y坐标
        0x64, 0x28, 0xE9, 0x7E, 0x5B, 0x55, 0x4F, 0xE1,
        0xAC, 0x0D, 0x35, 0x66, 0x5F, 0x4E, 0xA5, 0x53,
        0x92, 0x1A, 0x41, 0x7D, 0xF7, 0x6E, 0x35, 0xEB,
        0xEA, 0x0A, 0x5F, 0x0B, 0x32, 0x8F, 0x5D, 0xE2
};
unsigned int public_der_len = 65;

#define MODULE_NAME "sm2_crypto"
#define TEST_STRING "linux-kdev"

void hexdump(const char *prefix, const void *data, int len);
void *g_ciphertext = NULL;
size_t g_ciphertext_len = 0;

void hexdump(const char *prefix, const void *data, int len) {
    print_hex_dump(KERN_INFO, prefix, DUMP_PREFIX_OFFSET,
                   16, 1, data, len, true);
}

static int sm2_pub_encrypt(const void *plaintext, size_t plaintext_len)
{
    struct scatterlist src, dst;
    int ret;
    struct crypto_akcipher *encrypt_tfm;
    struct akcipher_request *encrypt_req;
    struct crypto_wait encrypt_wait;
    size_t key_size;
    void *inbuf_enc =NULL;

    /* Allocate crypto transforms */
    encrypt_tfm = crypto_alloc_akcipher("sm2", 0, 0);
    if (IS_ERR(encrypt_tfm)) {
        pr_err("Failed to allocate encryption akcipher handle: %ld\n", PTR_ERR(encrypt_tfm));
        ret = PTR_ERR(encrypt_tfm);
        goto free_encrypt_tfm;
    }
    pr_info("kdev: Encrypt crypto_alloc_akcipher ok\n");

    /* Allocate requests */
    encrypt_req = akcipher_request_alloc(encrypt_tfm, GFP_KERNEL);
    if (IS_ERR(encrypt_req)) {
        pr_err("Failed to allocate encryption akcipher request\n");
        ret = -ENOMEM;
        goto free_encrypt_req;
    }
    pr_info("kdev: Encrypt akcipher_request_alloc ok\n");

    crypto_init_wait(&encrypt_wait);

    ret = crypto_akcipher_set_pub_key(encrypt_tfm, public_der, public_der_len);
    if (ret) {
        pr_err("Failed to set public key: %d\n", ret);
        return ret;
    }
    pr_info("kdev: Encrypt set public key ok\n");
    pr_info("Using cipher: %s\n", crypto_akcipher_tfm(encrypt_tfm)->__crt_alg->cra_name);

    key_size = crypto_akcipher_maxsize(encrypt_tfm);
    g_ciphertext = kzalloc(key_size, GFP_KERNEL);
    if (!g_ciphertext) {
        ret = -ENOMEM;
        goto free_encrypt_req;
    }
    // plaintext must be writable
    inbuf_enc  = kzalloc(plaintext_len , GFP_KERNEL);
    if (!inbuf_enc) {
        ret = -ENOMEM;
        goto free_encrypt_req;
    }
    memcpy(inbuf_enc, plaintext, plaintext_len);

    pr_info("Encrypting %zu bytes with public key\n", plaintext_len);

    /* Prepare scatterlists */
    sg_init_one(&src, inbuf_enc, plaintext_len);
    hexdump("PLAINTEXT: ", inbuf_enc, plaintext_len);
    sg_init_one(&dst, g_ciphertext, key_size);

    /* Set up request */
    akcipher_request_set_callback(encrypt_req, CRYPTO_TFM_REQ_MAY_BACKLOG,
                                  crypto_req_done, &encrypt_wait);
    akcipher_request_set_crypt(encrypt_req, &src, &dst, plaintext_len, key_size);

    /* Perform encryption */
    ret = crypto_wait_req(crypto_akcipher_encrypt(encrypt_req), &encrypt_wait);
    if (ret) {
        pr_err("Public key encryption failed: %d\n", ret);
        goto free_encrypt_req;
    }
    g_ciphertext_len = encrypt_req->dst_len;

    pr_info("Encrypted to %zu bytes\n", g_ciphertext_len);
    hexdump("CIPHERTEXT: ", g_ciphertext, g_ciphertext_len);
    ret = 0;

free_encrypt_req:
    akcipher_request_free(encrypt_req);
free_encrypt_tfm:
    crypto_free_akcipher(encrypt_tfm);
    return ret;
}

static int sm2_priv_decrypt(void *ciphertext, size_t ciphertext_len)
{
    struct scatterlist src, dst;
    int ret;
    size_t key_size;
    struct crypto_akcipher *decrypt_tfm;
    struct akcipher_request *decrypt_req;
    struct crypto_wait decrypt_wait;
    void *decrypted;
    size_t decrypted_len;

    decrypt_tfm = crypto_alloc_akcipher("sm2", 0, 0);
    if (IS_ERR(decrypt_tfm)) {
        pr_err("Failed to allocate decryption akcipher handle: %ld\n", PTR_ERR(decrypt_tfm));
        ret = PTR_ERR(decrypt_tfm);
        goto free_decrypt_tfm;
    }
    pr_info("kdev: Decrypt crypto_alloc_akcipher ok\n");

    decrypt_req = akcipher_request_alloc(decrypt_tfm, GFP_KERNEL);
    if (!decrypt_req) {
        pr_err("Failed to allocate decryption akcipher request\n");
        ret = -ENOMEM;
        goto free_decrypt_req;
    }
    pr_info("kdev: Decrypt akcipher_request_alloc ok\n");

    crypto_init_wait(&decrypt_wait);

    /* Set private key for decryption */
    hexdump("PRIVATE: ", private_der, private_der_len);

    ret = crypto_akcipher_set_priv_key(decrypt_tfm, private_der, private_der_len);
    if (ret) {
        pr_err("Failed to set private key: %d\n", ret);
        goto free_decrypt_req;
    }
    pr_info("kdev: Decrypt set private key ok\n");


    key_size = crypto_akcipher_maxsize(decrypt_tfm);
    decrypted = kzalloc(key_size, GFP_KERNEL);
    if (!decrypted) {
        ret = -ENOMEM;
        goto free_decrypt_req;
    }

    pr_info("Decrypting %zu bytes with private key\n", ciphertext_len);
    if (!ciphertext) {
        pr_err("ciphertext is null!\n");
        ret = -ENOMEM;
        goto free_decrypted;
    }
    hexdump("CIPHERTEXT: ", ciphertext, ciphertext_len);


    pr_info("Using cipher: %s\n", crypto_akcipher_tfm(decrypt_tfm)->__crt_alg->cra_name);

    // 确保密文长度等于密钥长度 sm2 加密的密文长度必须等于模数长度
    if (ciphertext_len != key_size) {
        pr_err("Ciphertext length %zu doesn't match key size %zu\n",
               ciphertext_len, key_size);
        ret = -EINVAL;
        goto free_decrypted;
    }

    /* Prepare scatterlists */
    sg_init_one(&src, ciphertext, ciphertext_len);
    sg_init_one(&dst, decrypted, key_size);

    /* Set up request */
    akcipher_request_set_callback(decrypt_req, CRYPTO_TFM_REQ_MAY_BACKLOG,
                                  crypto_req_done, &decrypt_wait);
    akcipher_request_set_crypt(decrypt_req, &src, &dst, ciphertext_len, key_size);

    /* Perform decryption */
    ret = crypto_wait_req(crypto_akcipher_decrypt(decrypt_req), &decrypt_wait);
    if (ret) {
        pr_err("Private key decryption failed: %d\n", ret);
        goto free_decrypted;
    }

    decrypted_len = decrypt_req->dst_len;
    pr_info("Decrypted to %zu bytes\n", decrypted_len);
    hexdump("DECRYPTED: ", decrypted, key_size);
    ret = 0;

free_decrypted:
    kfree(decrypted);
free_decrypt_req:
    akcipher_request_free(decrypt_req);
free_decrypt_tfm:
    crypto_free_akcipher(decrypt_tfm);
    return ret;
}

static int __init sm2_crypto_init(void)
{
    int ret;
    const char *plaintext = TEST_STRING;
    size_t plaintext_len = strlen(plaintext);

    /* Encrypt with public key */
    pr_info("kdev: Encrypt test\n");
    ret = sm2_pub_encrypt(plaintext, plaintext_len);
    if (ret)
        goto out;

    /* Decrypt with private key */
    pr_info("kdev: Decrypt test\n");
    ret = sm2_priv_decrypt(g_ciphertext, g_ciphertext_len);
    if (ret)
        goto out;
    ret = 0;
out:
    kfree(g_ciphertext);
    return ret;
}

static void __exit sm2_crypto_exit(void) {
    pr_info("sm2 crypto module unloaded\n");
}

module_init(sm2_crypto_init);
module_exit(sm2_crypto_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("sm2 Crypto Module for Linux 6.6");
MODULE_VERSION("1.0");
