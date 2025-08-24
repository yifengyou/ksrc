#include <linux/module.h>
#include <linux/random.h>
#include <crypto/ecdh.h>

static int __init ecc_keygen_init(void)
{
	struct ecdh params = {
		.curve_id = ECC_CURVE_NIST_P256,
	};
	u8 private_key[32];
	u8 public_key[64];
	int ret;

	/* 生成私钥 */
	ret = crypto_ecdh_gen_privkey(params.curve_id, private_key, sizeof(private_key));
	if (ret)
		return ret;

	/* 计算公钥 */
	ret = crypto_ecdh_gen_pubkey(params.curve_id, private_key, sizeof(private_key),
			public_key, sizeof(public_key));
	if (ret)
		return ret;

	print_hex_dump(KERN_INFO, "Private key: ", DUMP_PREFIX_NONE, 16, 1, 
			private_key, sizeof(private_key), false);
	print_hex_dump(KERN_INFO, "Public key: ", DUMP_PREFIX_NONE, 16, 1,
			public_key, sizeof(public_key), false);

	return 0;
}

module_init(ecc_keygen_init);
MODULE_LICENSE("GPL");
