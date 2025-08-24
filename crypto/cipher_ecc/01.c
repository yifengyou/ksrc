#include <linux/module.h>
#include <crypto/ecc_curve.h>
#include <crypto/internal/ecc.h>

static int __init ecc_keygen_init(void) {
	u64 priv[ECC_CURVE_NIST_P256_DIGITS] = {0};
	struct ecc_point pub = {0};

	// 生成随机私钥
	get_random_bytes(priv, sizeof(priv));

	// 计算公钥（需实现 ecc_point_mult）
	ecc_point_mult(&pub, &ecc_curve_nist_p256.g, priv, &ecc_curve_nist_p256);

	printk("Private Key: %llx...\n", priv[0]);
	printk("Public Key: (%llx, %llx)\n", pub.x[0], pub.y[0]);
	return 0;
}

module_init(ecc_keygen_init);
MODULE_LICENSE("GPL");
