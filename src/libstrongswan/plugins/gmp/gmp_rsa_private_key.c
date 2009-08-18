/*
 * Copyright (C) 2005-2008 Martin Willi
 * Copyright (C) 2005 Jan Hutter
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <gmp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "gmp_rsa_private_key.h"
#include "gmp_rsa_public_key.h"

#include <debug.h>
#include <asn1/oid.h>
#include <asn1/asn1.h>
#include <asn1/asn1_parser.h>

/**
 *  Public exponent to use for key generation.
 */
#define PUBLIC_EXPONENT 0x10001

typedef struct private_gmp_rsa_private_key_t private_gmp_rsa_private_key_t;

/**
 * Private data of a gmp_rsa_private_key_t object.
 */
struct private_gmp_rsa_private_key_t {
	/**
	 * Public interface for this signer.
	 */
	gmp_rsa_private_key_t public;
	
	/**
	 * Version of key, as encoded in PKCS#1
	 */
	u_int version;
	
	/**
	 * Public modulus.
	 */
	mpz_t n;
	
	/**
	 * Public exponent.
	 */
	mpz_t e;
	
	/**
	 * Private prime 1.
	 */
	mpz_t p;
	
	/**
	 * Private Prime 2.
	 */
	mpz_t q;
	
	/**
	 * Private exponent.
	 */
	mpz_t d;
	
	/**
	 * Private exponent 1.
	 */
	mpz_t exp1;
	
	/**
	 * Private exponent 2.
	 */
	mpz_t exp2;
	
	/**
	 * Private coefficient.
	 */
	mpz_t coeff;
	
	/**
	 * Keysize in bytes.
	 */
	size_t k;

	/**
	 * Keyid formed as a SHA-1 hash of a publicKey object
	 */
	identification_t* keyid;

	/**
	 * Keyid formed as a SHA-1 hash of a publicKeyInfo object
	 */
	identification_t* keyid_info;
	
	/**
	 * reference count
	 */
	refcount_t ref;	
};

/**
 * Shared functions defined in gmp_rsa_public_key.c
 */
extern bool gmp_rsa_public_key_build_id(mpz_t n, mpz_t e, 
										identification_t **keyid,
										identification_t **keyid_info);

/**
 * Auxiliary function overwriting private key material with zero bytes
 */
static void mpz_clear_randomized(mpz_t z)
{
	size_t len = mpz_size(z) * GMP_LIMB_BITS / BITS_PER_BYTE;
	u_int8_t *random = alloca(len);
	
	memset(random, 0, len);
	/* overwrite mpz_t with zero bytes before clearing it */
	mpz_import(z, len, 1, 1, 1, 0, random);
	mpz_clear(z);
}

/**
 * Create a mpz prime of at least prime_size
 */
static status_t compute_prime(private_gmp_rsa_private_key_t *this,
							  size_t prime_size, mpz_t *prime)
{
	rng_t *rng;
	chunk_t random_bytes;
	
	rng = lib->crypto->create_rng(lib->crypto, RNG_TRUE);
	if (!rng)
	{
		DBG1("no RNG of quality %N found", rng_quality_names, RNG_TRUE);
		return FAILED;
	}
	
	mpz_init(*prime);
	do
	{
		rng->allocate_bytes(rng, prime_size, &random_bytes);
		/* make sure most significant bit is set */
		random_bytes.ptr[0] = random_bytes.ptr[0] | 0x80;
		
		mpz_import(*prime, random_bytes.len, 1, 1, 1, 0, random_bytes.ptr);
		mpz_nextprime (*prime, *prime);
		chunk_clear(&random_bytes);
	}
	/* check if it isn't too large */
	while (((mpz_sizeinbase(*prime, 2) + 7) / 8) > prime_size);
	
	rng->destroy(rng);
	return SUCCESS;
}

/**
 * PKCS#1 RSADP function
 */
static chunk_t rsadp(private_gmp_rsa_private_key_t *this, chunk_t data)
{
	mpz_t t1, t2;
	chunk_t decrypted;
	
	mpz_init(t1);
	mpz_init(t2);
	
	mpz_import(t1, data.len, 1, 1, 1, 0, data.ptr);
	
	mpz_powm(t2, t1, this->exp1, this->p);	/* m1 = c^dP mod p */
	mpz_powm(t1, t1, this->exp2, this->q);	/* m2 = c^dQ mod Q */
	mpz_sub(t2, t2, t1);					/* h = qInv (m1 - m2) mod p */
	mpz_mod(t2, t2, this->p);
	mpz_mul(t2, t2, this->coeff);
	mpz_mod(t2, t2, this->p);
	
	mpz_mul(t2, t2, this->q);				/* m = m2 + h q */
	mpz_add(t1, t1, t2);
	
	decrypted.len = this->k;
	decrypted.ptr = mpz_export(NULL, NULL, 1, decrypted.len, 1, 0, t1);
	if (decrypted.ptr == NULL)
	{
		decrypted.len = 0;
	}
	
	mpz_clear_randomized(t1);
	mpz_clear_randomized(t2);
	
	return decrypted;
}

/**
 * PKCS#1 RSASP1 function
 */
static chunk_t rsasp1(private_gmp_rsa_private_key_t *this, chunk_t data)
{
	return rsadp(this, data);
}

/**
 * Implementation of gmp_rsa_private_key_t.build_emsa_pkcs1_signature.
 */
static bool build_emsa_pkcs1_signature(private_gmp_rsa_private_key_t *this,
									   hash_algorithm_t hash_algorithm,
									   chunk_t data, chunk_t *signature)
{
	chunk_t digestInfo = chunk_empty;
	chunk_t em;

	if (hash_algorithm != HASH_UNKNOWN)
	{
		hasher_t *hasher;
		chunk_t hash;
		int hash_oid = hasher_algorithm_to_oid(hash_algorithm);
	
		if (hash_oid == OID_UNKNOWN)
		{
			return FALSE;
		}

		hasher = lib->crypto->create_hasher(lib->crypto, hash_algorithm);
		if (hasher == NULL)
		{
			return FALSE;
		}
		hasher->allocate_hash(hasher, data, &hash);
		hasher->destroy(hasher);
	
		/* build DER-encoded digestInfo */
		digestInfo = asn1_wrap(ASN1_SEQUENCE, "cm",
						asn1_algorithmIdentifier(hash_oid),
						asn1_simple_object(ASN1_OCTET_STRING, hash)
					  );
		chunk_free(&hash);
		data = digestInfo;
	}

	if (data.len > this->k - 3)
	{
		free(digestInfo.ptr);
		DBG1("unable to sign %d bytes using a %dbit key", data.len, this->k * 8);
		return FALSE;
	}
	
	/* build chunk to rsa-decrypt:
	 * EM = 0x00 || 0x01 || PS || 0x00 || T. 
	 * PS = 0xFF padding, with length to fill em
	 * T = encoded_hash
	 */
	em.len = this->k;
	em.ptr = malloc(em.len);
	
	/* fill em with padding */
	memset(em.ptr, 0xFF, em.len);
	/* set magic bytes */
	*(em.ptr) = 0x00;
	*(em.ptr+1) = 0x01;
	*(em.ptr + em.len - data.len - 1) = 0x00;
	/* set DER-encoded hash */
	memcpy(em.ptr + em.len - data.len, data.ptr, data.len);

	/* build signature */
	*signature = rsasp1(this, em);
	
	free(digestInfo.ptr);
	free(em.ptr);
	
	return TRUE;	
}

/**
 * Implementation of gmp_rsa_private_key.get_type.
 */
static key_type_t get_type(private_gmp_rsa_private_key_t *this)
{
	return KEY_RSA;
}

/**
 * Implementation of gmp_rsa_private_key.sign.
 */
static bool sign(private_gmp_rsa_private_key_t *this, signature_scheme_t scheme, 
				 chunk_t data, chunk_t *signature)
{
	switch (scheme)
	{
		case SIGN_RSA_EMSA_PKCS1_NULL:
			return build_emsa_pkcs1_signature(this, HASH_UNKNOWN, data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA1:
			return build_emsa_pkcs1_signature(this, HASH_SHA1, data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA224:
			return build_emsa_pkcs1_signature(this, HASH_SHA224, data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA256:
			return build_emsa_pkcs1_signature(this, HASH_SHA256, data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA384:
			return build_emsa_pkcs1_signature(this, HASH_SHA384, data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA512:
			return build_emsa_pkcs1_signature(this, HASH_SHA512, data, signature);
		case SIGN_RSA_EMSA_PKCS1_MD5:
			return build_emsa_pkcs1_signature(this, HASH_MD5, data, signature);
		default:
			DBG1("signature scheme %N not supported in RSA",
				 signature_scheme_names, scheme);
			return FALSE;
	}
}

/**
 * Implementation of gmp_rsa_private_key.decrypt.
 */
static bool decrypt(private_gmp_rsa_private_key_t *this, chunk_t crypto,
					chunk_t *plain)
{
	chunk_t em, stripped;
	bool success = FALSE;
	
	/* rsa decryption using PKCS#1 RSADP */
	stripped = em = rsadp(this, crypto);

	/* PKCS#1 v1.5 8.1 encryption-block formatting (EB = 00 || 02 || PS || 00 || D) */

	/* check for hex pattern 00 02 in decrypted message */
	if ((*stripped.ptr++ != 0x00) || (*(stripped.ptr++) != 0x02))
	{
		DBG1("incorrect padding - probably wrong rsa key");
		goto end;
	}
	stripped.len -= 2;

	/* the plaintext data starts after first 0x00 byte */
	while (stripped.len-- > 0 && *stripped.ptr++ != 0x00)

	if (stripped.len == 0)
	{
		DBG1("no plaintext data");
		goto end;
	}

	*plain = chunk_clone(stripped);
	success = TRUE;

end:
	chunk_clear(&em);
	return success;
}

/**
 * Implementation of gmp_rsa_private_key.get_keysize.
 */
static size_t get_keysize(private_gmp_rsa_private_key_t *this)
{
	return this->k;
}

/**
 * Implementation of gmp_rsa_private_key.get_id.
 */
static identification_t* get_id(private_gmp_rsa_private_key_t *this,
								id_type_t type)
{
	switch (type)
	{
		case ID_PUBKEY_INFO_SHA1:
			return this->keyid_info;
		case ID_PUBKEY_SHA1:
			return this->keyid;
		default:
			return NULL;
	}
}

/**
 * Convert a MP integer into a chunk_t
 */
chunk_t gmp_mpz_to_chunk(const mpz_t value)
{
	chunk_t n;
	
	n.len = 1 + mpz_sizeinbase(value, 2) / BITS_PER_BYTE;
	n.ptr = mpz_export(NULL, NULL, 1, n.len, 1, 0, value);
	if (n.ptr == NULL)
	{	/* if we have zero in "value", gmp returns NULL */
		n.len = 0;
	}
	return n;
}

/**
 * Convert a MP integer into a DER coded ASN.1 object
 */
chunk_t gmp_mpz_to_asn1(const mpz_t value)
{
	return asn1_wrap(ASN1_INTEGER, "m", gmp_mpz_to_chunk(value));
}

/**
 * Implementation of gmp_rsa_private_key.get_public_key.
 */
static public_key_t* get_public_key(private_gmp_rsa_private_key_t *this)
{
	chunk_t n, e;
	public_key_t *public;
	
	n = gmp_mpz_to_chunk(this->n);
	e = gmp_mpz_to_chunk(this->e);
	
	public = lib->creds->create(lib->creds, CRED_PUBLIC_KEY, KEY_RSA,
						BUILD_RSA_MODULUS, n, BUILD_RSA_PUB_EXP, e, BUILD_END);
	chunk_free(&n);
	chunk_free(&e);
	
	return public;
}

/**
 * Implementation of gmp_rsa_private_key.equals.
 */
static bool equals(private_gmp_rsa_private_key_t *this, private_key_t *other)
{
	identification_t *keyid;

	if (&this->public.interface == other)
	{
		return TRUE;
	}
	if (other->get_type(other) != KEY_RSA)
	{
		return FALSE;
	}
	keyid = other->get_id(other, ID_PUBKEY_SHA1);
	if (keyid && keyid->equals(keyid, this->keyid))
	{
		return TRUE;
	}
	keyid = other->get_id(other, ID_PUBKEY_INFO_SHA1);
	if (keyid && keyid->equals(keyid, this->keyid_info))
	{
		return TRUE;
	}
	return FALSE;
}

/**
 * Implementation of gmp_rsa_private_key.belongs_to.
 */
static bool belongs_to(private_gmp_rsa_private_key_t *this, public_key_t *public)
{
	identification_t *keyid;

	if (public->get_type(public) != KEY_RSA)
	{
		return FALSE;
	}
	keyid = public->get_id(public, ID_PUBKEY_SHA1);
	if (keyid && keyid->equals(keyid, this->keyid))
	{
		return TRUE;
	}
	keyid = public->get_id(public, ID_PUBKEY_INFO_SHA1);
	if (keyid && keyid->equals(keyid, this->keyid_info))
	{
		return TRUE;
	}
	return FALSE;
}

/**
 * Implementation of private_key_t.get_encoding.
 */
static chunk_t get_encoding(private_gmp_rsa_private_key_t *this)
{
	return asn1_wrap(ASN1_SEQUENCE, "cmmmmmmmm",
					 ASN1_INTEGER_0,
					 gmp_mpz_to_asn1(this->n),
					 gmp_mpz_to_asn1(this->e),
					 gmp_mpz_to_asn1(this->d),
					 gmp_mpz_to_asn1(this->p),
					 gmp_mpz_to_asn1(this->q),
					 gmp_mpz_to_asn1(this->exp1),
					 gmp_mpz_to_asn1(this->exp2),
					 gmp_mpz_to_asn1(this->coeff));
}

/**
 * Implementation of gmp_rsa_private_key.get_ref.
 */
static private_gmp_rsa_private_key_t* get_ref(private_gmp_rsa_private_key_t *this)
{
	ref_get(&this->ref);
	return this;

}

/**
 * Implementation of gmp_rsa_private_key.destroy.
 */
static void destroy(private_gmp_rsa_private_key_t *this)
{
	if (ref_put(&this->ref))
	{
		mpz_clear_randomized(this->n);
		mpz_clear_randomized(this->e);
		mpz_clear_randomized(this->p);
		mpz_clear_randomized(this->q);
		mpz_clear_randomized(this->d);
		mpz_clear_randomized(this->exp1);
		mpz_clear_randomized(this->exp2);
		mpz_clear_randomized(this->coeff);
		DESTROY_IF(this->keyid);
		DESTROY_IF(this->keyid_info);
		free(this);
	}
}

/**
 * Check the loaded key if it is valid and usable
 */
static status_t check(private_gmp_rsa_private_key_t *this)
{
	mpz_t t, u, q1;
	status_t status = SUCCESS;
	
	/* PKCS#1 1.5 section 6 requires modulus to have at least 12 octets.
	 * We actually require more (for security).
	 */
	if (this->k < 512 / BITS_PER_BYTE)
	{
		DBG1("key shorter than 512 bits");
		return FAILED;
	}
	
	/* we picked a max modulus size to simplify buffer allocation */
	if (this->k > 8192 / BITS_PER_BYTE)
	{
		DBG1("key larger than 8192 bits");
		return FAILED;
	}
	
	mpz_init(t);
	mpz_init(u);
	mpz_init(q1);
	
	/* check that n == p * q */
	mpz_mul(u, this->p, this->q);
	if (mpz_cmp(u, this->n) != 0)
	{
		status = FAILED;
	}
	
	/* check that e divides neither p-1 nor q-1 */
	mpz_sub_ui(t, this->p, 1);
	mpz_mod(t, t, this->e);
	if (mpz_cmp_ui(t, 0) == 0)
	{
		status = FAILED;
	}
	
	mpz_sub_ui(t, this->q, 1);
	mpz_mod(t, t, this->e);
	if (mpz_cmp_ui(t, 0) == 0)
	{
		status = FAILED;
	}
	
	/* check that d is e^-1 (mod lcm(p-1, q-1)) */
	/* see PKCS#1v2, aka RFC 2437, for the "lcm" */
	mpz_sub_ui(q1, this->q, 1);
	mpz_sub_ui(u, this->p, 1);
	mpz_gcd(t, u, q1);		/* t := gcd(p-1, q-1) */
	mpz_mul(u, u, q1);		/* u := (p-1) * (q-1) */
	mpz_divexact(u, u, t);	/* u := lcm(p-1, q-1) */
	
	mpz_mul(t, this->d, this->e);
	mpz_mod(t, t, u);
	if (mpz_cmp_ui(t, 1) != 0)
	{
		status = FAILED;
	}
	
	/* check that exp1 is d mod (p-1) */
	mpz_sub_ui(u, this->p, 1);
	mpz_mod(t, this->d, u);
	if (mpz_cmp(t, this->exp1) != 0)
	{
		status = FAILED;
	}
	
	/* check that exp2 is d mod (q-1) */
	mpz_sub_ui(u, this->q, 1);
	mpz_mod(t, this->d, u);
	if (mpz_cmp(t, this->exp2) != 0)
	{
		status = FAILED;
	}
	
	/* check that coeff is (q^-1) mod p */
	mpz_mul(t, this->coeff, this->q);
	mpz_mod(t, t, this->p);
	if (mpz_cmp_ui(t, 1) != 0)
	{
		status = FAILED;
	}
	
	mpz_clear_randomized(t);
	mpz_clear_randomized(u);
	mpz_clear_randomized(q1);
	if (status != SUCCESS)
	{
		DBG1("key integrity tests failed");
	}
	return status;
}

/**
 * Internal generic constructor
 */
static private_gmp_rsa_private_key_t *gmp_rsa_private_key_create_empty(void)
{
	private_gmp_rsa_private_key_t *this = malloc_thing(private_gmp_rsa_private_key_t);
	
	this->public.interface.get_type = (key_type_t (*) (private_key_t*))get_type;
	this->public.interface.sign = (bool (*) (private_key_t*, signature_scheme_t, chunk_t, chunk_t*))sign;
	this->public.interface.decrypt = (bool (*) (private_key_t*, chunk_t, chunk_t*))decrypt;
	this->public.interface.get_keysize = (size_t (*) (private_key_t*))get_keysize;
	this->public.interface.get_id = (identification_t* (*) (private_key_t*, id_type_t))get_id;
	this->public.interface.get_public_key = (public_key_t* (*) (private_key_t*))get_public_key;
	this->public.interface.equals = (bool (*) (private_key_t*, private_key_t*))equals;
	this->public.interface.belongs_to = (bool (*) (private_key_t*, public_key_t*))belongs_to;
	this->public.interface.get_encoding = (chunk_t (*) (private_key_t*))get_encoding;
	this->public.interface.get_ref = (private_key_t* (*) (private_key_t*))get_ref;
	this->public.interface.destroy = (void (*) (private_key_t*))destroy;
	
	this->keyid = NULL;
	this->keyid_info = NULL;
	this->ref = 1;
	
	return this;
}

/**
 * Generate an RSA key of specified key size
 */
static gmp_rsa_private_key_t *generate(size_t key_size)
{
	mpz_t p, q, n, e, d, exp1, exp2, coeff;
	mpz_t m, q1, t;
	private_gmp_rsa_private_key_t *this = gmp_rsa_private_key_create_empty();
	
	key_size = key_size / BITS_PER_BYTE;
	
	/* Get values of primes p and q  */
	if (compute_prime(this, key_size/2, &p) != SUCCESS)
	{
		free(this);
		return NULL;
	}	
	if (compute_prime(this, key_size/2, &q) != SUCCESS)
	{
		mpz_clear(p);
		free(this);
		return NULL;
	}
	
	mpz_init(t);
	mpz_init(n);
	mpz_init(d);
	mpz_init(exp1);
	mpz_init(exp2);
	mpz_init(coeff);
	
	/* Swapping Primes so p is larger then q */
	if (mpz_cmp(p, q) < 0)
	{
		mpz_swap(p, q);
	}
	
	mpz_mul(n, p, q);						/* n = p*q */
	mpz_init_set_ui(e, PUBLIC_EXPONENT);	/* assign public exponent */
	mpz_init_set(m, p); 					/* m = p */
	mpz_sub_ui(m, m, 1);					/* m = m -1 */
	mpz_init_set(q1, q);					/* q1 = q */
	mpz_sub_ui(q1, q1, 1);					/* q1 = q1 -1 */
	mpz_gcd(t, m, q1);						/* t = gcd(p-1, q-1) */
	mpz_mul(m, m, q1);						/* m = (p-1)*(q-1) */
	mpz_divexact(m, m, t);					/* m = m / t */
	mpz_gcd(t, m, e);						/* t = gcd(m, e) */

	mpz_invert(d, e, m);					/* e has an inverse mod m */
	if (mpz_cmp_ui(d, 0) < 0)				/* make sure d is positive */
	{
		mpz_add(d, d, m);
	}
	mpz_sub_ui(t, p, 1);					/* t = p-1 */
	mpz_mod(exp1, d, t);					/* exp1 = d mod p-1 */
	mpz_sub_ui(t, q, 1);					/* t = q-1 */
	mpz_mod(exp2, d, t);					/* exp2 = d mod q-1 */
	
	mpz_invert(coeff, q, p);				/* coeff = q^-1 mod p */
	if (mpz_cmp_ui(coeff, 0) < 0)			/* make coeff d is positive */
	{
		mpz_add(coeff, coeff, p);
	}

	mpz_clear_randomized(q1);
	mpz_clear_randomized(m);
	mpz_clear_randomized(t);

	/* apply values */
	*(this->p) = *p;
	*(this->q) = *q;
	*(this->n) = *n;
	*(this->e) = *e;
	*(this->d) = *d;
	*(this->exp1) = *exp1;
	*(this->exp2) = *exp2;
	*(this->coeff) = *coeff;
	
	/* set key size in bytes */
	this->k = key_size;
	
	return &this->public;
}

/**
 * load private key from a RSA components
 */
static gmp_rsa_private_key_t *load(chunk_t n, chunk_t e, chunk_t d,
				chunk_t p, chunk_t q, chunk_t exp1, chunk_t exp2, chunk_t coeff)
{
	private_gmp_rsa_private_key_t *this = gmp_rsa_private_key_create_empty();
	
	mpz_init(this->n);
	mpz_init(this->e);
	mpz_init(this->p);
	mpz_init(this->q);
	mpz_init(this->d);
	mpz_init(this->exp1);
	mpz_init(this->exp2);
	mpz_init(this->coeff);
	
	mpz_import(this->n, n.len, 1, 1, 1, 0, n.ptr);
	mpz_import(this->e, e.len, 1, 1, 1, 0, e.ptr);
	mpz_import(this->d, d.len, 1, 1, 1, 0, d.ptr);
	mpz_import(this->p, p.len, 1, 1, 1, 0, p.ptr);
	mpz_import(this->q, q.len, 1, 1, 1, 0, q.ptr);
	mpz_import(this->coeff, coeff.len, 1, 1, 1, 0, coeff.ptr);
	if (!exp1.len)
	{	/* exp1 missing in key, recalculate: exp1 = d mod (p-1) */
		mpz_sub_ui(this->exp1, this->p, 1);
		mpz_mod(this->exp1, this->d, this->exp1);
	}
	else
	{
		mpz_import(this->exp1, exp1.len, 1, 1, 1, 0, exp1.ptr);
	}
	if (!exp2.len)
	{	/* exp2 missing in key, recalculate: exp2 = d mod (q-1) */
		mpz_sub_ui(this->exp2, this->q, 1);
		mpz_mod(this->exp2, this->d, this->exp2);
	}
	else
	{
		mpz_import(this->exp2, exp2.len, 1, 1, 1, 0, exp2.ptr);
	}
	this->k = (mpz_sizeinbase(this->n, 2) + 7) / BITS_PER_BYTE;
	if (!gmp_rsa_public_key_build_id(this->n, this->e,
									 &this->keyid, &this->keyid_info))
	{
		destroy(this);
		return NULL;
	}
	if (check(this) != SUCCESS)
	{
		destroy(this);
		return NULL;
	}
	return &this->public;
}

typedef struct private_builder_t private_builder_t;
/**
 * Builder implementation for key loading/generation
 */
struct private_builder_t {
	/** implements the builder interface */
	builder_t public;
	/** key size, if generating */
	u_int key_size;
	/** rsa key parameters */
	chunk_t n, e, d, p, q, exp1, exp2, coeff;
};

/**
 * Implementation of builder_t.build
 */
static gmp_rsa_private_key_t *build(private_builder_t *this)
{
	gmp_rsa_private_key_t *key = NULL;
	
	if (this->key_size)
	{
		key = generate(this->key_size);
	}
	else
	{
		key = load(this->n, this->e, this->d, this->p, this->q,
				   this->exp1, this->exp2, this->coeff);
	}
	free(this);
	return key;
}

/**
 * Implementation of builder_t.add
 */
static void add(private_builder_t *this, builder_part_t part, ...)
{
	va_list args;
	
	va_start(args, part);
	switch (part)
	{
		case BUILD_KEY_SIZE:
			this->key_size = va_arg(args, u_int);
			return;
		case BUILD_RSA_MODULUS:
			this->n = va_arg(args, chunk_t);
			break;
		case BUILD_RSA_PUB_EXP:
			this->e = va_arg(args, chunk_t);
			break;
		case BUILD_RSA_PRIV_EXP:
			this->d = va_arg(args, chunk_t);
			break;
		case BUILD_RSA_PRIME1:
			this->p = va_arg(args, chunk_t);
			break;
		case BUILD_RSA_PRIME2:
			this->q = va_arg(args, chunk_t);
			break;
		case BUILD_RSA_EXP1:
			this->exp1 = va_arg(args, chunk_t);
			break;
		case BUILD_RSA_EXP2:
			this->exp1 = va_arg(args, chunk_t);
			break;
		case BUILD_RSA_COEFF:
			this->coeff = va_arg(args, chunk_t);
			break;
		default:
			builder_cancel(&this->public);
			break;
	}
	va_end(args);
}

/**
 * Builder construction function
 */
builder_t *gmp_rsa_private_key_builder(key_type_t type)
{
	private_builder_t *this;
	
	if (type != KEY_RSA)
	{
		return NULL;
	}
	
	this = malloc_thing(private_builder_t);
	
	this->n = this->e = this->d = this->p = this->q = chunk_empty;
	this->exp1 = this->exp2 = this->coeff = chunk_empty;
	this->key_size = 0;
	this->public.add = (void(*)(builder_t *this, builder_part_t part, ...))add;
	this->public.build = (void*(*)(builder_t *this))build;
	
	return &this->public;
}

