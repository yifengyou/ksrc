#include <linux/module.h>
#include <linux/init.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <crypto/skcipher.h>

#define AES_KEY_SIZE 32 // 256 bits
#define DATA_SIZE 4096  // 4K data
#define AES_BLOCK_SIZE 16 // AES block size (for IV)

static char key[AES_KEY_SIZE] = "0123456789abcdef0123456789abcdef"; // 32-byte key
static u8 iv[AES_BLOCK_SIZE] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
static u8 *plaintext;
static u8 *ciphertext;

/*
 * In-place decryption function
 * Parameters:
 *   ciphertext_ptr - pointer to ciphertext data (will be overwritten with plaintext)
 *   len - length of data to decrypt
 *   iv_ptr - initialization vector (must be same as used for encryption)
 */
static int decrypt_inplace(u8 *ciphertext_ptr, unsigned int len, const u8 *iv_ptr)
{
	int ret = 0;
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
	struct scatterlist sg;
	u8 local_iv[AES_BLOCK_SIZE];

	if (!ciphertext_ptr || len == 0 || !iv_ptr) {
		pr_err("Invalid parameters to decrypt_inplace\n");
		return -EINVAL;
	}

	// Allocate transform
	tfm = crypto_alloc_skcipher("ctr(aes)", 0, 0);
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
	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("Failed to allocate request\n");
		ret = -ENOMEM;
		goto out_free;
	}

	// Copy IV to local buffer (we don't want to modify caller's IV)
	memcpy(local_iv, iv_ptr, AES_BLOCK_SIZE);

	// Set up scatterlist for in-place operation
	sg_init_one(&sg, ciphertext_ptr, len);

	// Perform decryption (which is same as encryption in CTR mode)
	skcipher_request_set_callback(req, 0, NULL, NULL);
	skcipher_request_set_crypt(req, &sg, &sg, len, local_iv);
	ret = crypto_skcipher_decrypt(req);
	if (ret) {
		pr_err("Decryption failed: %d\n", ret);
	}

	skcipher_request_free(req);
out_free:
	crypto_free_skcipher(tfm);
	return ret;
}

static int test_aes(void)
{
	int ret = 0;
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
	struct scatterlist sg_in, sg_out;
	u8 saved_iv[AES_BLOCK_SIZE]; // Save encryption IV for later decryption

	// Allocate transform
	tfm = crypto_alloc_skcipher("ctr(aes)", 0, 0);
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
	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("Failed to allocate request\n");
		ret = -ENOMEM;
		goto out_free;
	}

	// Generate random IV for CTR mode and save it
	//get_random_bytes(iv, AES_BLOCK_SIZE);
	memcpy(saved_iv, iv, AES_BLOCK_SIZE);

	// Allocate buffers
	plaintext = kzalloc(DATA_SIZE, GFP_KERNEL);
	if (!plaintext) {
		pr_err("Failed to allocate plaintext\n");
		ret = -ENOMEM;
		goto out_req;
	}

	ciphertext = kzalloc(DATA_SIZE, GFP_KERNEL);
	if (!ciphertext) {
		pr_err("Failed to allocate ciphertext\n");
		ret = -ENOMEM;
		goto out_plain;
	}

	// Generate random data
	get_random_bytes(plaintext, DATA_SIZE);
	//plaintext[0] = 'Y';

	pr_info("Generated %d bytes of random data\n", DATA_SIZE);
	print_hex_dump(KERN_DEBUG, "plaintext (first 128 bytes): ",
			DUMP_PREFIX_NONE, 16, 1, plaintext, 128, true);

	// Set up scatterlists
	sg_init_one(&sg_in, plaintext, DATA_SIZE);
	sg_init_one(&sg_out, ciphertext, DATA_SIZE);

	// Perform encryption
	skcipher_request_set_callback(req, 0, NULL, NULL);
	skcipher_request_set_crypt(req, &sg_in, &sg_out, DATA_SIZE, iv);
	ret = crypto_skcipher_encrypt(req);
	if (ret) {
		pr_err("Encryption failed: %d\n", ret);
		goto out_cipher;
	}

	pr_info("Encryption successful\n");
	print_hex_dump(KERN_DEBUG, "ciphertext (first 128 bytes): ",
			DUMP_PREFIX_NONE, 16, 1, ciphertext, 128, true);

	// Now decrypt in-place using our new function
	ret = decrypt_inplace(ciphertext, DATA_SIZE, saved_iv);
	if (ret) {
		pr_err("In-place decryption failed: %d\n", ret);
		goto out_cipher;
	}

	pr_info("In-place decryption successful\n");
	print_hex_dump(KERN_DEBUG, "decrypted (first 128 bytes): ",
			DUMP_PREFIX_NONE, 16, 1, ciphertext, 128, true);

	// Verify
	if (memcmp(plaintext, ciphertext, DATA_SIZE)) {
		pr_err("Decrypted data doesn't match original!\n");
		ret = -EINVAL;
	} else {
		pr_info("Verification successful - all %d bytes match\n", DATA_SIZE);
	}

out_cipher:
	kfree(ciphertext);
out_plain:
	kfree(plaintext);
out_req:
	skcipher_request_free(req);
out_free:
	crypto_free_skcipher(tfm);
	return ret;
}

static int __init aes_init(void)
{
	pr_info("AES-256 CTR module loaded\n");
	return test_aes();
}

static void __exit aes_exit(void)
{
	pr_info("AES-256 CTR module unloaded\n");
}

module_init(aes_init);
module_exit(aes_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kdev");
MODULE_DESCRIPTION("AES-256 CTR Crypto Module with In-Place Decryption");
MODULE_VERSION("1.0");

