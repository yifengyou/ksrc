#include <linux/module.h>
#include <linux/init.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <crypto/aead.h>

#define AES_KEY_SIZE 32     // 256 bits
#define DATA_SIZE 4096      // 4K data
#define IV_SIZE 12          // GCM推荐使用12字节IV
#define AUTH_SIZE 16        // 认证标签长度(16字节)

static char key[AES_KEY_SIZE] = "0123456789abcdef0123456789abcdef"; // 32-byte key
// 固定IV值 (示例值，实际应用中应根据安全要求选择)
static u8 iv[IV_SIZE] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                        0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
static u8 *plaintext;
static u8 *ciphertext;
static u8 *decrypted;
static u8 auth_tag[AUTH_SIZE]; // 认证标签

struct crypto_aead *tfm;
struct aead_request *req;

static int aes_gcm_encrypt(void)
{
    int ret = 0;
    struct scatterlist sg_in, sg_out;
    u8 saved_iv[IV_SIZE];

    // 使用固定IV值，不再随机生成
    memcpy(saved_iv, iv, IV_SIZE);

    // Allocate buffers (额外空间存放认证标签)
    plaintext = kzalloc(DATA_SIZE, GFP_KERNEL);
    if (!plaintext) {
        pr_err("Failed to allocate plaintext\n");
        return -ENOMEM;
    }

    ciphertext = kzalloc(DATA_SIZE + AUTH_SIZE, GFP_KERNEL);
    if (!ciphertext) {
        pr_err("Failed to allocate ciphertext\n");
        ret = -ENOMEM;
        goto out_plain;
    }

    // Generate random data
    get_random_bytes(plaintext, DATA_SIZE);
    pr_info("Generated %d bytes of random data\n", DATA_SIZE);
    print_hex_dump(KERN_DEBUG, "plaintext (first 128 bytes): ",
                  DUMP_PREFIX_NONE, 16, 1, plaintext, 128, true);

    // 打印固定IV值
    print_hex_dump(KERN_DEBUG, "Using fixed IV: ",
                  DUMP_PREFIX_NONE, 16, 1, iv, IV_SIZE, true);

    // Set up scatterlists for encryption
    sg_init_one(&sg_in, plaintext, DATA_SIZE);
    sg_init_one(&sg_out, ciphertext, DATA_SIZE + AUTH_SIZE);

    // Prepare encryption request
    aead_request_set_callback(req, 0, NULL, NULL);
    aead_request_set_crypt(req, &sg_in, &sg_out, DATA_SIZE, iv);
    aead_request_set_ad(req, 0); // 无附加认证数据(AAD)

    // Perform encryption
    ret = crypto_aead_encrypt(req);
    if (ret) {
        pr_err("Encryption failed: %d\n", ret);
        goto out_cipher;
    }

    // 保存认证标签(位于密文末尾)
    memcpy(auth_tag, ciphertext + DATA_SIZE, AUTH_SIZE);

    pr_info("Encryption successful\n");
    print_hex_dump(KERN_DEBUG, "ciphertext (first 128 bytes): ",
                  DUMP_PREFIX_NONE, 16, 1, ciphertext, 128, true);
    print_hex_dump(KERN_DEBUG, "auth tag: ",
                  DUMP_PREFIX_NONE, 16, 1, auth_tag, AUTH_SIZE, true);

    return 0;

out_cipher:
    kfree(ciphertext);
out_plain:
    kfree(plaintext);
    return ret;
}

static int aes_gcm_decrypt(void)
{
    int ret = 0;
    struct scatterlist sg_in, sg_out;

    decrypted = kzalloc(DATA_SIZE, GFP_KERNEL);
    if (!decrypted) {
        pr_err("Failed to allocate decrypted buffer\n");
        return -ENOMEM;
    }

    // 打印固定IV值
    print_hex_dump(KERN_DEBUG, "Using fixed IV for decryption: ",
                  DUMP_PREFIX_NONE, 16, 1, iv, IV_SIZE, true);

    // Set up scatterlists for decryption
    sg_init_one(&sg_in, ciphertext, DATA_SIZE + AUTH_SIZE);
    sg_init_one(&sg_out, decrypted, DATA_SIZE);

    // Prepare decryption request
    aead_request_set_callback(req, 0, NULL, NULL);
    aead_request_set_crypt(req, &sg_in, &sg_out, DATA_SIZE + AUTH_SIZE, iv);
    aead_request_set_ad(req, 0); // 无附加认证数据(AAD)

    // Now decrypt
    ret = crypto_aead_decrypt(req);
    if (ret == -EBADMSG) {
        pr_err("Authentication failed - data may be tampered\n");
        goto out;
    } else if (ret) {
        pr_err("Decryption failed: %d\n", ret);
        goto out;
    }

    pr_info("Decryption successful\n");
    print_hex_dump(KERN_DEBUG, "decrypted (first 128 bytes): ",
                  DUMP_PREFIX_NONE, 16, 1, decrypted, 128, true);

    // Verify (GCM已经验证了认证标签，这里可以双重检查)
    if (memcmp(plaintext, decrypted, DATA_SIZE)) {
        pr_err("Decrypted data doesn't match original!\n");
        ret = -EINVAL;
    } else {
        pr_info("Verification successful - all %d bytes match\n", DATA_SIZE);
    }

out:
    kfree(decrypted);
    return ret;
}

static void cleanup_buffers(void)
{
    if (plaintext) kfree(plaintext);
    if (ciphertext) kfree(ciphertext);
    if (decrypted) kfree(decrypted);
}

static int test_aes_gcm(void)
{
    int ret = 0;

    // Allocate transform
    tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
    if (IS_ERR(tfm)) {
        pr_err("Failed to allocate cipher\n");
        return PTR_ERR(tfm);
    }

    // Set key
    ret = crypto_aead_setkey(tfm, key, AES_KEY_SIZE);
    if (ret) {
        pr_err("Failed to set key: %d\n", ret);
        goto out_free;
    }

    // 设置认证标签长度
    ret = crypto_aead_setauthsize(tfm, AUTH_SIZE);
    if (ret) {
        pr_err("Failed to set auth size: %d\n", ret);
        goto out_free;
    }

    // Allocate request
    req = aead_request_alloc(tfm, GFP_KERNEL);
    if (!req) {
        pr_err("Failed to allocate request\n");
        ret = -ENOMEM;
        goto out_free;
    }

    // Perform encryption
    ret = aes_gcm_encrypt();
    if (ret) {
        pr_err("Encryption test failed\n");
        goto out_req;
    }

    // Tamper with ciphertext to test authentication
    ciphertext[0] ^= 0x01; // Flip one bit

    // Perform decryption
    ret = aes_gcm_decrypt();
    if (!ret) {
        pr_err("Authentication should have failed but didn't!\n");
        ret = -EINVAL;
    } else if (ret == -EBADMSG) {
        pr_info("Authentication correctly detected tampered data\n");
        ret = 0; // This was expected
    }

    // Restore original ciphertext and try again
    ciphertext[0] ^= 0x01;

    // Perform decryption again
    ret = aes_gcm_decrypt();
    if (ret) {
        pr_err("Decryption failed after restoring ciphertext\n");
    }

out_req:
    aead_request_free(req);
out_free:
    crypto_free_aead(tfm);
    cleanup_buffers();
    return ret;
}

static int __init aes_init(void)
{
    pr_info("AES-256 GCM module loaded\n");
    pr_info("Using fixed IV for GCM mode\n");
    return test_aes_gcm();
}

static void __exit aes_exit(void)
{
    pr_info("AES-256 GCM module unloaded\n");
}

module_init(aes_init);
module_exit(aes_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kdev");
MODULE_DESCRIPTION("AES-256 GCM Crypto Module with Fixed IV");
MODULE_VERSION("1.0");

