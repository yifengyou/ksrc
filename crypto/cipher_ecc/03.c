#include <linux/module.h>
#include <linux/init.h>
#include <linux/random.h>
#include <crypto/internal/ecc.h>
#include <crypto/ecdh.h>

#define ECC_CURVE_NIST_P256_BYTES 32

static void print_ecc_key(const char *name, const u64 *key, unsigned int ndigits)
{
	u8 buf[ECC_CURVE_NIST_P256_BYTES];
	unsigned int i, pos = 0;

	printk(KERN_INFO "%s: ", name);
	for (i = 0; i < ndigits; i++) {
		pos += snprintf(buf + pos, sizeof(buf) - pos, "%016llx", key[i]);
	}
	printk("%s\n", buf);
}

static int __init ecc_keygen_init(void)
{
	const struct ecc_curve *curve = ecc_get_curve(ECC_CURVE_NIST_P256);
	struct ecc_point *pub_key;
	u64 priv_key[ECC_MAX_DIGITS] = {0};
	int ret;

	if (!curve) {
		printk(KERN_ERR "Failed to get ECC curve\n");
		return -ENODEV;
	}

	/* 分配公钥结构 */
	pub_key = ecc_alloc_point(curve->g.ndigits);
	if (!pub_key) {
		printk(KERN_ERR "Failed to allocate ECC point\n");
		return -ENOMEM;
	}

	/* 生成随机私钥 */
	get_random_bytes(priv_key, curve->g.ndigits * sizeof(u64));

	/* 确保私钥在有效范围内 (0 < priv_key < n) */
	if (vli_is_zero(priv_key, curve->g.ndigits)) {
		printk(KERN_WARNING "Generated zero private key, retrying...\n");
		get_random_bytes(priv_key, curve->g.ndigits * sizeof(u64));
	}
	vli_mod(priv_key, priv_key, curve->n, curve->g.ndigits);

	/* 计算公钥 Q = d * G */
	ecc_point_mult(pub_key, &curve->g, priv_key, curve);

	/* 打印密钥 */
	print_ecc_key("Private key", priv_key, curve->g.ndigits);
	print_ecc_key("Public key X", pub_key->x, curve->g.ndigits);
	print_ecc_key("Public key Y", pub_key->y, curve->g.ndigits);

	/* 验证公钥是否在曲线上 */
	ret = ecc_is_pubkey_valid_full(curve, pub_key);
	printk(KERN_INFO "Public key validation: %s\n", ret ? "Valid" : "Invalid");

	ecc_free_point(pub_key);
	return 0;
}

static void __exit ecc_keygen_exit(void)
{
	printk(KERN_INFO "ECC keygen module unloaded\n");
}

module_init(ecc_keygen_init);
module_exit(ecc_keygen_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("ECC Key Generation using Linux Kernel Crypto API");
