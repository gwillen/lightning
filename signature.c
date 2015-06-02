#include "signature.h"
#include "shadouble.h"
#include "bitcoin_tx.h"
#include "pubkey.h"
#include "bitcoin_script.h"
#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#include <assert.h>
#include <ccan/cast/cast.h>

struct signature *sign_hash(const tal_t *ctx, EC_KEY *private_key,
			    const struct sha256_double *h)
{
	ECDSA_SIG *sig;
	int len;
	struct signature *s;
	
	sig = ECDSA_do_sign(h->sha.u.u8, sizeof(*h), private_key);
	if (!sig)
		return NULL;

	/* See https://github.com/sipa/bitcoin/commit/a81cd9680.
	 * There can only be one signature with an even S, so make sure we
	 * get that one. */
	if (BN_is_odd(sig->s)) {
		const EC_GROUP *group;
		BIGNUM order;

		BN_init(&order);
		group = EC_KEY_get0_group(private_key);
		EC_GROUP_get_order(group, &order, NULL);
		BN_sub(sig->s, &order, sig->s);
		BN_free(&order);

		assert(!BN_is_odd(sig->s));
        }

	s = talz(ctx, struct signature);

	/* Pack r and s into signature, 32 bytes each. */
	len = BN_num_bytes(sig->r);
	assert(len <= sizeof(s->r));
	BN_bn2bin(sig->r, s->r + sizeof(s->r) - len);
	len = BN_num_bytes(sig->s);
	assert(len <= sizeof(s->s));
	BN_bn2bin(sig->s, s->s + sizeof(s->s) - len);

	ECDSA_SIG_free(sig);
	return s;
}

/* Only does SIGHASH_ALL */
static void sha256_tx_one_input(struct bitcoin_tx *tx,
				size_t input_num,
				const u8 *script, size_t script_len,
				struct sha256_double *hash)
{
	struct sha256_ctx ctx = SHA256_INIT;
	size_t i;

	assert(input_num < tx->input_count);

	/* You must have all inputs zeroed to start. */
	for (i = 0; i < tx->input_count; i++)
		assert(tx->input[i].script_length == 0);

	tx->input[input_num].script_length = script_len;
	tx->input[input_num].script = cast_const(u8 *, script);

	sha256_init(&ctx);
	sha256_tx(&ctx, tx);
	sha256_le32(&ctx, SIGHASH_ALL);
	sha256_double_done(&ctx, hash);

	/* Reset it for next time. */
	tx->input[input_num].script_length = 0;
	tx->input[input_num].script = NULL;
}
	
struct signature *sign_tx_input(const tal_t *ctx, struct bitcoin_tx *tx,
				unsigned int in,
				const u8 *subscript, size_t subscript_len,
				EC_KEY *privkey)
{
	struct sha256_double hash;

	sha256_tx_one_input(tx, in, subscript, subscript_len, &hash);
	return sign_hash(ctx, privkey, &hash);
}

static bool check_signed_hash(const struct sha256_double *hash,
			      const struct signature *signature,
			      const struct pubkey *key)
{
	bool ok = false;	
	BIGNUM r, s;
	ECDSA_SIG sig = { &r, &s };
	EC_KEY *eckey = EC_KEY_new_by_curve_name(NID_secp256k1);
	const unsigned char *k = key->key;

	/* S must be even: https://github.com/sipa/bitcoin/commit/a81cd9680 */
	assert((signature->s[31] & 1) == 0);

	/* Unpack public key. */
	if (!o2i_ECPublicKey(&eckey, &k, pubkey_len(key)))
		goto out;

	/* Unpack signature. */
	BN_init(&r);
	BN_init(&s);
	if (!BN_bin2bn(signature->r, sizeof(signature->r), &r)
	    || !BN_bin2bn(signature->s, sizeof(signature->s), &s))
		goto free_bns;

	/* Now verify hash with public key and signature. */
	switch (ECDSA_do_verify(hash->sha.u.u8, sizeof(hash->sha.u), &sig,
				eckey)) {
	case 0:
		/* Invalid signature */
		goto free_bns;
	case -1:
		/* Malformed or other error. */
		goto free_bns;
	}

	ok = true;

free_bns:
	BN_free(&r);
	BN_free(&s);

out:
	EC_KEY_free(eckey);
        return ok;
}

bool check_2of2_sig(struct bitcoin_tx *tx, size_t input_num,
		    const struct bitcoin_tx_output *output,
		    const struct pubkey *key1, const struct pubkey *key2,
		    const struct signature *sig1, const struct signature *sig2)
{
	struct sha256_double hash;
	assert(input_num < tx->input_count);

	assert(is_p2sh(output->script, output->script_length));
	sha256_tx_one_input(tx, input_num,
			    output->script, output->script_length, &hash);

	return check_signed_hash(&hash, sig1, key1)
		&& check_signed_hash(&hash, sig2, key2);
}

Signature *signature_to_proto(const tal_t *ctx, const struct signature *sig)
{
	Signature *pb = tal(ctx, Signature);
	signature__init(pb);

	assert((sig->s[31] & 1) == 0);

	/* Kill me now... */
	memcpy(&pb->r1, sig->r, 8);
	memcpy(&pb->r2, sig->r + 8, 8);
	memcpy(&pb->r3, sig->r + 16, 8);
	memcpy(&pb->r4, sig->r + 24, 8);
	memcpy(&pb->s1, sig->s, 8);
	memcpy(&pb->s2, sig->s + 8, 8);
	memcpy(&pb->s3, sig->s + 16, 8);
	memcpy(&pb->s4, sig->s + 24, 8);

	return pb;
}

bool proto_to_signature(const Signature *pb, struct signature *sig)
{
	/* Kill me again. */
	memcpy(sig->r, &pb->r1, 8);
	memcpy(sig->r + 8, &pb->r2, 8);
	memcpy(sig->r + 16, &pb->r3, 8);
	memcpy(sig->r + 24, &pb->r4, 8);
	memcpy(sig->s, &pb->s1, 8);
	memcpy(sig->s + 8, &pb->s2, 8);
	memcpy(sig->s + 16, &pb->s3, 8);
	memcpy(sig->s + 24, &pb->s4, 8);

	/* S must be even */
	return (sig->s[31] & 1) == 0;
}
