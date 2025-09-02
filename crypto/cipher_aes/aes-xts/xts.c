#include <linux/module.h>
#include <linux/init.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <crypto/skcipher.h>
#include <crypto/xts.h>
#include <linux/types.h>

#define AES_KEY_SIZE 32 // 256 bits
#define XTS_KEY_SIZE 64 // 512 bits (two 256-bit keys for XTS)
#define DATA_SIZE 4096  // 4K data
#define AES_BLOCK_SIZE 16 // AES block size

static char xts_key[XTS_KEY_SIZE] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"; // 64-byte key
static u64 sector_number = 0; // Starting sector number
static u8 *plaintext;
static u8 *ciphertext;

/*
 * In-place decryption function for XTS mode
 * Parameters:
 *   ciphertext_ptr - pointer to ciphertext data (will be overwritten with plaintext)
 *   len - length of data to decrypt (must be multiple of AES block size)
 *   sector - sector number for tweak calculation
 */
static int decrypt_inplace(u8 *ciphertext_ptr, unsigned int len, u64 sector)
{
	int ret = 0;
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
	struct scatterlist sg;
	u8 tweak[AES_BLOCK_SIZE] = {0};

	if (!ciphertext_ptr || len == 0 || len % AES_BLOCK_SIZE != 0) {
		pr_err("Invalid parameters to decrypt_inplace\n");
		return -EINVAL;
	}

	// Allocate transform
	tfm = crypto_alloc_skcipher("xts(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("Failed to allocate cipher\n");
		return PTR_ERR(tfm);
	}

	// Set key
	ret = crypto_skcipher_setkey(tfm, xts_key, XTS_KEY_SIZE);
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

	// Prepare tweak: sector number in little endian format
	memcpy(tweak, &sector, sizeof(sector));

	// Set up scatterlist for in-place operation
	sg_init_one(&sg, ciphertext_ptr, len);

	// Perform decryption
	skcipher_request_set_callback(req, 0, NULL, NULL);
	skcipher_request_set_crypt(req, &sg, &sg, len, tweak);
	ret = crypto_skcipher_decrypt(req);
	if (ret) {
		pr_err("Decryption failed: %d\n", ret);
	}

	skcipher_request_free(req);
out_free:
	crypto_free_skcipher(tfm);
	return ret;
}

/*
 * Encryption function for XTS mode
 * Parameters:
 *   plaintext_ptr - pointer to plaintext data
 *   ciphertext_ptr - pointer to ciphertext buffer
 *   len - length of data to encrypt (must be multiple of AES block size)
 *   sector - sector number for tweak calculation
 */
static int encrypt_data(u8 *plaintext_ptr, u8 *ciphertext_ptr, unsigned int len, u64 sector)
{
	int ret = 0;
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
	struct scatterlist sg_in, sg_out;
	u8 tweak[AES_BLOCK_SIZE] = {0};

	if (!plaintext_ptr || !ciphertext_ptr || len == 0 || len % AES_BLOCK_SIZE != 0) {
		pr_err("Invalid parameters to encrypt_data\n");
		return -EINVAL;
	}

	// Allocate transform
	tfm = crypto_alloc_skcipher("xts(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("Failed to allocate cipher\n");
		return PTR_ERR(tfm);
	}

	// Set key
	ret = crypto_skcipher_setkey(tfm, xts_key, XTS_KEY_SIZE);
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

	// Prepare tweak: sector number in little endian format
	memcpy(tweak, &sector, sizeof(sector));

	// Set up scatterlists
	sg_init_one(&sg_in, plaintext_ptr, len);
	sg_init_one(&sg_out, ciphertext_ptr, len);

	// Perform encryption
	skcipher_request_set_callback(req, 0, NULL, NULL);
	skcipher_request_set_crypt(req, &sg_in, &sg_out, len, tweak);
	ret = crypto_skcipher_encrypt(req);
	if (ret) {
		pr_err("Encryption failed: %d\n", ret);
	}

	skcipher_request_free(req);
out_free:
	crypto_free_skcipher(tfm);
	return ret;
}

static int test_aes_xts(void)
{
	int ret = 0;

	// Allocate buffers
	plaintext = kzalloc(DATA_SIZE, GFP_KERNEL);
	if (!plaintext) {
		pr_err("Failed to allocate plaintext\n");
		return -ENOMEM;
	}

	ciphertext = kzalloc(DATA_SIZE, GFP_KERNEL);
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

	// Perform encryption
	ret = encrypt_data(plaintext, ciphertext, DATA_SIZE, sector_number);
	if (ret) {
		pr_err("Encryption failed: %d\n", ret);
		goto out_cipher;
	}

	pr_info("Encryption successful\n");
	print_hex_dump(KERN_DEBUG, "ciphertext (first 128 bytes): ",
			DUMP_PREFIX_NONE, 16, 1, ciphertext, 128, true);

	// Save ciphertext for comparison
	u8 *original_ciphertext = kzalloc(DATA_SIZE, GFP_KERNEL);
	if (!original_ciphertext) {
		pr_err("Failed to allocate original ciphertext buffer\n");
		ret = -ENOMEM;
		goto out_cipher;
	}
	memcpy(original_ciphertext, ciphertext, DATA_SIZE);

	// Now decrypt in-place using our new function
	ret = decrypt_inplace(ciphertext, DATA_SIZE, sector_number);
	if (ret) {
		pr_err("In-place decryption failed: %d\n", ret);
		goto out_original;
	}

	pr_info("In-place decryption successful\n");
	print_hex_dump(KERN_DEBUG, "decrypted (first 128 bytes): ",
			DUMP_PREFIX_NONE, 16, 1, ciphertext, 128, true);

	// Verify
	if (memcmp(plaintext, ciphertext, DATA_SIZE)) {
		pr_err("Decrypted data doesn't match original!\n");
		print_hex_dump(KERN_ERR, "expected (first 128 bytes): ",
				DUMP_PREFIX_NONE, 16, 1, plaintext, 128, true);
		ret = -EINVAL;
	} else {
		pr_info("Verification successful - all %d bytes match\n", DATA_SIZE);
	}

out_original:
	kfree(original_ciphertext);
out_cipher:
	kfree(ciphertext);
out_plain:
	kfree(plaintext);
	return ret;
}

static int __init aes_init(void)
{
	pr_info("AES-256 XTS module loaded\n");
	return test_aes_xts();
}

static void __exit aes_exit(void)
{
	pr_info("AES-256 XTS module unloaded\n");
}

module_init(aes_init);
module_exit(aes_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kdev");
MODULE_DESCRIPTION("AES-256 XTS Crypto Module for Disk Encryption");
MODULE_VERSION("1.0");

