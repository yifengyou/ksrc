#include <linux/module.h>
#include <linux/init.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <crypto/skcipher.h>
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
static u8 iv[AES_BLOCK_SIZE]; // Initialization Vector for CBC mode
static u8 *plaintext;
static u8 *ciphertext;
static u8 *decrypted;

static int test_aes(void)
{
	int ret = 0;
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
	struct scatterlist sg_in, sg_out;
	u8 saved_iv[AES_BLOCK_SIZE]; // Save IV for decryption

	// Allocate transform - changed to "cbc(aes)"
	tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
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

	// Generate random IV for CBC mode and save it
	get_random_bytes(iv, AES_BLOCK_SIZE);
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

	decrypted = kzalloc(DATA_SIZE, GFP_KERNEL);
	if (!decrypted) {
		pr_err("Failed to allocate decrypted buffer\n");
		ret = -ENOMEM;
		goto out_cipher;
	}

	// Generate random data (must be multiple of block size for CBC)
	get_random_bytes(plaintext, DATA_SIZE-4090);
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
		goto out_decrypted;
	}

	pr_info("Encryption successful\n");
	print_hex_dump(KERN_DEBUG, "ciphertext (first 128 bytes): ",
			DUMP_PREFIX_NONE, 16, 1, ciphertext, 128, true);

	//ciphertext[0] = 'Y';

	// Reset IV for decryption (must use same IV as encryption)
	memcpy(iv, saved_iv, AES_BLOCK_SIZE);

	// Now decrypt
	sg_init_one(&sg_in, ciphertext, DATA_SIZE);
	sg_init_one(&sg_out, decrypted, DATA_SIZE);
	skcipher_request_set_crypt(req, &sg_in, &sg_out, DATA_SIZE, iv);
	ret = crypto_skcipher_decrypt(req);
	if (ret) {
		pr_err("Decryption failed: %d\n", ret);
		goto out_decrypted;
	}

	pr_info("Decryption successful\n");
	print_hex_dump(KERN_DEBUG, "decrypted (first 128 bytes): ",
			DUMP_PREFIX_NONE, 16, 1, decrypted, 128, true);

	// Verify
	if (memcmp(plaintext, decrypted, DATA_SIZE)) {
		pr_err("Decrypted data doesn't match original!\n");
		ret = -EINVAL;
	} else {
		pr_info("Verification successful - all %d bytes match\n", DATA_SIZE);
	}

out_decrypted:
	kfree(decrypted);
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
	pr_info("AES-256 CBC module loaded\n");
	return test_aes();
}

static void __exit aes_exit(void)
{
	pr_info("AES-256 CBC module unloaded\n");
}

module_init(aes_init);
module_exit(aes_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kdev");
MODULE_DESCRIPTION("AES-256 CBC Crypto Module with 4K Random Data");
MODULE_VERSION("1.0");
