#include <linux/module.h>
#include <linux/init.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/crypto.h>
#include <crypto/skcipher.h>
#include <crypto/public_key.h>
#include <crypto/internal/rsa.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/oid_registry.h>
#include <keys/asymmetric-subtype.h>
#include <keys/asymmetric-type.h>
#include <keys/system_keyring.h>
#include <crypto/public_key.h>


#define AES_KEY_SIZE 32 // 256 bits
#define DATA_SIZE 16    // AES block size

char *key = "0123456789abcdef0123456789abcdef"; // 32-byte key
char *origin = "hello";          // 16-byte data
struct crypto_skcipher *skcipher;
struct skcipher_request *req;
struct scatterlist sg_in, sg_out;

static int test_aes(void)
{
    int ret = 0;
    struct crypto_skcipher *tfm;
    struct skcipher_request *req_local;
    char *plaintext;
    char *ciphertext;
    char *decrypted;

    // Allocate transform
    tfm = crypto_alloc_skcipher("ecb(aes)", 0, 0);
    if (IS_ERR(tfm)) {
        pr_err("Failed to allocate cipher\n");
        return PTR_ERR(tfm);
    }

    // Set key
    ret = crypto_skcipher_setkey(tfm, key, AES_KEY_SIZE);
    if (ret) {
        pr_err("Failed to set key: %d\n", ret);
        goto out_free;
    }

    // Allocate request
    req_local = skcipher_request_alloc(tfm, GFP_KERNEL);
    if (!req_local) {
        pr_err("Failed to allocate request\n");
        ret = -ENOMEM;
        goto out_free;
    }

    plaintext = kzalloc(DATA_SIZE, GFP_KERNEL);
    if (!plaintext) {
        pr_err("alloc plaintext fail!\n");
        ret = -ENOMEM;
        goto out_free;
    }
    memcpy(plaintext, origin, DATA_SIZE);

    ciphertext = kzalloc(DATA_SIZE, GFP_KERNEL);
    if (!ciphertext) {
        pr_err("alloc ciphertext fail!\n");
        ret = -ENOMEM;
        goto out_free;
    }
    decrypted = kzalloc(DATA_SIZE, GFP_KERNEL);
    if (!decrypted) {
        pr_err("alloc decrypted fail!\n");
        ret = -ENOMEM;
        goto out_free;
    }

    print_hex_dump(KERN_INFO, "plaintext: ", DUMP_PREFIX_NONE, 16, 1, plaintext, DATA_SIZE, true);
    // Set up scatterlists
    sg_init_one(&sg_in, plaintext, DATA_SIZE);
    sg_init_one(&sg_out, ciphertext, DATA_SIZE);

    // Perform encryption
    skcipher_request_set_callback(req_local, 0, NULL, NULL);
    skcipher_request_set_crypt(req_local, &sg_in, &sg_out, DATA_SIZE, NULL);
    ret = crypto_skcipher_encrypt(req_local);
    if (ret) {
        pr_err("Encryption failed: %d\n", ret);
        goto out_req;
    }

    pr_info("Encryption successful\n");
    print_hex_dump(KERN_INFO, "ciphertext: ", DUMP_PREFIX_NONE, 16, 1, ciphertext, DATA_SIZE, true);

    // Now decrypt
    sg_init_one(&sg_in, ciphertext, DATA_SIZE);
    sg_init_one(&sg_out, decrypted, DATA_SIZE);
    skcipher_request_set_crypt(req_local, &sg_in, &sg_out, DATA_SIZE, NULL);
    ret = crypto_skcipher_decrypt(req_local);
    if (ret) {
        pr_err("Decryption failed: %d\n", ret);
        goto out_req;
    }

    pr_info("Decryption successful\n");
    print_hex_dump(KERN_INFO, "Decrypted: ", DUMP_PREFIX_NONE, 16, 1, decrypted, DATA_SIZE, true);

    // Verify
    if (memcmp(plaintext, decrypted, DATA_SIZE)) {
        pr_err("Decrypted data doesn't match original!\n");
        ret = -EINVAL;
    } else {
        pr_info("Verification successful\n");
    }

out_req:
    skcipher_request_free(req_local);
out_free:
    crypto_free_skcipher(tfm);
    return ret;
}

static int __init aes_init(void)
{
    pr_info("AES-256 ECB module loaded\n");
    return test_aes();
}

static void __exit aes_exit(void)
{
    pr_info("AES-256 ECB module unloaded\n");
}

module_init(aes_init);
module_exit(aes_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kdev");
MODULE_DESCRIPTION("AES-256 ECB Crypto Module");
MODULE_VERSION("1.0");
